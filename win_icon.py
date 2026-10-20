# -*- coding: utf-8 -*-
"""win_icon.py —— 用纯 Python 画托盘图标（不依赖 Pillow / 任何第三方库）

两种样式：
* 旧样式：3x5 点阵数字 + 可选圆角底板（`render_rgba`）。
* 新样式：**圆角正方形电量进度框 + 中间直角像素设备图标**
  （`render_tray_rgba`）。

新样式全部按整数像素直接绘制，**不做抗锯齿**，16px 下也不会发糊。
设备图标是 6–8px 的直角像素点阵（宽高均为偶数，保证整数居中），按整数倍放大。
ICO 内含 16/20/24/28/32/36/40/48/56/64 多种尺寸，并按系统托盘实际
尺寸加载，避免 Windows 缩放导致模糊。
"""

from __future__ import annotations

import ctypes
import math
import os
import struct
import tempfile
from ctypes import wintypes
from typing import Optional

# ---- 3x5 点阵数字（每行 3 位，"1" 点亮）----
DIGITS = {
    "0": ("111", "101", "101", "101", "111"),
    "1": ("010", "110", "010", "010", "111"),
    "2": ("111", "001", "111", "100", "111"),
    "3": ("111", "001", "111", "001", "111"),
    "4": ("101", "101", "111", "001", "001"),
    "5": ("111", "100", "111", "001", "111"),
    "6": ("111", "100", "111", "101", "111"),
    "7": ("111", "001", "001", "010", "010"),
    "8": ("111", "101", "111", "101", "111"),
    "9": ("111", "101", "111", "001", "111"),
    "%": ("101", "001", "010", "100", "101"),
    "-": ("000", "000", "111", "000", "000"),
}

GLYPH_W, GLYPH_H = 3, 5

# --------------------------------------------------------------------------
# 直角像素设备图标（# = 点亮，. = 透明）
# --------------------------------------------------------------------------
# 宽高都用偶数，保证在偶数尺寸的画布（16/20/24…）里能整数居中
DEVICE_PIXELS = {
    "mouse": (
        ".####.",
        "##..##",
        "######",
        "######",
        "######",
        "######",
        "######",
        ".####.",
    ),
    "keyboard": (
        "########",
        "#.#..#.#",
        "########",
        "#......#",
    ),
    "headset": (
        ".######.",
        "##....##",
        "#......#",
        "#......#",
        "#......#",
        "##....##",
    ),
}


# --------------------------------------------------------------------------
# 像素级绘制（硬边，不抗锯齿）
# --------------------------------------------------------------------------
def _put(px: bytearray, size: int, x: int, y: int, rgba) -> None:
    if 0 <= x < size and 0 <= y < size:
        i = (y * size + x) * 4
        px[i:i + 4] = bytes(rgba)


def _draw_pixel_device(px: bytearray, size: int, device: str,
                       cx: float, cy: float, scale: int, color) -> None:
    art = DEVICE_PIXELS.get(device)
    if not art:
        return
    h = len(art)
    w = max(len(r) for r in art)
    # 宽高、尺寸、缩放都是偶数 → 正好整数居中，不会偏半个像素
    ox = int(cx - w * scale / 2)
    oy = int(cy - h * scale / 2)
    for ry, row in enumerate(art):
        for rx, ch in enumerate(row):
            if ch != "#":
                continue
            for dy in range(scale):
                for dx in range(scale):
                    _put(px, size, ox + rx * scale + dx,
                         oy + ry * scale + dy, (*color, 255))


def _rounded_rect_pixels(size: int, margin: float, radius: float,
                         thick: float):
    """圆角正方形边框像素：(x, y, 角度)。角度以正上方为 0、顺时针增加。"""
    cx = cy = size / 2.0
    hw = hh = size / 2.0 - margin
    r = min(radius, hw - 0.5, hh - 0.5)
    out = []
    for y in range(size):
        for x in range(size):
            dx = x + 0.5 - cx
            dy = y + 0.5 - cy
            qx = abs(dx) - (hw - r)
            qy = abs(dy) - (hh - r)
            dist = (min(max(qx, qy), 0.0)
                    + math.hypot(max(qx, 0.0), max(qy, 0.0)) - r)
            # 只取边界内侧的 thick 个像素，形成 1/2/3px 的细环
            if -thick <= dist <= 0.05:
                ang = (math.degrees(math.atan2(dy, dx)) + 90.0) % 360.0
                out.append((x, y, ang))
    return out


# --------------------------------------------------------------------------
# 旧样式：点阵数字 + 可选圆角底板
# --------------------------------------------------------------------------
def render_rgba(text: str, size: int, color: tuple[int, int, int],
                plate: bool = False, plate_color=(28, 30, 34, 235)) -> bytes:
    """把文字渲染成 size×size 的 RGBA 像素（返回 size*size*4 字节，行优先）。"""
    n = len(text)
    gap = 1
    scale = 1
    while True:
        w = n * GLYPH_W * (scale + 1) + (n - 1) * gap
        h = GLYPH_H * (scale + 1)
        if w > size - 2 or h > size - 2:
            break
        scale += 1

    gw, gh = GLYPH_W * scale, GLYPH_H * scale
    total_w = n * gw + (n - 1) * gap
    total_h = gh
    ox = (size - total_w) // 2
    oy = (size - total_h) // 2

    px = bytearray(size * size * 4)

    def put(x: int, y: int, rgba):
        if 0 <= x < size and 0 <= y < size:
            i = (y * size + x) * 4
            px[i:i + 4] = bytes(rgba)

    def get(x: int, y: int):
        i = (y * size + x) * 4
        return (px[i], px[i + 1], px[i + 2], px[i + 3])

    if plate:
        r = max(1, size // 8)
        for y in range(size):
            for x in range(size):
                cx = min(x, size - 1 - x)
                cy = min(y, size - 1 - y)
                if cx + cy < r and cx < r and cy < r:
                    continue
                put(x, y, plate_color)

    for k, ch in enumerate(text):
        glyph = DIGITS.get(ch)
        if not glyph:
            continue
        gx = ox + k * (gw + gap)
        for gy, row in enumerate(glyph):
            for gxx, bit in enumerate(row):
                if bit != "1":
                    continue
                for dy in range(scale):
                    for dx in range(scale):
                        put(gx + gxx * scale + dx, oy + gy * scale + dy,
                            (*color, 255))

    outline = (16, 17, 19, 190)
    edge = []
    for y in range(size):
        for x in range(size):
            if get(x, y)[3] == 0:
                continue
            for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                nx, ny = x + dx, y + dy
                if 0 <= nx < size and 0 <= ny < size and get(nx, ny)[3] == 0:
                    edge.append((nx, ny))
    for x, y in edge:
        if get(x, y)[3] == 0:
            put(x, y, outline)
    return bytes(px)


# --------------------------------------------------------------------------
# 新样式：圆角正方形电量进度框 + 中间直角像素设备图标
# --------------------------------------------------------------------------
def render_tray_rgba(text: str, size: int, color: tuple[int, int, int],
                     device: Optional[str] = None, show_device: bool = False,
                     plate: bool = False) -> bytes:
    """圆角正方形电量框；中间按开关显示设备图标或退回数字图标。

    * ``show_device=False``（或 ``device`` 为空）时退回旧的点阵数字样式。
    * ``show_device=True``：圆角正方形进度框 + 中间直角像素设备图标。
    """
    if device is None or not show_device:
        return render_rgba(text, size, color, plate)

    px = bytearray(size * size * 4)
    thick = 1 if size <= 24 else (2 if size <= 48 else 3)
    margin = 1.0
    radius = max(2.0, size * 0.14)

    border = _rounded_rect_pixels(size, margin, radius, thick)
    track = (78, 82, 88, 255)
    for x, y, _ in border:
        _put(px, size, x, y, track)

    try:
        level = max(0, min(100, int(text)))
    except (TypeError, ValueError):
        level = None
    if level:
        limit = 360.0 * level / 100.0
        for x, y, ang in border:
            if ang <= limit:
                _put(px, size, x, y, (*color, 255))

    # 中间设备图标：5–7px 点阵，按尺寸整数倍放大，保持"小图标"
    scale = 1 if size <= 24 else (2 if size < 56 else (3 if size < 64 else 4))
    _draw_pixel_device(px, size, device, size / 2.0, size / 2.0,
                       scale, color)
    return bytes(px)


def build_ico(pngs: list[tuple[int, bytes]]) -> bytes:
    """把多张 RGBA 位图打包成 ICO 文件内容。"""
    entries = []
    blobs = []
    offset = 6 + 16 * len(pngs)
    for size, rgba in pngs:
        bih = struct.pack("<IiiHHIIiiII", 40, size, size * 2, 1, 32, 0,
                          size * size * 4, 0, 0, 0, 0)
        xor = bytearray()
        for y in range(size - 1, -1, -1):
            row = rgba[y * size * 4:(y + 1) * size * 4]
            for x in range(size):
                r, g, b, a = row[x * 4:x * 4 + 4]
                xor += bytes((b, g, r, a))
        row_bytes = ((size + 31) // 32) * 4
        mask = b"\x00" * (row_bytes * size)
        blob = bih + bytes(xor) + mask
        blobs.append(blob)
        entries.append(struct.pack("<BBBBHHII", size if size < 256 else 0,
                                   size if size < 256 else 0, 0, 0, 1, 32,
                                   len(blob), offset))
        offset += len(blob)
    return (struct.pack("<HHH", 0, 1, len(pngs)) + b"".join(entries)
            + b"".join(blobs))


# ---------------- HICON 缓存 ----------------
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
user32 = ctypes.WinDLL("user32", use_last_error=True)

IMAGE_ICON = 1
LR_LOADFROMFILE = 0x0010

user32.LoadImageW.restype = ctypes.c_void_p
user32.LoadImageW.argtypes = [ctypes.c_void_p, wintypes.LPCWSTR, wintypes.UINT,
                              ctypes.c_int, ctypes.c_int, wintypes.UINT]
user32.DestroyIcon.restype = wintypes.BOOL
user32.DestroyIcon.argtypes = [ctypes.c_void_p]
user32.GetSystemMetrics.restype = ctypes.c_int
user32.GetSystemMetrics.argtypes = [ctypes.c_int]

SM_CXSMICON, SM_CYSMICON = 49, 50

# 常见 DPI 下的托盘图标尺寸（100% → 16，125% → 20 … 400% → 64）
SIZES = (16, 20, 24, 28, 32, 36, 40, 48, 56, 64)
_icon_cache: dict[tuple, int] = {}
_tmp_dir: Optional[str] = None


def _ensure_tmp() -> str:
    global _tmp_dir
    if _tmp_dir is None:
        _tmp_dir = os.path.join(tempfile.gettempdir(), "device_battery_icons")
        os.makedirs(_tmp_dir, exist_ok=True)
    return _tmp_dir


def make_hicon(text: str, color: tuple[int, int, int], plate: bool = False,
               device: Optional[str] = None,
               show_device: bool = False) -> int:
    """按文字+颜色(+设备图标)生成（带缓存的）HICON；失败返回 0。"""
    # 按系统托盘图标的实际尺寸加载，避免 Windows 把 32px 缩放成 16px 发糊
    sw = user32.GetSystemMetrics(SM_CXSMICON) or 16
    sh = user32.GetSystemMetrics(SM_CYSMICON) or sw
    key = (text, color, plate, device, show_device, sw, sh)
    if key in _icon_cache:
        return _icon_cache[key]
    images = [(s, render_tray_rgba(text, s, color, device, show_device, plate))
              for s in SIZES]
    ico = build_ico(images)
    path = os.path.join(
        _ensure_tmp(),
        f"ico_{text}_{color[0]:02x}{color[1]:02x}{color[2]:02x}"
        f"_{int(plate)}_{device or 'none'}_{int(show_device)}.ico")
    try:
        with open(path, "wb") as f:
            f.write(ico)
    except OSError:
        return 0
    h = user32.LoadImageW(None, path, IMAGE_ICON, sw, sh, LR_LOADFROMFILE)
    hicon = int(h) if h else 0
    _icon_cache[key] = hicon
    return hicon


def destroy_all() -> None:
    for h in _icon_cache.values():
        if h:
            try:
                user32.DestroyIcon(ctypes.c_void_p(h))
            except Exception:
                pass
    _icon_cache.clear()


def cleanup_files() -> None:
    """清掉临时目录里的 ico 文件。"""
    global _tmp_dir
    if _tmp_dir and os.path.isdir(_tmp_dir):
        for name in os.listdir(_tmp_dir):
            try:
                os.remove(os.path.join(_tmp_dir, name))
            except OSError:
                pass


# --------------------------------------------------------------------------
# 预览（开发用）：生成 ICO + PNG，方便肉眼检查
# --------------------------------------------------------------------------
def _write_png(path: str, size: int, rgba: bytes) -> None:
    import zlib

    raw = b"".join(b"\x00" + rgba[y * size * 4:(y + 1) * size * 4]
                   for y in range(size))

    def chunk(tag: bytes, data: bytes) -> bytes:
        return (struct.pack(">I", len(data)) + tag + data
                + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(raw, 9))
           + chunk(b"IEND", b""))
    with open(path, "wb") as f:
        f.write(png)


if __name__ == "__main__":
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                       "_icon_preview")
    os.makedirs(out, exist_ok=True)
    cases = [
        ("100", (93, 220, 138), "mouse", True, "mouse_on"),
        ("100", (93, 220, 138), "keyboard", True, "keyboard_on"),
        ("95", (93, 220, 138), "headset", True, "headset_on"),
        ("25", (255, 107, 107), "mouse", True, "mouse_low"),
        ("82", (93, 220, 138), "mouse", False, "mouse_off"),
        ("25", (255, 107, 107), "keyboard", False, "keyboard_off"),
        ("--", (150, 155, 160), "headset", False, "headset_off"),
    ]
    for text, color, device, show, name in cases:
        imgs = [(s, render_tray_rgba(text, s, color, device, show))
                for s in SIZES]
        with open(os.path.join(out, name + ".ico"), "wb") as f:
            f.write(build_ico(imgs))
        for size, rgba in imgs:
            _write_png(os.path.join(out, f"{name}_{size}.png"), size, rgba)
        print("生成", name)
