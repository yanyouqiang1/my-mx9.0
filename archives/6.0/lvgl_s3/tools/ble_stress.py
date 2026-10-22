#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
针对用户报的三个现象做**定向复现**（不是泛泛地"连一下看看"）：

  ① 蓝牙不稳   → 断开后设备还能不能被重新扫到？反复断连会不会越连越上不去？
  ② HTML 连接  → 网页连上会自动拉整份配置（CFGGET），这条已单独验过；
                  这里补测"连上之后立刻干重活"的场景。
  ③ 发 ME 崩   → 网页分片下发 ME_START / ME_DATA×N / ME_END。
                  ⚠ 关键：真实场景是 **5KB 中文**（=10KB hex=79 片），
                  之前只测过 402 字节（4 片），那测不出队列溢出和 SPIFFS GC。

设备有没有重启，用固件自己打在日志里的 `boots=N` 判定 —— 这是唯一的硬证据，
比"看起来还活着"可靠。

用法：
    /usr/bin/python3 tools/ble_stress.py            # 全套
    /usr/bin/python3 tools/ble_stress.py --only me  # 只测大 ME
"""

import asyncio
import re
import sys
import time

from bleak import BleakClient, BleakScanner

SERVICE_UUID = "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
CHAR_UUID = "beb5483e-36e1-4688-b7f5-ea07361b26a8"


# ---------------------------------------------------------------- 小工具

def log(s):
    print(s, flush=True)


class Bt:
    """按 '\r' 切包、按 'VERB:KEY:' 重组，和网页里那套一致。"""

    def __init__(self):
        self._line = ""
        self._buf = {}
        self._expect = {}
        self.done = []
        self.raw = []

    def feed(self, data):
        try:
            txt = bytes(data).decode("utf-8", "replace")
        except Exception:
            return
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


async def find_kb(timeout=6.0):
    # 可选：只挑地址前缀匹配的设备（多块同名板子在场时要指定）
    only = ""
    for i, a in enumerate(sys.argv):
        if a == "--addr" and i + 1 < len(sys.argv):
            only = sys.argv[i + 1].upper()
    res = await BleakScanner.discover(timeout=timeout)
    addrs = [d.address for d in res] if isinstance(res, list) else list(res)
    if only:
        addrs = [a for a in addrs if a.upper().startswith(only)]
        log("按 --addr %s 过滤后剩 %d 个候选" % (only, len(addrs)))
    log("扫描到 %d 个候选，逐个探测 GATT…" % len(addrs))
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


async def send(c, cmd, wait=0.3):
    try:
        await c.write_gatt_char(CHAR_UUID, (cmd + "\n").encode(), response=True)
    except Exception as e:
        log("  ★ 写失败(%s): %r" % (cmd[:40], e))
        return False
    await asyncio.sleep(wait)
    return True


async def read_boots(c, bt):
    """拉一次固件日志历史，从里面抠出 boots=N —— 设备重启次数。"""
    bt.done.clear()
    await send(c, "LOG:dump", 0.2)
    for _ in range(20):
        await asyncio.sleep(0.4)
        g = bt.take("LOGDUMP")
        if g:
            body = g[0][2]
            m = re.search(r"boots=(\d+)", body)
            r = re.search(r"reason=(\d+)", body)
            return (int(m.group(1)) if m else None,
                    int(r.group(1)) if r else None,
                    body)
    return (None, None, "")


# ---------------------------------------------------------------- 各段测试

async def test_big_me(c, bt, kb):
    """③ 大 ME：5KB 中文 = 79 片 ME_DATA。"""
    log("\n" + "=" * 66)
    log("③ 大 ME 传输（模拟真实场景：5KB 中文）")
    log("=" * 66)

    text = "这是一段用来测试键盘蓝牙下发稳定性的中文文本，" * 100
    view = text.encode("utf-8")
    hexstr = "".join("%02X" % b for b in view)
    nchunk = (len(hexstr) + 127) // 128
    log("  文本 %d 字符 / %d 字节 → hex %d 字节 → %d 片 ME_DATA"
        % (len(text), len(view), len(hexstr), nchunk))

    bt.done.clear()
    ok = await send(c, "ME_START", 0.3)
    if not ok:
        log("  ✗ ME_START 都没发出去")
        return False

    t0 = time.time()
    failed = 0
    slowest = 0.0
    for i in range(0, len(hexstr), 128):
        ts = time.time()
        if not await send(c, "ME_DATA:" + hexstr[i:i + 128], 0.0):
            failed += 1
        dt = time.time() - ts
        slowest = max(slowest, dt)
        # 每片之间让一点点，模拟真实浏览器的节奏（writeValue 是 await 的）
        await asyncio.sleep(0.004)
    dur = time.time() - t0
    log("  下发 %d 片耗时 %.2fs（最慢一片 %.0fms），写失败 %d 片"
        % (nchunk, dur, slowest * 1000, failed))

    await send(c, "ME_END", 1.2)

    # 固件会用一条 [ME] END onDisk=... 告诉我们它到底写进去多少字节
    me_line = None
    for _ in range(20):
        await asyncio.sleep(0.3)
        for t in bt.raw:
            if "[ME] END" in t:
                me_line = t
        if me_line:
            break
    log("  固件回报：%s" % (me_line.strip() if me_line else "（没收到 [ME] END）"))

    # 核对：声明的 hex 字节数 vs 固件实际落盘
    if me_line:
        m = re.search(r"onDisk=(\d+) appended=(\d+) errs=(\d+)", me_line)
        if m:
            on_disk, appended, errs = int(m.group(1)), int(m.group(2)), int(m.group(3))
            want = len(hexstr)
            if on_disk == want and appended == want and errs == 0:
                log("  ✓ 落盘完整：%d/%d 字节，0 错误" % (on_disk, want))
                return True
            log("  ★ 落盘对不上：期望 %d，落盘 %d，追加 %d，错误 %d"
                % (want, on_disk, appended, errs))
            if on_disk < want:
                log("    → 少了 %d 字节 = %d 片 ME_DATA 被吞了"
                    % (want - on_disk, (want - on_disk) // 128 + 1))
            return False
    return False


async def test_reconnect(kb, bt, rounds=4):
    """① 蓝牙稳不稳：断开 → 等 → 能不能再连上。"""
    log("\n" + "=" * 66)
    log("① 断链后重连（蓝牙稳定性）")
    log("=" * 66)

    ok_count = 0
    for r in range(rounds):
        addr = await find_kb(timeout=5.0)
        if not addr:
            log("  第 %d 轮：★ 扫不到设备了（广播没起来？）" % (r + 1))
            await asyncio.sleep(1.0)
            continue
        try:
            async with BleakClient(addr, timeout=10) as c:
                await c.start_notify(CHAR_UUID, lambda s, d: bt.feed(d))
                await asyncio.sleep(0.3)
                await send(c, "PING", 0.4)
                alive = any("PONG" in t or "PING" in t for t in bt.raw[-12:])
                log("  第 %d 轮：连上了，PING %s"
                    % (r + 1, "有回" if alive else "★ 没回"))
                ok_count += 1
        except Exception as e:
            log("  第 %d 轮：★ 连接异常 %r" % (r + 1, e))
            continue
        # 主动断开，模拟主机走人
        await asyncio.sleep(0.3)
    log("  → %d/%d 轮成功重新连上" % (ok_count, rounds))
    return ok_count == rounds


async def main():
    argv = sys.argv
    only = argv[argv.index("--only") + 1] if "--only" in argv else "all"

    kb = await find_kb()
    if not kb:
        log("✗ 没找到 BLE 设备")
        return 1
    log("✓ 设备 %s" % kb)

    bt = Bt()

    async with BleakClient(kb, timeout=15) as c:
        await c.start_notify(CHAR_UUID, lambda s, d: bt.feed(d))
        await asyncio.sleep(0.4)

        boots0, reason0, _ = await read_boots(c, bt)
        log("起始状态：boots=%s reason=%s" % (boots0, reason0))

        if only in ("all", "me"):
            await test_big_me(c, bt, kb)

        # ME 连发几轮，逼 SPIFFS 的 GC 出来（注释里提过"数据一大就崩重启"）
        if only == "all":
            log("\n--- ME 连发 3 轮，逼 SPIFFS GC ---")
            for i in range(3):
                await test_big_me(c, bt, kb)
                bt.raw.clear()

        boots1, reason1, _ = await read_boots(c, bt)
        log("\n结束状态：boots=%s reason=%s" % (boots1, reason1))
        if boots0 is not None and boots1 is not None:
            if boots1 > boots0:
                log("★★ 设备在这轮测试中重启了 %d 次 —— 崩溃复现！"
                    % (boots1 - boots0))
            else:
                log("✓ 设备全程没重启")

    if only == "all":
        await test_reconnect(kb, bt)

    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
