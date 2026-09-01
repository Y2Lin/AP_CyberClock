#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""CyberClock BLE 时间同步（Mac / Windows / Linux 通用）

走系统蓝牙（macOS 为 CoreBluetooth），与浏览器完全无关——
Chrome 网页搜不到设备时的可靠替代方案。

用法:
    python3 mac-sync.py               自动: 按服务UUID扫描 -> 连接 -> 写入当前时间 -> 回读状态
    python3 mac-sync.py --list        列出附近所有 BLE 设备（排查"搜不到设备"）
    python3 mac-sync.py --name 名字    按设备名连接（名称被系统缓存成旧名时手动指定）
    python3 mac-sync.py --addr 地址    按地址连接（从 --list 结果里复制）

依赖: pip3 install bleak
"""
import argparse
import asyncio
import sys
import time
from datetime import datetime

try:
    from bleak import BleakScanner, BleakClient
except ImportError:
    print("缺少依赖: 请先运行  pip3 install bleak")
    sys.exit(1)

SERVICE_UUID = "0000ffc0-0000-1000-8000-00805f9b34fb"
CHAR_WRITE_UUID = "0000ffc1-0000-1000-8000-00805f9b34fb"
CHAR_NOTIFY_UUID = "0000ffc2-0000-1000-8000-00805f9b34fb"


def tz_hours():
    off = datetime.now().astimezone().utcoffset()
    return int(off.total_seconds()) // 3600


def notify_handler(sender, data):
    print(f"  设备状态(FFC2): {data.decode(errors='replace')}")


async def list_devices():
    print("扫描附近所有 BLE 设备(约 5 秒)…")
    devices = await BleakScanner.discover(timeout=5.0)
    if not devices:
        print("没扫到任何 BLE 设备。检查:")
        print("  1) Mac 蓝牙是否开启")
        print("  2) 若此刻安卓手机能看到 CyberClock，则是 Mac 蓝牙权限/适配器问题——重启蓝牙或重启 Mac")
        return
    for d in sorted(devices, key=lambda x: (x.name or "")):
        print(f"  {d.address}  {d.name or '(无名)'}")


async def find_device():
    # 优先按服务 UUID 扫描: 不受 macOS 广播名缓存影响
    print(f"按服务 UUID 扫描…(约 5 秒)")
    devices = await BleakScanner.discover(service_uuids=[SERVICE_UUID], timeout=5.0)
    if devices:
        return devices[0]
    print("按 UUID 未找到，全量扫描按名称查找…")
    devices = await BleakScanner.discover(timeout=5.0)
    for d in devices:
        if d.name and "cyberclock" in d.name.lower():
            return d
    return None


async def sync(target):
    tz = tz_hours()
    ts = int(time.time())
    payload = ts.to_bytes(4, "little") + tz.to_bytes(2, "little", signed=True)
    print(f"本机时间: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')} (UTC{tz:+d})")
    print(f"写入字节: {payload.hex(' ').upper()}")

    async with BleakClient(target) as client:
        print(f"已连接: {client.is_connected}")
        await client.start_notify(CHAR_NOTIFY_UUID, notify_handler)
        await client.write_gatt_char(CHAR_WRITE_UUID, payload, response=True)
        print("写入成功，等待设备状态回读(约 6 秒)…")
        await asyncio.sleep(6)
        await client.stop_notify(CHAR_NOTIFY_UUID)
    print("完成: 屏幕应已跳到当前时间(状态栏 SYNCED)。")


async def main():
    ap = argparse.ArgumentParser(description="CyberClock BLE 时间同步")
    ap.add_argument("--list", action="store_true", help="列出附近所有 BLE 设备")
    ap.add_argument("--name", help="按设备名连接")
    ap.add_argument("--addr", help="按地址连接")
    args = ap.parse_args()

    if args.list:
        await list_devices()
        return

    if args.name:
        print(f"按名称扫描: {args.name}…")
        devices = await BleakScanner.discover(timeout=5.0)
        target = next((d for d in devices if d.name and args.name.lower() in d.name.lower()), None)
        if not target:
            print("未找到该名称的设备; 用 --list 查看实际广播名")
            sys.exit(1)
    elif args.addr:
        target = args.addr  # BleakClient 可直接接受地址字符串
    else:
        target = await find_device()
        if target is None:
            print("未找到 CyberClock。排查:")
            print("  1) 安卓手机是否仍处于连接状态(连接中设备不广播, 先 DISCONNECT)")
            print("  2) 运行 python3 mac-sync.py --list 看设备以什么名字出现, 再 --name 指定")
            print("  3) --list 里一个设备都没有: Mac 蓝牙权限/适配器问题, 重启蓝牙或重启 Mac")
            sys.exit(1)

    await sync(target)


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
    except Exception as e:
        print(f"出错: {e}")
        sys.exit(1)
