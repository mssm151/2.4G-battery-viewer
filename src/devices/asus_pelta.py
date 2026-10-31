# -*- coding: utf-8 -*-
"""asus_pelta.py —— ASUS ROG 破风（ROG PELTA）2.4G 耳机电量读取

协议由 AacAudioHal_x64.dll（Armoury Crate 的耳机 HAL）逆向得到，
并在本机 ROG PELTA (2.4GHz) 上实测通过。

传输
----
* HID 接口：UsagePage 0xFF00 / Usage 0x0001，输入/输出各 64 字节
* Report ID：0xCC（65 字节报文 = Report ID + 64 字节载荷）
* 请求载荷以 ``12 <子命令> 00 00`` 开头，其余补 0
* 回复载荷以 ``12 <子命令> 00 00`` 回显，数据从第 4 字节开始

用到的子命令
------------
* ``0x07`` 电源信息：回复载荷
  ``12 07 00 00 <状态> <电量> <未知> <未知>``
  其中 **载荷[5] 是电量百分比**（0xFE 表示无效，>100 视为异常）。
* ``0x28`` 充电状态：回复 ``12 28 00 00 <状态>``
  厂商 HAL 把原始值映射为：0=已充满 / 1=未充电 / 2=充电中。

用法
----
    python -c "import asus_pelta; print(asus_pelta.read_info())"
"""
from __future__ import annotations

import time
from dataclasses import dataclass
from typing import Optional

import hid_win as hw
from hid_win import HidDevice, HidError, HidInterface

VID = 0x0B05
# 破风的三个 SKU：2.4G 接收器 / 有线 / 蓝牙，2.4G 版本通常是 0x1B84
PIDS = (0x1B82, 0x1B84, 0x1B86)
USAGE_PAGE = 0xFF00
USAGE = 0x0001
REPORT_ID = 0xCC
REPORT_LEN = 65

CMD_POWER_INFO = 0x07
CMD_CHARGING = 0x28

CHARGING_TEXT = {
    0: "已充满",
    1: "未充电",
    2: "充电中",
    0xFE: "无效",
}


@dataclass
class PeltaInfo:
    online: bool = False
    level: Optional[int] = None
    charging: Optional[int] = None       # 0=满 1=放电 2=充电
    name: str = "ROG PELTA"
    detail: str = ""
    raw_power: bytes = b""
    raw_charging: bytes = b""

    @property
    def charging_text(self) -> str:
        if self.charging is None:
            return ""
        return CHARGING_TEXT.get(self.charging, f"状态 {self.charging}")


_iface: Optional[HidInterface] = None


def _invalidate() -> None:
    global _iface
    _iface = None


def find_interface(refresh: bool = False) -> Optional[HidInterface]:
    """缓存接口路径；枚举 HID 一次约 100+ms，没必要每轮都做。"""
    global _iface
    if _iface is not None and not refresh:
        return _iface
    _iface = None
    for it in hw.enumerate_hid():
        if (it.vid == VID and it.pid in PIDS
                and it.usage_page == USAGE_PAGE and it.usage == USAGE):
            _iface = it
            return it
    return None


def _query(dev: HidDevice, subcmd: int, timeout: float = 0.8) -> Optional[bytes]:
    """发一条查询并等对应回复，返回去掉 Report ID 后的载荷。"""
    body = bytes([REPORT_ID, 0x12, subcmd, 0x00, 0x00]) + bytes(REPORT_LEN - 5)
    dev.drain()
    try:
        dev.write_output(body)
    except HidError:
        return None
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        r = dev.read_input(timeout_ms=150)
        if not r:
            continue
        payload = r[1:] if r and r[0] == REPORT_ID else r
        if len(payload) >= 2 and payload[0] == 0x12 and payload[1] == subcmd:
            return payload
    return None


def read_info(timeout: float = 0.8) -> PeltaInfo:
    """读取一次耳机电量/充电状态。"""
    info = PeltaInfo()
    iface = find_interface()
    if iface is None:
        info.detail = "没找到 ROG PELTA 接收器（请插好 2.4G dongle）"
        return info
    info.online = True
    try:
        with HidDevice(iface.path, write=True) as dev:
            power = _query(dev, CMD_POWER_INFO, timeout) \
                or _query(dev, CMD_POWER_INFO, timeout)
            if power is None:
                info.detail = "耳机没有回复（请确认耳机已开机）"
                return info
            info.raw_power = power
            if len(power) > 5:
                raw = power[5]
                if raw == 0xFE:
                    info.detail = "耳机上报电量为无效值"
                elif raw <= 100:
                    info.level = raw
                else:
                    info.detail = f"电量字节异常：0x{raw:02X}"

            charging = _query(dev, CMD_CHARGING, timeout) \
                or _query(dev, CMD_CHARGING, timeout)
            if charging is not None:
                info.raw_charging = charging
                if len(charging) > 4:
                    info.charging = charging[4]
    except HidError as e:
        _invalidate()                       # 可能被拔插过，下次重新枚举
        info.detail = f"打开接收器失败：{e}"
    return info


if __name__ == "__main__":
    r = read_info()
    if r.level is None:
        print(f"{r.name}: 读取失败 —— {r.detail}")
    else:
        print(f"{r.name}: {r.level}%  {r.charging_text}"
              f"{('  ' + r.detail) if r.detail else ''}")
        print("  power   :", r.raw_power.hex(" "))
        print("  charging:", r.raw_charging.hex(" "))
