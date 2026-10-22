#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""用 CoreBluetooth 直接断开某个外设的连接（绕开 blueutil 的 TCC 授权等待）。

用法: /usr/bin/python3 ble_disconnect.py AC:A7:04:E1:01:E1
"""
import sys
import time

from CoreBluetooth import CBCentralManager
from Foundation import NSRunLoop, NSDate

TARGET = sys.argv[1] if len(sys.argv) > 1 else "AC:A7:04:E1:01:E1"


def main():
    central = CBCentralManager.alloc().initWithDelegate_queue_options_(None, None, None)
    # 等 state 变成 PoweredOn
    deadline = time.time() + 6.0
    while time.time() < deadline and central.state() != 5:  # 5 == CBManagerStatePoweredOn
        NSRunLoop.currentRunLoop().runUntilDate_(NSDate.dateWithTimeIntervalSinceNow_(0.2))
    print("central state =", central.state(), flush=True)

    # 未配对设备 retrievePeripheralsWithIdentifiers_ 取不到，
    # 改按服务 UUID 取「当前已连接的外设」
    from CoreBluetooth import CBUUID
    peris = central.retrieveConnectedPeripheralsWithServices_(
        [CBUUID.alloc().initWithString_("4fafc201-1fb5-459e-8fcc-c5c9c331914b")])
    peris = [p for p in peris if str(p.identifier()).upper() == TARGET.upper()] or list(peris)
    if not peris:
        print("找不到已连接的外设", flush=True)
        return 2
    for p in peris:
        print("目标: id=%s name=%r state=%s" % (p.identifier(), p.name(), p.state()), flush=True)
        central.cancelPeripheralConnection_(p)
        print("已发起 cancelPeripheralConnection", flush=True)

    # 等连接真正断开
    deadline = time.time() + 8.0
    while time.time() < deadline:
        NSRunLoop.currentRunLoop().runUntilDate_(NSDate.dateWithTimeIntervalSinceNow_(0.3))
        states = [p.state() for p in peris]
        if all(s != 5 for s in states):  # 5 == connected
            print("已断开，state=%s" % states, flush=True)
            return 0
    print("超时，仍未断开，state=%s" % [p.state() for p in peris], flush=True)
    return 1


sys.exit(main())
