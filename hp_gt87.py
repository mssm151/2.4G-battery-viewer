# -*- coding: utf-8 -*-
"""hp_gt87.py —— 惠普 GT87 2.4G 键盘电量读取

协议来自 HP Keyboard（OemDrv.exe）启动时的真实报文抓取 + 复现：

* 设备：VID 0x3554 / PID 0xFA09（Compx 2.4G 接收器）
* 接口：UsagePage 0xFF02 / Usage 0x0002，输入/输出各 20 字节
* Report ID 0x13
* 查询请求：``13 05 01 00`` + 16 字节 0（共 20 字节）
* 接收器随后回两条：

     13 05 01 00 0a 03 00 00 00 03 32 01 ... 5c   ← PSD
     13 0a 01 00 04 05 64 01 00 ...          8c   ← 电源信息

  回复格式：``13 <子命令> <状态> 00 <长度> <数据...> <校验>``
  子命令 0x0a 的数据为 ``05 64 01 00``：
      数据[0] = 0x05（字段标识）
      数据[1] = 电量百分比（0x64 = 100）
      数据[2] = 状态位：bit4（0x10）= 充电中；未充电时为 0x01

用法
----
    python hp_gt87.py
"""
from __future__ import annotations

import time
from dataclasses import dataclass
from typing import Optional

import hid_win as hw
from hid_win import HidDevice, HidError, HidInterface

VID = 0x3554
PID = 0xFA09
USAGE_PAGE = 0xFF02
USAGE = 0x0002
REPORT_ID = 0x13
REPORT_LEN = 20

CMD_GET_INFO = 0x05
CMD_POWER = 0x0A
BATTERY_INDEX = 6        # 含 Report ID 的报文下标
CHARGING_INDEX = 7


@dataclass
class HPInfo:
    online: bool = False
    level: Optional[int] = None
    status_raw: Optional[int] = None
    charging: Optional[bool] = None
    name: str = "HP GT87"
    detail: str = ""
    raw: bytes = b""

    @property
    def charging_text(self) -> str:
        if self.charging is None:
            return ""
        return "充电中" if self.charging else "未充电"


def find_interface() -> Optional[HidInterface]:
    for it in hw.enumerate_hid():
        if (it.vid == VID and it.pid == PID
                and it.usage_page == USAGE_PAGE and it.usage == USAGE):
            return it
    return None


def read_info(timeout: float = 1.0) -> HPInfo:
    info = HPInfo()
    iface = find_interface()
    if iface is None:
        info.detail = "没找到惠普 GT87 接收器（请插好 2.4G dongle）"
        return info
    info.online = True
    body = bytes([REPORT_ID, CMD_GET_INFO, 0x01, 0x00]) + bytes(REPORT_LEN - 4)
    try:
        with HidDevice(iface.path, write=True) as dev:
            for _ in range(2):                       # 偶尔会丢一帧，重试一次
                dev.drain()
                dev.write_output(body)
                deadline = time.monotonic() + timeout
                while time.monotonic() < deadline:
                    r = dev.read_input(timeout_ms=150)
                    if not r or len(r) < 8 or r[0] != REPORT_ID:
                        continue
                    if r[1] != CMD_POWER:
                        continue
                    info.raw = r
                    if r[2] != 0x01:
                        info.detail = f"接收器返回错误状态 0x{r[2]:02X}"
                        return info
                    length = r[4]
                    data = r[5:5 + length]
                    if len(data) >= 2:
                        level = data[1]
                        if 0 <= level <= 100:
                            info.level = level
                        else:
                            info.detail = f"电量字节异常：0x{level:02X}"
                    if len(data) >= 3:
                        info.status_raw = data[2]
                        info.charging = bool(data[2] & 0x10)
                    return info
    except HidError as e:
        info.detail = f"打开接收器失败：{e}"
        return info
    info.detail = "接收器没有回复电量（请按一下键盘唤醒）"
    return info


if __name__ == "__main__":
    r = read_info()
    if r.level is None:
        print(f"{r.name}: 读取失败 —— {r.detail}")
    else:
        print(f"{r.name}: {r.level}%  {r.charging_text}")
        print("  raw:", r.raw.hex(" "))
