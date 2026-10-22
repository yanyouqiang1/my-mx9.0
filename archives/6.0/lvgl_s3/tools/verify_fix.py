#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
修完之后的一轮验证。三个用户现象各验一遍，全部走真实链路：

① HTML 连接上   → CFGGET 拉整份配置，看完整性和耗时
② 发 ME         → 小段 ME 文本下发 + ME:FIRE 触发输出，
                   **输出进行中同时发 CFGGET**，
                   关键判据：ME 在打字的时候，主循环还活着吗？
                   （老实现是一把梭：loop() 被按住 40 秒，
                     这期间蓝牙一条都不回 —— 用户看到的就是"崩了"）
③ 蓝牙稳不稳   → 断开后能不能被重新扫到、连上

用法：
    /usr/bin/python3 tools/verify_fix.py
"""
import asyncio
import re
import sys
import time

from bleak import BleakClient, BleakScanner

SERVICE_UUID = "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
CHAR_UUID = "beb5483e-36e1-4688-b7f5-ea07361b26a8"


class Bt:
    def __init__(self):
        self._line = ""
        self._buf = {}
        self._expect = {}
        self.done = []
        self.raw = []

    def feed(self, data):
        txt = bytes(data).decode("utf-8", "replace")
        self.raw.append(txt)
        self._line += txt
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


async def find_kb(timeout=8.0):
    # ⚠ 不用 BleakScanner.discover()：在 macOS（CoreBluetooth 后端）上它会**漏掉**
    #   本项目的设备（实测 discover() 返回 40 个设备，里面没有 YYQ-MX9.0，
    #   而同一时刻用 detection_callback 扫是能扫到的）。
    #   所以改成回调式扫描：只按广播里的名字挑出 YYQ-MX9.0。
    only = ""
    for i, a in enumerate(sys.argv):
        if a == "--addr" and i + 1 < len(sys.argv):
            only = sys.argv[i + 1].upper()

    hits = []

    def cb(dev, adv):
        n = (adv.local_name or dev.name or "")
        if "YYQ-MX9.0" not in n:
            return
        if only and not dev.address.upper().startswith(only):
            return
        if dev.address not in hits:
            hits.append(dev.address)

    scanner = BleakScanner(detection_callback=cb)
    await scanner.start()
    try:
        t0 = asyncio.get_event_loop().time()
        while asyncio.get_event_loop().time() - t0 < timeout and not hits:
            await asyncio.sleep(0.4)
    finally:
        await scanner.stop()
    return hits[0] if hits else None


async def write(c, cmd):
    await c.write_gatt_char(CHAR_UUID, (cmd + "\n").encode(), response=True)


async def main():
    bt = Bt()
    kb = await find_kb()
    if not kb:
        print("✗ 没找到 BLE 设备")
        return 1
    print("✓ 设备 %s\n" % kb)

    async with BleakClient(kb, timeout=15) as c:
        await c.start_notify(CHAR_UUID, lambda s, d: bt.feed(d))
        await asyncio.sleep(0.4)

        # ---------- ① HTML 连接上：CFGGET ----------
        print("=" * 68)
        print("① 「HTML 连接上」= 自动拉整份配置")
        print("=" * 68)
        bt.done.clear()
        t0 = time.time()
        await write(c, "CFGGET")
        got = None
        for _ in range(60):
            await asyncio.sleep(0.3)
            g = bt.take("CFGDUMP")
            if g:
                got = g[0]
                break
        if got:
            v, k, body, exp, n = got
            print("  CFGDUMP 收到 %d 字节（固件声明 %s），耗时 %.2fs"
                  % (n, exp, time.time() - t0))
            if exp is not None and n != exp:
                print("  ★ 丢片！声明 %s 实收 %d" % (exp, n))
            else:
                print("  ✓ 完整无丢片")
        else:
            print("  ✗ 没收到 CFGDUMP")

        # ---------- ② 发 ME ----------
        print("\n" + "=" * 68)
        print("② 「发 ME」= 下发文本 + 触发输出，测输出中主循环死没死")
        print("=" * 68)
        text = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"
        hexstr = "".join("%02X" % b for b in text.encode())
        print("  文本 %d 字符 → hex %d 字节（1 片 ME_DATA）" % (len(text), len(hexstr)))
        await write(c, "ME_START")
        await asyncio.sleep(0.3)
        await write(c, "ME_DATA:" + hexstr)
        await asyncio.sleep(0.3)
        await write(c, "ME_END")
        await asyncio.sleep(0.8)

        me_line = None
        for t in bt.raw:
            if "[ME] END" in t:
                me_line = t
        print("  固件回报：%s" % (me_line.strip() if me_line else "(没收到 [ME] END)"))

        # 触发输出。⚠ 它会真的往这台 Mac 上打字（hex 字符），
        #    这是验证"输出期间主循环还活着"的唯一办法。
        print("\n  → 发 ME:FIRE 触发输出，**同时**每 50ms 发一次 CFGGET，")
        print("    看主循环有没有被按住（老实现会 40 秒不回话）")
        bt.done.clear()
        await write(c, "ME:FIRE")
        t_start = time.time()

        lat = []
        rounds = 0
        while time.time() - t_start < 3.0:
            rounds += 1
            ts = time.time()
            bt.done.clear()
            await write(c, "CFGGET")
            # 等这一次的 CFGDUMP 收尾
            deadline = time.time() + 2.0
            ok = False
            while time.time() < deadline:
                await asyncio.sleep(0.02)
                g = bt.take("CFGDUMP")
                if g:
                    lat.append(time.time() - ts)
                    ok = True
                    break
            if not ok:
                lat.append(None)
            await asyncio.sleep(0.05)

        good = [x for x in lat if x is not None]
        bad = len(lat) - len(good)
        print("  ME 输出期间：共 %d 次往返，成功 %d 次，超时 %d 次"
              % (rounds, len(good), bad))
        if good:
            print("  往返耗时：最快 %.0fms，最慢 %.0fms，中位 %.0fms"
                  % (min(good) * 1000, max(good) * 1000,
                     sorted(good)[len(good) // 2] * 1000))
        if bad == 0 and good:
            print("  ✓ ME 在打字的时候主循环**照常活着**，蓝牙一条没丢")
        else:
            print("  ★ 有 %d 次没回话 —— 主循环被按住了" % bad)

        # 输出完了吗
        done_line = None
        for t in bt.raw:
            if "[ME] done" in t:
                done_line = t
        print("  输出收尾：%s" % (done_line.strip() if done_line else "(还没看到 done)"))

        # ---------- ③ 蓝牙稳定性 ----------
        print("\n" + "=" * 68)
        print("③ 「蓝牙不稳」= 断开后还能不能被重新扫到")
        print("=" * 68)
        print("  断开当前连接…")
        await c.disconnect()
        await asyncio.sleep(1.0)

        back = None
        for attempt in range(5):
            t0 = time.time()
            back = await find_kb(timeout=4.0)
            if back:
                print("  ✓ 第 %d 次尝试就扫到了（%.1fs）" % (attempt + 1, time.time() - t0))
                break
            await asyncio.sleep(1.0)
        if not back:
            print("  ✗ 5 次尝试都扫不到 —— 广播没起来")
        else:
            async with BleakClient(back, timeout=10) as c2:
                await c2.start_notify(CHAR_UUID, lambda s, d: bt.feed(d))
                await asyncio.sleep(0.3)
                bt.done.clear()
                await write(c2, "CFGGET")
                ok = False
                for _ in range(40):
                    await asyncio.sleep(0.25)
                    if bt.take("CFGDUMP"):
                        ok = True
                        break
                print("  重连后拉配置：%s" % ("✓ 正常" if ok else "✗ 没回话"))

    print("\n验证完毕。")
    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
