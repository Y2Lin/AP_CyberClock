#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""CyberClock USB 对时(免蓝牙方案,Mac/Windows/Linux 通用)

用刷固件的那条 USB 线直接对时,完全绕开蓝牙和浏览器。

用法:
    python3 usb-sync.py            自动: 找到设备 -> 探活 -> 写入本机时间 -> 回读状态
    python3 usb-sync.py --list      列出候选串口
    python3 usb-sync.py --port /dev/cu.usbmodemXXX   指定串口

依赖: pip3 install pyserial
说明: 设备固件需 v5 及以上(带 USB 对时命令);旧固件会提示需要升级。
"""
import argparse
import sys
import time
from datetime import datetime

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    print("缺少依赖: 请先运行  pip3 install pyserial")
    sys.exit(1)

# Espressif 官方 USB VID: ESP32-C3 USB Serial/JTAG = 303A:1001
ESPRESSIF_VID = 0x303A


def find_ports():
    """返回候选串口列表 [(device, description)]"""
    out = []
    for p in list_ports.comports():
        if p.vid == ESPRESSIF_VID or (p.device and "usb" in p.device.lower()):
            out.append((p.device, p.description or ""))
    return out


def open_port(name):
    # USB CDC 不关心波特率,但 pyserial 需要一个值
    return serial.Serial(name, 115200, timeout=2)


def read_until(ser, prefixes, timeout=3.0):
    """读行直到出现以 prefixes 开头的行;返回该行(不含换行),超时返回 None"""
    deadline = time.time() + timeout
    buf = b""
    while time.time() < deadline:
        chunk = ser.read(256)
        if chunk:
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                text = line.decode(errors="replace").strip()
                if not text:
                    continue
                for pre in prefixes:
                    if text.startswith(pre):
                        return text
            # 可能不带换行(极少数终端),按前缀匹配整段
            text = buf.decode(errors="replace")
            for pre in prefixes:
                if text.startswith(pre):
                    return text
    return None


def main():
    ap = argparse.ArgumentParser(description="CyberClock USB 对时")
    ap.add_argument("--list", action="store_true", help="列出候选串口")
    ap.add_argument("--port", help="指定串口(如 /dev/cu.usbmodemXXX)")
    args = ap.parse_args()

    if args.list:
        ports = find_ports()
        if not ports:
            print("未发现候选串口。检查: USB 线是否插好、是否为数据线、"
                  "设备是否上电。")
            return
        for dev, desc in ports:
            print(f"  {dev}  {desc}")
        return

    if args.port:
        port_name = args.port
    else:
        ports = find_ports()
        if not ports:
            print("未发现候选串口。检查: USB 线是否插好、是否为数据线、"
                  "设备是否上电;或用 --list 查看。")
            sys.exit(1)
        if len(ports) > 1:
            print("发现多个候选串口:")
            for dev, desc in ports:
                print(f"  {dev}  {desc}")
            print("请用 --port 指定其一。")
            sys.exit(1)
        port_name = ports[0][0]
    print(f"串口: {port_name}")

    try:
        ser = open_port(port_name)
    except Exception as e:
        print(f"打开串口失败: {e}")
        print("若提示权限错误(Linux): sudo usermod -aG dialout $USER 后重新登录")
        sys.exit(1)

    # 保持 RTS/DTR 常低,避免关闭串口时触发 ESP32-C3 内置 USB 复位逻辑
    # (关闭端口时的电平回落会被芯片当作复位脉冲,表现为对时完设备重启)。
    # 顺序必须先 RTS 后 DTR: 若先降 DTR 会出现 DTR=0 & RTS=1 的复位组合。
    try:
        ser.rts = False
        ser.dtr = False
    except Exception:
        pass

    with ser:
        ser.reset_input_buffer()

        # 1) 探活:确认固件支持 USB 对时
        ser.write(b"PING\n")
        pong = read_until(ser, ("PONG",), timeout=3.0)
        if pong is None:
            print("设备无响应 PING。可能原因:")
            print("  - 固件是 v4 及更早(无 USB 对时命令),请刷入 v5 固件")
            print("  - 串口选错(用 --list 查看),或设备未在运行本固件")
            sys.exit(1)
        print("设备响应: PONG (固件支持 USB 对时)")

        # 2) 写入本机当前时间
        tz = int(datetime.now().astimezone().utcoffset().total_seconds()) // 3600
        ts = int(time.time())
        cmd = f"T {ts} {tz}\n".encode()
        ser.write(cmd)
        print(f"发送: {cmd.decode().strip()}  (本地 {datetime.now().strftime('%Y-%m-%d %H:%M:%S')} UTC{tz:+d})")

        reply = read_until(ser, ("OK", "ERR"), timeout=3.0)
        if not reply or not reply.startswith("OK"):
            print(f"对时失败: {reply or '超时'}")
            sys.exit(1)
        print(f"设备确认: {reply}")

        # 3) 回读状态
        ser.write(b"Q\n")
        status = read_until(ser, ('{"ts"',), timeout=3.0)
        if status:
            print(f"设备状态: {status}")
            print("完成: 屏幕时间应在 1 秒内更新(状态栏 SYNCED)。")
        else:
            print("状态回读超时(不影响已完成的对时)。")


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
