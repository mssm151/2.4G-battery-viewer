# -*- coding: utf-8 -*-
"""rapoo_vt1.py —— 雷柏 VT1 / VT1 Pro（24AE:1214）电量读取

设备在 0xFF00:0x0002（7 字节）上周期推送状态报文，例如：

    BB B0 51 B0 04 01 60
                      ^^
    最后字节 = 电量百分比（0x60 = 96）

它**不参与**托盘的三环显示，只在 tooltip 分割线下方作为“其他设备”显示。
"""
from __future__ import annotations

import threading
import time
from dataclasses import dataclass
from typing import Optional

import hid_win as hw
from hid_win import HidDevice, HidError, HidInterface

VID = 0x24AE
PID = 0x1214
USAGE_PAGE = 0xFF00
USAGE = 0x0002
INPUT_LEN = 7
REPORT_ID = 0xBB
BATTERY_OFFSET = 6
FRESH_SECONDS = 10.0


@dataclass
class VT1Info:
    online: bool = False
    level: Optional[int] = None
    name: str = "雷柏 VT1"
    detail: str = ""
    raw: bytes = b""

    @property
    def charging_text(self) -> str:
        return ""


_iface: Optional[HidInterface] = None
_latest: Optional[VT1Info] = None
_latest_ts = 0.0
_lock = threading.Lock()
_thread: Optional[threading.Thread] = None
_stop = threading.Event()


def _invalidate() -> None:
    global _iface
    _iface = None


def find_interface(refresh: bool = False) -> Optional[HidInterface]:
    """匹配 0xFF00:0x0002 中 **7 字节** 的那个接口（同 usage 还有一个 10 字节）。"""
    global _iface
    if _iface is not None and not refresh:
        return _iface
    _iface = None
    for it in hw.enumerate_hid():
        if (it.vid == VID and it.pid == PID
                and it.usage_page == USAGE_PAGE and it.usage == USAGE
                and it.input_len == INPUT_LEN):
            _iface = it
            return it
    return None


def _parse_report(r: bytes) -> VT1Info:
    info = VT1Info(online=True, raw=r)
    level = r[BATTERY_OFFSET]
    if 0 <= level <= 100:
        info.level = level
    else:
        info.detail = f"电量字节异常：0x{level:02X}"
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
            _invalidate()
            _stop.wait(1.0)


def _ensure_listener() -> None:
    global _thread
    if _thread is None or not _thread.is_alive():
        _thread = threading.Thread(target=_listen_loop, daemon=True,
                                   name="rapoo-vt1-listener")
        _thread.start()


def read_info(timeout: float = 6.5) -> VT1Info:
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
    with _lock:
        if _latest is not None:
            return _latest
    info = VT1Info(online=find_interface() is not None)
    info.detail = "暂无数据"
    return info


if __name__ == "__main__":
    r = read_info()
    print(f"{r.name}: {r.level}%" if r.level is not None else
          f"{r.name}: 读取失败 —— {r.detail}")
    if r.raw:
        print("  raw:", r.raw.hex(" "))
