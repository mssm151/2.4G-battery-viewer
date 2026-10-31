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

性能
----
首次读取会启动一个**常驻监听线程**，持续收设备推送并缓存最新报文；
之后 `read_info()` 直接返回缓存，通常 0 延迟，刷新不再等待 2–5 秒。
线程阻塞在 HID 读事件上，空闲时几乎不占 CPU。

用法
----
    python rapoo_vt7.py
"""
from __future__ import annotations

import threading
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
FRESH_SECONDS = 10.0     # 缓存报文的有效期（设备每 2–5 秒推一次）


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


_iface: Optional[HidInterface] = None
_latest: Optional[RapooInfo] = None
_latest_ts = 0.0
_lock = threading.Lock()
_thread: Optional[threading.Thread] = None
_stop = threading.Event()


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
        if (it.vid == VID and it.pid == PID
                and it.usage_page == USAGE_PAGE and it.usage == USAGE):
            _iface = it
            return it
    return None


def _parse_report(r: bytes) -> RapooInfo:
    info = RapooInfo(online=True, raw=r)
    if len(r) > BATTERY_OFFSET:
        level = r[BATTERY_OFFSET]
        if 0 <= level <= 100:
            info.level = level
        else:
            info.detail = f"电量字节异常：0x{level:02X}"
    if len(r) > CHARGING_OFFSET:
        info.charging = r[CHARGING_OFFSET]
    return info


def _listen_loop() -> None:
    global _latest, _latest_ts
    backoff = 1.0
    while not _stop.is_set():
        iface = find_interface()
        if iface is None:
            _stop.wait(backoff)
            backoff = min(backoff * 2, 30.0)
            continue
        backoff = 1.0
        try:
            with HidDevice(iface.path) as dev:
                while not _stop.is_set():
                    t0 = time.monotonic()
                    r = dev.read_input(timeout_ms=500)
                    if not r:
                        # 读句柄异常时会立即返回 None，避免忙等
                        if time.monotonic() - t0 < 0.05:
                            _stop.wait(0.2)
                        continue
                    if r[0] != REPORT_ID:
                        continue
                    info = _parse_report(r)
                    with _lock:
                        _latest = info
                        _latest_ts = time.monotonic()
        except HidError:
            _invalidate()                   # 可能被拔插过，重新枚举
            _stop.wait(1.0)


def _ensure_listener() -> None:
    global _thread
    if _thread is None or not _thread.is_alive():
        _thread = threading.Thread(target=_listen_loop, daemon=True,
                                   name="rapoo-listener")
        _thread.start()


def read_info(timeout: float = 6.5) -> RapooInfo:
    """读取最新推送报文；有缓存时立即返回。"""
    _ensure_listener()
    deadline = time.monotonic() + timeout
    while True:
        with _lock:
            info, ts = _latest, _latest_ts
        if info is not None and time.monotonic() - ts <= FRESH_SECONDS:
            return info
        if time.monotonic() >= deadline:
            break
        time.sleep(0.05)

    info = RapooInfo()
    if find_interface() is None:
        info.detail = "没找到雷柏 VT7 接收器（请插好 2.4G dongle）"
    else:
        info.online = True
        info.detail = "等了几秒没收到状态报文（请动一下鼠标）"
    return info


if __name__ == "__main__":
    r = read_info()
    if r.level is None:
        print(f"{r.name}: 读取失败 —— {r.detail}")
    else:
        print(f"{r.name}: {r.level}%  {r.charging_text}")
        print("  raw:", r.raw.hex(" "))
