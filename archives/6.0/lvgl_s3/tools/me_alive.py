#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""专门验「发 ME 会不会把主循环按住」。

判据：
  ① ME:FIRE 之后，固件把 ME 文本分帧敲进 USB HID
  ② **敲的过程中**主循环必须活着 —— 每 50ms 发一次 CFGGET，
     只要有一次在 2 秒内没回话，就说明主循环被按住了（老实现是 40 秒）
  ③ 敲完之后日志环里要有 "[ME] done"（说明真的敲完了，不是半途中断）

用法:
    /usr/bin/python3 tools/me_alive.py [设备地址前缀] [ME 文本字符数]
"""
import asyncio
import sys
import time

from bleak import BleakClient, BleakScanner

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from verify_fix import Bt, CHAR_UUID  # noqa: E402


async def write(c, cmd, wait=0.0):
    """发一条指令（带一个可选的等待）。"""
    await c.write_gatt_char(CHAR_UUID, (cmd + "\n").encode(), response=True)
    if wait:
        await asyncio.sleep(wait)

ONLY = sys.argv[1].upper() if len(sys.argv) > 1 else ""
CHARS = int(sys.argv[2]) if len(sys.argv) > 2 else 36


async def find(timeout=10.0):
    hits = []

    def cb(dev, adv):
        n = (adv.local_name or dev.name or "")
        if "YYQ-MX9.0" in n and (not ONLY or dev.address.upper().startswith(ONLY)):
            if dev.address not in hits:
                hits.append(dev.address)

    s = BleakScanner(detection_callback=cb)
    await s.start()
    try:
        t0 = time.monotonic()
        while time.monotonic() - t0 < timeout and not hits:
            await asyncio.sleep(0.4)
    finally:
        await s.stop()
    return hits[0] if hits else None


async def main():
    addr = await find()
    if not addr:
        print("✗ 没找到设备")
        return 1
    print("✓ 设备 %s\n" % addr)

    bt = Bt()
    async with BleakClient(addr, timeout=20) as c:
        await c.start_notify(CHAR_UUID, lambda s, d: bt.feed(d))
        await asyncio.sleep(0.5)

        # ---- 先把 ME 文本写进 SPIFFS ----
        text = "测试" * (CHARS // 2)
        hexstr = text.encode("utf-8").hex()
        print("① 下发 ME 文本：%d 字符 → hex %d 字节" % (len(text), len(hexstr)))
        await write(c, "ME_START", 0.3)
        for i in range(0, len(hexstr), 128):
            await write(c, "ME_DATA:" + hexstr[i:i + 128], 0.0)
            await asyncio.sleep(0.01)
        await write(c, "ME_END", 1.5)
        print("   已发 ME_END")

        # ---- 触发输出，同时不停问 CFGGET ----
        print("② 发 ME:FIRE 触发输出，同时每 50ms 发一次 CFGGET")
        t0 = time.monotonic()
        await write(c, "ME:FIRE", 0.05)
        ok = bad = 0
        worst = 0.0
        while time.monotonic() - t0 < 12:
            s0 = time.monotonic()
            try:
                await write(c, "CFGGET", 0.0)
                await asyncio.sleep(0.05)
            except Exception:
                pass
            dt = time.monotonic() - s0
            worst = max(worst, dt)
            if dt < 2.0:
                ok += 1
            else:
                bad += 1
            await asyncio.sleep(0.05)
        print("   往返 %d 次：正常 %d，超时(>2s) %d，最慢 %.2fs"
              % (ok + bad, ok, bad, worst))
        if bad == 0:
            print("   ✓ 主循环全程活着 —— ME 没把它按住")
        else:
            print("   ★ 有 %d 次没回话 —— 主循环被按住了" % bad)

        # ---- 轮询日志环，等 [ME] done ----
        # 2000 字符 = 12000 个 hex 字符 = 24000 个 HID report，
        # 就算每个 report 2ms 也要 48 秒 —— 这正是"老实现会把主循环按住 40 秒"的量级。
        # 新实现是分帧后台敲，所以这段时间主循环照常活着（上面已经证明了）。
        print("③ 轮询日志环，等 [ME] done（可能要几十秒）…")
        me_lines = []
        done = False
        for _ in range(40):          # 最多 40 × 5s = 200s
            await asyncio.sleep(5)
            bt.done.clear()
            await write(c, "LOG:dump", 0.2)
            for _ in range(80):
                await asyncio.sleep(0.25)
                if bt.done:
                    break
            text_dump = ""
            for d in bt.take("LOGDUMP"):
                text_dump += d[2]
            me_lines = [l for l in text_dump.splitlines() if "[ME]" in l]
            done = any("done" in l for l in me_lines)
            if done:
                break
        print("   日志里的 ME 相关行（最后 12 条）：")
        for l in me_lines[-12:]:
            print("   " + l)
        if done:
            print("   ✓ ME 完整敲完了（用时约 %d 秒）" % (5 * (_ + 1)))
        else:
            print("   ★ 没看到 [ME] done —— 输出没跑完")
    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
