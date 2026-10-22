#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
用产品固件自己的 BLE 日志通道做复现。

产品固件（日志走 UART0，USB 被 HID 键盘占着）本来有一条自带的调试通道：
    主机发 "LOG:on"  → 固件把 LOG_PORT 的每一行按行推回来（LOG:<text>）
    主机发 "LOG:dump" → 把 PSRAM 里 8KB 的历史环形缓冲一次性拉回来
所以不接 USB-TTL、不改固件，也能看到固件到底发生了什么。

本脚本复现用户报的两个症状：
    ① "HTML 连接上就崩"  → 网页连上会自动拉整份配置（CFGGET），这里照做
    ② "发送 ME 就崩"    → 网页下发 ME_START / ME_DATA / ME_END，这里照做

用法：
    /usr/bin/python3 tools/prod_repro.py
    /usr/bin/python3 tools/prod_repro.py --only me
"""

import asyncio
import re
import sys
import time

from bleak import BleakClient, BleakScanner

SERVICE_UUID = "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
CHAR_UUID = "beb5483e-36e1-4688-b7f5-ea07361b26a8"


class BtLink:
    """和网页里那套 BtLink 一样的重组规则。"""

    def __init__(self):
        self._line = ""
        self._buf = {}
        self._expect = {}
        self.done = []

    def feed(self, _sender, data):
        try:
            self._line += bytes(data).decode("utf-8", "replace")
        except Exception:
            return
        parts = self._line.split("\r")
        self._line = parts.pop()
        for raw in parts:
            self._on_line(raw)

    def _on_line(self, line):
        if not line:
            return
        c1 = line.find(":")
        if c1 <= 0:
            return
        c2 = line.find(":", c1 + 1)
        if c2 < 0:
            return
        verb, key, body = line[:c1], line[c1 + 1:c2], line[c2 + 1:]
        ident = verb + "|" + key
        if body == "END":
            full = self._buf.pop(ident, "")
            exp = self._expect.pop(ident, None)
            self.done.append((verb, key, full, exp, len(full.encode("utf-8"))))
            return
        m = re.match(r"^LEN:(\d+)([\s\S]*)$", body)
        if m and (m.group(2) == "" or m.group(2)[0] == "{"):
            self._expect[ident] = int(m.group(1))
            self._buf[ident] = m.group(2)
            return
        self._buf[ident] = self._buf.get(ident, "") + body

    def take(self, verb):
        out = [d for d in self.done if d[0] == verb]
        self.done = [d for d in self.done if d[0] != verb]
        return out


async def find_keyboard(timeout=6.0):
    res = await BleakScanner.discover(timeout=timeout)
    addrs = [d.address for d in res] if isinstance(res, list) else list(res)
    print("扫描到 %d 个候选，逐个探测 GATT…" % len(addrs), flush=True)
    sem = asyncio.Semaphore(8)

    async def probe(a):
        async with sem:
            try:
                async with BleakClient(a, timeout=5) as c:
                    for s in c.services:
                        if SERVICE_UUID.lower() in str(s.uuid).lower():
                            return a
            except Exception:
                pass
            return None

    hits = [h for h in await asyncio.gather(
        *[probe(a) for a in addrs], return_exceptions=True)
        if h and not isinstance(h, Exception)]
    return hits[0] if hits else None


async def send(c, cmd, wait=0.5):
    try:
        await c.write_gatt_char(CHAR_UUID, (cmd + "\n").encode(), response=True)
    except Exception as e:
        print("  写失败(%s): %r" % (cmd, e))
        return False
    await asyncio.sleep(wait)
    return True


async def main():
    argv = sys.argv
    only = argv[argv.index("--only") + 1] if "--only" in argv else "all"

    addr = await find_keyboard()
    if not addr:
        print("✗ 没找到 BLE 设备")
        return 1
    print("✓ 设备 %s\n" % addr)

    bt = BtLink()
    logs = []

    def on_notify(_s, d):
        txt = bytes(d).decode("utf-8", "replace")
        logs.append(txt)
        # ★ 必须喂给 BtLink，否则 CFGDUMP/LOGDUMP 的分片永远不会拼起来
        bt.feed(None, d)
        # 固件日志行长这样：LOG:<正文>
        if txt.startswith("LOG:"):
            print("   [固件日志] " + txt[4:].rstrip("\r"))
        else:
            # 其余一律原样打出来（CFGDUMP / CFGERR / LOGDUMP 的分片都在这里）
            print("   [通知] " + (txt[:160].rstrip("\r")))

    async with BleakClient(addr, timeout=10) as c:
        await c.start_notify(CHAR_UUID, on_notify)
        await asyncio.sleep(0.5)
        print("=== 打开固件日志通道（LOG:on）===")
        await send(c, "LOG:on", 1.0)

        if only in ("all", "cfg"):
            print("\n=== ① 复现「HTML 连接上」：网页连上会自动拉整份配置 ===")
            bt.done.clear()
            t0 = time.time()
            await send(c, "CFGGET", 0.3)
            # 等收尾标记，最多 30 秒
            got = None
            for _ in range(60):
                await asyncio.sleep(0.5)
                g = bt.take("CFGDUMP")
                if g:
                    got = g[0]
                    break
            if got:
                v, k, body, exp, n = got
                print("  CFGDUMP 收到 %d 字节（声明 %s），耗时 %.1fs" % (n, exp, time.time() - t0))
                if exp is not None and n != exp:
                    print("  ★ 丢片：声明 %s，实收 %d" % (exp, n))
                else:
                    print("  ✓ 整份配置完整回来了")
            else:
                print("  ✗ 30 秒内没收到 CFGDUMP 的收尾标记")
            print("  固件还活着吗？", end=" ")
            await send(c, "PING", 1.0)   # 随便发一条，看固件还回不回
            print("见下面的固件日志")

        if only in ("all", "me"):
            print("\n=== ② 复现「发送 ME」：网页下发 ME_START / ME_DATA / ME_END ===")
            text = "你好，这是一段用来测试 ME 文本下发的中文内容。" * 3
            view = text.encode("utf-8")
            hexstr = "".join("%02X" % b for b in view)
            print("  文本 %d 字符 → hex %d 字节 → %d 片 ME_DATA"
                  % (len(text), len(hexstr), (len(hexstr) + 127) // 128))
            await send(c, "ME_START", 0.4)
            for i in range(0, len(hexstr), 128):
                ok = await send(c, "ME_DATA:" + hexstr[i:i + 128], 0.12)
                if not ok:
                    print("  ★ 第 %d 片写失败" % (i // 128))
                    break
            await send(c, "ME_END", 1.5)
            print("  下发完毕，等固件反应…")
            await asyncio.sleep(3)

        print("\n=== ③ 拉固件日志历史（LOG:dump）===")
        bt.done.clear()
        await send(c, "LOG:dump", 0.3)
        for _ in range(20):
            await asyncio.sleep(0.5)
            g = bt.take("LOGDUMP")
            if g:
                body = g[0][2]
                print(body[-3000:] if len(body) > 3000 else body)
                break
        else:
            print("  ✗ 没拉到 LOGDUMP")

    print("\n================= 收到的通知总数: %d =================" % len(logs))
    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
