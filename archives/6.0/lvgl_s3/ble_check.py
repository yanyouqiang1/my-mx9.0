#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
用 BLE 连上键盘，确认固件活着，并切到指定的主屏风格。

BLE 协议（src/bt_link.cpp）：
  · 服务 4fafc201-1fb5-459e-8fcc-c5c9c331914b
  · 特征 beb5483e-36e1-4688-b7f5-ea07361b26a8（WRITE + NOTIFY）
  · 命令是**以 \\n 结尾的纯文本**，一行一条
  · 固件用 NOTIFY 回 CFGDUMP 分片，格式：
        CFGDUMP:<key>:LEN:<总字节数><json 第一片>
        CFGDUMP:<key>:<json 续片>
        CFGDUMP:<key>:END

⚠⚠ macOS 的 CoreBluetooth 后端有两个坑，这脚本就是为绕开它们写的：

1. **不暴露**未连接设备的广播名，find_device_by_name 扫不到 ——
   只能挨个连、看 GATT 里有没有本机的 service。

2. **千万别并发探测**。NimBLE 只有**一个**连接槽：8 个并发连接同时打到
   同一台设备，只有一个能连上，其余的连接请求会把它的连接状态搅乱；
   等那个"唯一成功"的探测连接一断开，槽位还没释放，紧接着再连就会
   **连上即掉**（macOS 后端在断连后会把服务缓存清掉 → 下一次写特征直接报
   "Service Discovery has not been performed yet"）。
   所以这里：已知地址直连优先 → 失败才扫描，且**串行**探测、每个之间歇一下。

用法：
    /usr/bin/python3 ble_check.py                       # 只连上并读回状态
    /usr/bin/python3 ble_check.py --mode 7              # 顺便切到第 7 种风格
    /usr/bin/python3 ble_check.py --mode 7 --rebuild 6  # 再来回重建 6 轮测稳定性
    /usr/bin/python3 ble_check.py --addr AA:BB:...      # 指定设备地址
"""

import asyncio
import json
import re
import sys

from bleak import BleakClient, BleakScanner

SERVICE_UUID = "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
CHAR_UUID = "beb5483e-36e1-4688-b7f5-ea07361b26a8"
DEV_NAME = "YYQ-MX9.0"
# 上次实测到的地址。macOS 扫不到广播名，全量扫描又会把设备连接搅乱，
# 所以默认直接连它 —— 连不上再退回去扫描。
KNOWN_ADDR = "7810793C-EBA3-5930-27EF-418615106A20"


class CfgDump:
    """把 CFGDUMP 的分片拼回完整 JSON。"""

    def __init__(self):
        self.buf = ""
        self.done = []
        self.raw = []          # 收到的原始包，调试用
        self.expect = None     # LEN 行里说的总字节数

    def feed(self, text):
        self.raw.append(text)
        # 头是 CFGDUMP:<key>:，key 是请求号（纯数字）
        m = re.match(r"^CFGDUMP:\d+:(.*)$", text, re.S)
        if not m:
            return
        body = m.group(1)
        if body.startswith("LEN:"):
            # "LEN:<总字节数>" 和 json 第一片是**粘在一起**的
            m2 = re.match(r"^LEN:(\d+)", body)
            if m2:
                self.expect = int(m2.group(1))
            body = re.sub(r"^LEN:\d+", "", body)
        if body.strip() in (":END", "END"):
            if self.buf:
                self.done.append(self.buf)
            self.buf = ""
            self.expect = None
            return
        self.buf += body

    def last(self):
        return self.done[-1] if self.done else None


class Link:
    """一条 BLE 连接，带断线自动重连。"""

    def __init__(self, addr, dump):
        self.addr = addr
        self.dump = dump
        self.client = None
        self.notifying = False

    async def open(self, tries=4):
        last = None
        for i in range(tries):
            try:
                c = BleakClient(self.addr, timeout=12)
                await c.connect()
                # 等服务发现真正完成（macOS 后端偶尔会晚一拍）
                for _ in range(60):
                    try:
                        if c.services:
                            break
                    except Exception:
                        pass
                    await asyncio.sleep(0.1)
                if not c.services:
                    raise RuntimeError("服务发现超时")
                self.client = c
                try:
                    await c.start_notify(
                        CHAR_UUID,
                        lambda s, x: self.dump.feed(bytes(x).decode("utf-8", "replace")))
                    self.notifying = True
                except Exception as e:
                    print("⚠ 订阅通知失败（还能写，只是读不回配置）: %s" % e, flush=True)
                print("✓ 已连接 %s（服务发现完成）" % self.addr, flush=True)
                return True
            except Exception as e:
                last = e
                print("  连接尝试 %d 失败: %s" % (i + 1, e), flush=True)
                try:
                    await c.disconnect()
                except Exception:
                    pass
                # 断连后别马上重连 —— NimBLE 的连接槽还没释放，
                # 硬连就是"连上即掉"。歇 2 秒再试。
                await asyncio.sleep(2.0)
        print("✗ 连不上 %s：%s" % (self.addr, last), flush=True)
        return False

    async def close(self):
        if self.client is not None:
            try:
                if self.notifying:
                    await self.client.stop_notify(CHAR_UUID)
                await self.client.disconnect()
            except Exception:
                pass
            self.client = None

    async def send(self, cmd, wait=1.0):
        """发一条命令。断了就重连重发一次。"""
        for attempt in (0, 1):
            try:
                await self.client.write_gatt_char(
                    CHAR_UUID, (cmd + "\n").encode("utf-8"), response=True)
                await asyncio.sleep(wait)
                return True
            except Exception as e:
                print("  发送 %r 失败（第 %d 次）: %s" % (cmd, attempt + 1, e), flush=True)
                if attempt == 0:
                    await self.close()
                    if not await self.open():
                        return False
        return False


async def scan_for_keyboard():
    """兜底：扫描 + **串行**探测（并发会把设备的连接槽搅乱）。"""
    res = await BleakScanner.discover(timeout=6.0)
    addrs = [d.address for d in res] if isinstance(res, list) else list(res)
    print("扫描到 %d 个设备，逐个串行探测…" % len(addrs), flush=True)
    for i, addr in enumerate(addrs):
        try:
            c = BleakClient(addr, timeout=8)
            await c.connect()
            ok = False
            for s in c.services:
                if SERVICE_UUID.lower() in str(s.uuid).lower():
                    ok = True
                    break
            await c.disconnect()
            if ok:
                print("✓ 找到键盘: %s" % addr, flush=True)
                return addr
        except Exception:
            pass
        # 每个之间歇一下，别把设备的连接槽打爆
        await asyncio.sleep(0.4)
        if i % 10 == 9:
            print("  …已探测 %d/%d" % (i + 1, len(addrs)), flush=True)
    return None


async def main():
    args = sys.argv
    want_mode = int(args[args.index("--mode") + 1]) if "--mode" in args else None
    rebuild = int(args[args.index("--rebuild") + 1]) if "--rebuild" in args else 0
    addr = args[args.index("--addr") + 1] if "--addr" in args else KNOWN_ADDR

    dump = CfgDump()
    link = Link(addr, dump)
    if not await link.open():
        print("  直连 %s 失败，改用扫描…" % addr, flush=True)
        found = await scan_for_keyboard()
        if not found:
            print("✗ 没找到带 YYQ service 的 BLE 设备")
            return 1
        link = Link(found, dump)
        if not await link.open():
            return 1

    async def cfg():
        dump.done.clear()
        await link.send("CFGGET", 2.0)
        raw = dump.last()
        if raw is None:
            return None
        try:
            return json.loads(raw)
        except Exception as e:
            print("  配置 JSON 解析失败: %s（前 120 字: %r）" % (e, raw[:120]), flush=True)
            return None

    first = await cfg()
    print("起始状态: disp_mode=%s curr_prof=%s" % (
        first["disp_mode"] if first else None,
        first["curr_prof"] if first else None), flush=True)

    if want_mode is not None:
        await link.send("DISP_MODE:%d" % want_mode, 1.0)
        j = await cfg()
        print("✓ 已切到 disp_mode=%s" % (j["disp_mode"] if j else None), flush=True)

    # 主屏重建压力测试：来回切风格，每次都是一次完整的
    # 「删旧容器 → resetStylePointers → build_style_plankey」。
    # 这条路径历史上是「切风格就崩」的重灾区，必须盯住设备还活着。
    for i in range(rebuild):
        await link.send("DISP_MODE:%d" % (6 if i % 2 == 0 else 7), 0.9)
        j = await cfg()
        print("  重建轮 %d → disp_mode=%s %s" % (
            i + 1, j["disp_mode"] if j else None,
            "✓" if j else "⚠ 无响应"), flush=True)

    if want_mode is not None:
        await link.send("DISP_MODE:%d" % want_mode, 1.0)

    last = await cfg()
    await link.close()
    if last is None:
        print("✗ 设备没响应了")
        return 1
    print("✓ 设备仍响应  disp_mode=%s curr_prof=%s saver_mode=%s" % (
        last["disp_mode"], last["curr_prof"], last["saver_mode"]))
    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
