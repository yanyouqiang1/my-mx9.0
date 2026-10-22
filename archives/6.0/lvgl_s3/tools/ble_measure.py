#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
BLE 链路层实测 —— 对着 ble_probe 固件把事实量出来。

每一项都对应一个待验证的猜想：

  A. 首次连接：conn_id 是几？协商后 MTU 几？预算算出几？
  B. 刚好等于预算的通知：收得到吗？
  C. 故意超 MTU 的通知：收得到吗？（库的 notify() 日志说"Truncating"但其实没截断）
  D. 20KB 流式回拉：固件声明的总长 vs 主机实收字节数 = 丢片率
  E. **断线后多久能重新被扫到**（产品固件是"阻塞 500ms 后只广播一次"）
  F. 网页式 ME 文本下发：会不会丢指令
  G. 远程重启后 conn_id / MTU 是什么样

用法：
    /usr/bin/python3 tools/ble_measure.py
    /usr/bin/python3 tools/ble_measure.py --skip-flood
"""

import asyncio
import re
import sys
import time

from bleak import BleakClient, BleakScanner

SERVICE_UUID = "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
CHAR_UUID = "beb5483e-36e1-4688-b7f5-ea07361b26a8"


# ----------------------------------------------------------------- 协议解析
class BtLink:
    """固件 → 主机方向的重组，和网页里的 BtLink 是同一套规则。"""

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


# ----------------------------------------------------------------- 连接helper
async def find_keyboard(timeout=6.0):
    res = await BleakScanner.discover(timeout=timeout)
    addrs = [d.address for d in res] if isinstance(res, list) else list(res)
    print("扫描到 %d 个候选，逐个探测 GATT…" % len(addrs), flush=True)
    sem = asyncio.Semaphore(8)

    async def probe(a):
        async with sem:
            try:
                async with BleakClient(a, timeout=6) as c:
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


async def send(c, cmd, wait=0.6):
    await c.write_gatt_char(CHAR_UUID, (cmd + "\n").encode(), response=True)
    await asyncio.sleep(wait)


async def try_connect(addr, timeout=5):
    """试着连一下：连得上 = 设备还在广播。"""
    try:
        c = BleakClient(addr, timeout=timeout)
        await c.connect()
        return c
    except Exception:
        return None


# ----------------------------------------------------------------- 主流程
async def main():
    skip_flood = "--skip-flood" in sys.argv
    addr = await find_keyboard()
    if not addr:
        print("✗ 没找到 BLE 设备")
        return 1
    print("✓ 设备: %s\n" % addr)

    bt = BtLink()
    report = {}

    # ---------------- A/B/C/D ----------------
    c = await try_connect(addr)
    if c is None:
        print("✗ 连不上")
        return 1
    try:
        await c.start_notify(CHAR_UUID, bt.feed)
        await asyncio.sleep(0.8)

        print("=== A. 首次连接（诊断在串口上，这里量通知本身）===")
        await send(c, "PROBE:DUMP", 0.8)

        print("\n=== B. 刚好等于预算的通知 ===")
        bt.done.clear()
        await send(c, "PROBE:MTU", 1.5)
        got = bt.take("PROBEMTU")
        if got:
            v, k, body, exp, n = got[0]
            print("  正文 %d 字节（声明 %s）→ %s" %
                  (n, exp, "收到且长度一致" if (exp is None or n == exp) else "★ 长度对不上"))
            report["B_预算内通知字节"] = n
        else:
            print("  ✗ 没收到")
            report["B_预算内通知字节"] = -1

        print("\n=== C. 故意超 MTU 的通知（PROBE:BIG:250）===")
        bt.done.clear()
        await send(c, "PROBE:BIG:250", 2.0)
        got = bt.take("PROBEBIG")
        if got:
            v, k, body, exp, n = got[0]
            verdict = "完整" if n == 250 else ("★ 被截断" if n < 250 else "★ 超了")
            print("  请求 250 字节，实收 %d 字节 → %s" % (n, verdict))
            report["C_超MTU通知实收"] = n
        else:
            print("  ✗ 完全没收到（连收尾标记都没有）→ 整条被协议栈丢了")
            report["C_超MTU通知实收"] = -1

        print("\n=== D. 20KB 流式回拉（PROBE:STREAM:20480）===")
        bt.done.clear()
        t0 = time.time()
        await send(c, "PROBE:STREAM:20480", 14.0)
        got = bt.take("PROBESTREAM")
        if got:
            v, k, body, exp, n = got[0]
            if exp and exp != n:
                print("  固件声明 %d 字节，主机实收 %d 字节，耗时 %.1fs → ★ 丢了 %d 字节 (%.1f%%)"
                      % (exp, n, time.time() - t0, exp - n, 100.0 * (exp - n) / exp))
            else:
                print("  固件声明 %s 字节，主机实收 %d 字节，耗时 %.1fs → 完整"
                      % (exp, n, time.time() - t0))
            report["D_流式声明"] = exp
            report["D_流式实收"] = n
        else:
            print("  ✗ 没收到 PROBESTREAM")
            report["D_流式实收"] = -1
    finally:
        try:
            await c.stop_notify(CHAR_UUID)
            await c.disconnect()
        except Exception:
            pass

    # ---------------- E. 断线后重新广播 ----------------
    print("\n=== E. 断线后多久能重新被扫到 ===")
    for delay_ms, retries in [(0, 1), (500, 1), (500, 5), (2000, 1)]:
        c = await try_connect(addr)
        if c is None:
            print("  (delay=%d,retry=%d) 连不上，跳过" % (delay_ms, retries))
            continue
        bt4 = BtLink()
        try:
            await c.start_notify(CHAR_UUID, bt4.feed)
            await asyncio.sleep(0.4)
            await send(c, "PROBE:ADVDELAY:%d" % delay_ms, 0.35)
            await send(c, "PROBE:ADVRETRY:%d" % retries, 0.35)
        except Exception as e:
            print("  设置失败: %r" % (e,))
        try:
            await c.disconnect()
        except Exception:
            pass
        t0 = time.time()
        back = None
        while time.time() - t0 < 12:
            await asyncio.sleep(0.5)
            cc = await try_connect(addr, timeout=3)
            if cc is not None:
                back = time.time() - t0
                try:
                    await cc.disconnect()
                except Exception:
                    pass
                break
        if back is None:
            print("  delay=%4dms retry=%d → ★ 12 秒内都没重新出现（设备彻底不可见）" %
                  (delay_ms, retries))
            report["E_%dms_%d次" % (delay_ms, retries)] = "回不来"
        else:
            print("  delay=%4dms retry=%d → %.1fs 后重新可连" % (delay_ms, retries, back))
            report["E_%dms_%d次" % (delay_ms, retries)] = "%.1fs" % back
        await asyncio.sleep(1.0)

    # ---------------- F. 网页式 ME 文本下发 ----------------
    if not skip_flood:
        print("\n=== F. 网页式 ME 文本下发（ME_START + 20 片 ME_DATA + ME_END）===")
        c = await try_connect(addr)
        if c is not None:
            bt5 = BtLink()
            try:
                await c.start_notify(CHAR_UUID, bt5.feed)
                await asyncio.sleep(0.4)
                await send(c, "ME_START", 0.4)
                for i in range(20):
                    await send(c, "ME_DATA:" + ("%02X" % (i % 256)) * 64, 0.15)
                await send(c, "ME_END", 1.0)
                echos = [d for d in bt5.done if d[0] == "PROBEECHO"]
                print("  固件回显 %d / 22 条 → %s" %
                      (len(echos), "全收到" if len(echos) == 22 else "★ 丢了 %d 条" % (22 - len(echos))))
                report["F_ME回显"] = "%d/22" % len(echos)
            except Exception as e:
                print("  失败: %r" % (e,))
            try:
                await c.disconnect()
            except Exception:
                pass

    # ---------------- G. 远程重启 ----------------
    print("\n=== G. 远程重启（PROBE:RESET）===")
    c = await try_connect(addr)
    if c is not None:
        try:
            await c.write_gatt_char(CHAR_UUID, b"PROBE:RESET\n", response=True)
            print("  已发 PROBE:RESET，等设备掉线…")
            await asyncio.sleep(4)
        except Exception as e:
            print("  连接断开（预期）: %r" % (e,))
        try:
            await c.disconnect()
        except Exception:
            pass
    await asyncio.sleep(2)
    back = None
    t0 = time.time()
    while time.time() - t0 < 12:
        cc = await try_connect(addr, timeout=3)
        if cc is not None:
            back = time.time() - t0
            try:
                await cc.disconnect()
            except Exception:
                pass
            break
        await asyncio.sleep(0.5)
    print("  重启后 %.1fs 可连" % back if back else "  ★ 重启后没回来")

    print("\n================= 小结 =================")
    for k, v in report.items():
        print("  %-22s %s" % (k, v))
    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
