# -*- coding: utf-8 -*-
"""rapoo_vt7.py —— 雷柏 VT7（Rapoo VT7）2.4G 鼠标电量读取

协议来自 RapooDevice.dll（A HUB）逆向 + 本机抓包：

* 设备：VID 0x24AE / PID 0x1413（nRF54L15 方案接收器）
* 接口：UsagePage 0xFF00 / Usage 0x0002，输入/输出各 19 字节
* 设备会**主动**每 2–5 秒推送一条 Report ID 0x07 的状态报文，
  不需要发任何命令：

      07 | 10 04 60 09 60 09 02 64 00 03 02 00 13 46 01 02 02 xx
                         ^^^^^
      报文[7] = 充电状态（0x01 = 未充电，0x02 = 充电中）
      报文[8] = 电量百分比（0x64 = 100）

  报文里还带着 DPI（0x0960 = 2400）等信息，这里只用前两个字段。

用法
----
    python rapoo_vt7.py
"""
from __future__ import annotations

import time
from dataclasses import dataclass
from typing import Optional

import hid_win as hw
from hid_win import HidDevice, HidError, HidInterface

VID = 0x24AE
PID = 0x1413
USAGE_PAGE = 0xFF00
USAGE = 0x0002
REPORT_ID = 0x07
CHARGING_OFFSET = 7      # 含 Report ID 的报文下标（1=未充电，2=充电中）
BATTERY_OFFSET = 8


@dataclass
class RapooInfo:
    online: bool = False
    level: Optional[int] = None
    charging: Optional[int] = None
    name: str = "Rapoo VT7"
    detail: str = ""
    raw: bytes = b""

    @property
    def charging_text(self) -> str:
        if self.charging is None:
            return ""
        if self.charging == 2:
            return "充电中"
        if self.charging == 1:
            return "未充电"
        return f"状态 {self.charging}"


def find_interface() -> Optional[HidInterface]:
    for it in hw.enumerate_hid():
        if (it.vid == VID and it.pid == PID
                and it.usage_page == USAGE_PAGE and it.usage == USAGE):
            return it
    return None


def read_info(timeout: float = 6.5) -> RapooInfo:
    """等一条设备主动推送的状态报文，读出电量。"""
    info = RapooInfo()
    iface = find_interface()
    if iface is None:
        info.detail = "没找到雷柏 VT7 接收器（请插好 2.4G dongle）"
        return info
    info.online = True
    try:
        with HidDevice(iface.path) as dev:
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                r = dev.read_input(timeout_ms=250)
                if not r:
                    continue
                if r[0] != REPORT_ID:
                    continue
                info.raw = r
                if len(r) > BATTERY_OFFSET:
                    level = r[BATTERY_OFFSET]
                    if 0 <= level <= 100:
                        info.level = level
                    else:
                        info.detail = f"电量字节异常：0x{level:02X}"
                if len(r) > CHARGING_OFFSET:
                    info.charging = r[CHARGING_OFFSET]
                return info
    except HidError as e:
        info.detail = f"打开接收器失败：{e}"
        return info
    info.detail = "等了几秒没收到状态报文（请动一下鼠标）"
    return info


if __name__ == "__main__":
    r = read_info()
    if r.level is None:
        print(f"{r.name}: 读取失败 —— {r.detail}")
    else:
        print(f"{r.name}: {r.level}%  {r.charging_text}")
        print("  raw:", r.raw.hex(" "))
