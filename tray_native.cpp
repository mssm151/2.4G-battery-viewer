// tray_native.cpp —— 原生 Win32 托盘电量监视器
//
// 与 Python 面板共用：
//   * 窗口类 DeviceBatteryTrayWnd
//   * 命名互斥 Global\DeviceBatteryTray_single_instance
//   * 消息 DeviceBatteryTray_Reload / DeviceBatteryTray_Quit
//   * 配置 tray_config.json、PID 文件 tray_pid.txt
//
// 设备协议：
//   雷柏 VT7   24AE:1413  0xFF00:0x0002  Report 0x07（设备主动推送）
//   惠普 GT87  3554:FA09  0xFF02:0x0002  Report 0x13（发 13 05 01 00）
//   ROG 破风   0B05:1B84  0xFF00:0x0001  Report 0xCC（发 CC 12 07 00 00）
//
// 编译：build_tray.ps1（MinGW g++，静态链接，无运行时依赖）

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <hidpi.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <string>
#include <vector>

#ifndef HIDP_STATUS_SUCCESS
#define HIDP_STATUS_SUCCESS 0x00110000
#endif

static const wchar_t* kClassName = L"DeviceBatteryTrayWnd";
static const wchar_t* kAppTitle = L"DeviceBatteryTray";
static const wchar_t* kMutexName =
    L"Global\\DeviceBatteryTray_single_instance";
static const wchar_t* kReloadMsgName = L"DeviceBatteryTray_Reload";
static const wchar_t* kQuitMsgName = L"DeviceBatteryTray_Quit";
static const wchar_t* kUiTitle = L"我的设备电量";

static const UINT WM_TRAY = WM_APP + 1;
static const UINT WM_REFRESH_DONE = WM_APP + 2;
static const UINT WM_RAPOO_UPDATE = WM_APP + 3;
static const UINT_PTR TIMER_ID = 1;

static const GUID kHidGuid = {
    0x4D1E55B2, 0xF16F, 0x11CF,
    {0x88, 0xCB, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30}};

struct DeviceState {
    bool online = false;
    int level = -1;          // 0..100，-1 = 未知
    int charging = -1;       // -1 未知 / 0 未充电 / 1 充电中 / 2 已充满
    std::wstring status;
    std::wstring detail;
};

struct Config {
    int interval = 60;
    int ring[3] = {0, 1, 2};   // 内/中/外 -> 0=rapoo, 1=hp, 2=asus
    std::wstring pythonw;
};

struct Rgb {
    int r, g, b;
};

static std::wstring g_dir;
static Config g_cfg;
static HWND g_hwnd = nullptr;
static HINSTANCE g_inst = nullptr;
static bool g_debug = false;

static UINT g_taskbar_created = 0;
static UINT g_msg_reload = 0;
static UINT g_msg_quit = 0;

static HANDLE g_refresh_event = nullptr;
static HANDLE g_quit_event = nullptr;
static HANDLE g_refresh_thread = nullptr;
static HANDLE g_listener_thread = nullptr;

static std::mutex g_state_mu;
static DeviceState g_states[3];

static std::mutex g_rapoo_mu;
static DeviceState g_rapoo_state;
static ULONGLONG g_rapoo_ts = 0;
static bool g_rapoo_has = false;
static std::wstring g_rapoo_path;   // 仅监听线程访问

static std::wstring g_hp_path;      // 仅刷新线程访问
static std::wstring g_asus_path;

// --------------------------------------------------------------------------
// 字符串 / 调试
// --------------------------------------------------------------------------
static std::string wide_to_utf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                                nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                        s.data(), n, nullptr, nullptr);
    return s;
}

static std::wstring utf8_to_wide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(),
                                nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(),
                        w.data(), n);
    return w;
}

static void debug_log(const wchar_t* fmt, ...) {
    if (!g_debug) return;
    wchar_t buf[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(buf, 1023, fmt, ap);
    va_end(ap);
    buf[1023] = 0;
    OutputDebugStringW(buf);
    OutputDebugStringW(L"\n");
    HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
    if (!h || h == INVALID_HANDLE_VALUE) return;
    if (GetFileType(h) == FILE_TYPE_CHAR) {
        DWORD n = 0;
        WriteConsoleW(h, buf, (DWORD)wcslen(buf), &n, nullptr);
        WriteConsoleW(h, L"\r\n", 2, &n, nullptr);
    } else {
        std::string s = wide_to_utf8(buf) + "\r\n";
        DWORD n = 0;
        WriteFile(h, s.data(), (DWORD)s.size(), &n, nullptr);
    }
}

static void print_line(const std::wstring& s) {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!h || h == INVALID_HANDLE_VALUE) {
        AllocConsole();
        h = GetStdHandle(STD_OUTPUT_HANDLE);
    }
    if (!h || h == INVALID_HANDLE_VALUE) return;
    if (GetFileType(h) == FILE_TYPE_CHAR) {
        DWORD n = 0;
        WriteConsoleW(h, s.c_str(), (DWORD)s.size(), &n, nullptr);
        WriteConsoleW(h, L"\r\n", 2, &n, nullptr);
    } else {
        std::string u = wide_to_utf8(s) + "\r\n";
        DWORD n = 0;
        WriteFile(h, u.data(), (DWORD)u.size(), &n, nullptr);
    }
}

// --------------------------------------------------------------------------
// 路径 / 配置 / PID
// --------------------------------------------------------------------------
static std::wstring module_dir() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring p(buf, n);
    size_t pos = p.find_last_of(L"\\/");
    return pos == std::wstring::npos ? L"." : p.substr(0, pos);
}

static std::string read_file(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    LARGE_INTEGER sz{};
    GetFileSizeEx(h, &sz);
    std::string s;
    s.resize((size_t)sz.QuadPart);
    DWORD got = 0;
    if (!s.empty()) ReadFile(h, s.data(), (DWORD)s.size(), &got, nullptr);
    CloseHandle(h);
    s.resize(got);
    return s;
}

static void write_text(const std::wstring& path, const std::string& text) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD n = 0;
    WriteFile(h, text.data(), (DWORD)text.size(), &n, nullptr);
    CloseHandle(h);
}

static std::wstring pid_path() { return g_dir + L"\\tray_pid.txt"; }
static std::wstring config_path() { return g_dir + L"\\tray_config.json"; }

static size_t find_key(const std::string& s, const char* key) {
    return s.find(std::string("\"") + key + "\"");
}

static int parse_ring(const std::string& s, const char* key, int fallback) {
    size_t p = find_key(s, key);
    if (p == std::string::npos) return fallback;
    p = s.find(':', p);
    if (p == std::string::npos) return fallback;
    p = s.find('"', p);
    if (p == std::string::npos) return fallback;
    size_t q = s.find('"', p + 1);
    if (q == std::string::npos) return fallback;
    std::string v = s.substr(p + 1, q - p - 1);
    if (v == "rapoo") return 0;
    if (v == "hp") return 1;
    if (v == "asus") return 2;
    return fallback;
}

static void load_config(Config& c) {
    c = Config{};
    std::string s = read_file(config_path());
    if (!s.empty()) {
        size_t p = find_key(s, "interval");
        if (p != std::string::npos) {
            p = s.find(':', p);
            if (p != std::string::npos) c.interval = atoi(s.c_str() + p + 1);
        }
        c.ring[0] = parse_ring(s, "ring_inner", 0);
        c.ring[1] = parse_ring(s, "ring_middle", 1);
        c.ring[2] = parse_ring(s, "ring_outer", 2);
        bool seen[3] = {false, false, false};
        bool ok = true;
        for (int i = 0; i < 3; ++i) {
            int v = c.ring[i];
            if (v < 0 || v > 2 || seen[v]) { ok = false; break; }
            seen[v] = true;
        }
        if (!ok) {
            c.ring[0] = 0;
            c.ring[1] = 1;
            c.ring[2] = 2;
        }
        p = find_key(s, "pythonw");
        if (p != std::string::npos) {
            p = s.find(':', p);
            if (p != std::string::npos) {
                p = s.find('"', p);
                if (p != std::string::npos) {
                    size_t q = s.find('"', p + 1);
                    if (q != std::string::npos) {
                        std::string v = s.substr(p + 1, q - p - 1);
                        std::string out;
                        for (size_t i = 0; i < v.size(); ++i) {
                            if (v[i] == '\\' && i + 1 < v.size()
                                && (v[i + 1] == '\\' || v[i + 1] == '"')) {
                                out += v[++i];
                            } else {
                                out += v[i];
                            }
                        }
                        c.pythonw = utf8_to_wide(out);
                    }
                }
            }
        }
    }
    if (c.interval < 5) c.interval = 60;
}

static void write_pid() {
    write_text(pid_path(), std::to_string(GetCurrentProcessId()));
}

static void remove_pid() { DeleteFileW(pid_path().c_str()); }

static int read_pid() {
    std::string s = read_file(pid_path());
    return atoi(s.c_str());
}

static void cleanup_native_zombie() {
    int pid = read_pid();
    if (pid <= 0 || pid == (int)GetCurrentProcessId()) return;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE,
                           FALSE, (DWORD)pid);
    if (!h) {
        remove_pid();
        return;
    }
    wchar_t img[MAX_PATH] = {};
    DWORD len = MAX_PATH;
    bool ok = QueryFullProcessImageNameW(h, 0, img, &len);
    std::wstring name = ok ? std::wstring(img, len) : L"";
    size_t pos = name.find_last_of(L"\\/");
    std::wstring base = pos == std::wstring::npos ? name : name.substr(pos + 1);
    if (_wcsicmp(base.c_str(), L"tray_native.exe") == 0) {
        debug_log(L"清理无窗口的原生托盘 PID %d", pid);
        TerminateProcess(h, 1);
        WaitForSingleObject(h, 2000);
    }
    CloseHandle(h);
    remove_pid();
}

// --------------------------------------------------------------------------
// HID
// --------------------------------------------------------------------------
static std::wstring find_hid_path(USHORT vid, USHORT pid,
                                  USHORT usage_page, USHORT usage) {
    HDEVINFO devs = SetupDiGetClassDevsW(
        &kHidGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devs == INVALID_HANDLE_VALUE) return L"";

    std::wstring found;
    for (DWORD i = 0;; ++i) {
        SP_DEVICE_INTERFACE_DATA did{};
        did.cbSize = sizeof(did);
        if (!SetupDiEnumDeviceInterfaces(devs, nullptr, &kHidGuid, i, &did))
            break;

        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(devs, &did, nullptr, 0, &need,
                                         nullptr);
        if (!need) continue;

        std::vector<BYTE> buf(need);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(
            buf.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(devs, &did, detail, need,
                                              nullptr, nullptr))
            continue;

        std::wstring path = detail->DevicePath;
        HANDLE h = CreateFileW(path.c_str(), 0,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;

        bool match = false;
        HIDD_ATTRIBUTES attr{};
        attr.Size = sizeof(attr);
        if (HidD_GetAttributes(h, &attr) && attr.VendorID == vid
            && attr.ProductID == pid) {
            PHIDP_PREPARSED_DATA pp = nullptr;
            if (HidD_GetPreparsedData(h, &pp) && pp) {
                HIDP_CAPS caps{};
                if (HidP_GetCaps(pp, &caps) == HIDP_STATUS_SUCCESS
                    && caps.UsagePage == usage_page
                    && caps.Usage == usage) {
                    match = true;
                }
                HidD_FreePreparsedData(pp);
            }
        }
        CloseHandle(h);
        if (match) {
            found = path;
            break;
        }
    }
    SetupDiDestroyDeviceInfoList(devs);
    return found;
}

static HANDLE open_read(const std::wstring& path) {
    return CreateFileW(path.c_str(), GENERIC_READ,
                       FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                       OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
}

static HANDLE open_write(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        h = CreateFileW(path.c_str(), GENERIC_WRITE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                        OPEN_EXISTING, 0, nullptr);
    return h;
}

static bool read_report(HANDLE h, BYTE* buf, DWORD len, DWORD timeout_ms,
                        DWORD* got) {
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent) return false;

    *got = 0;
    BOOL ok = ReadFile(h, buf, len, nullptr, &ov);
    DWORD err = ok ? ERROR_SUCCESS : GetLastError();
    if (!ok && err != ERROR_IO_PENDING) {
        CloseHandle(ov.hEvent);
        return false;
    }
    DWORD w = WaitForSingleObject(ov.hEvent, ok ? 0 : timeout_ms);
    if (w == WAIT_OBJECT_0) {
        ok = GetOverlappedResult(h, &ov, got, FALSE);
    } else {
        CancelIoEx(h, &ov);
        WaitForSingleObject(ov.hEvent, 100);
        DWORD n = 0;
        GetOverlappedResult(h, &ov, &n, FALSE);
        ok = FALSE;
    }
    CloseHandle(ov.hEvent);
    return ok && *got > 0;
}

static bool send_report(HANDLE h, const BYTE* data, DWORD len) {
    if (HidD_SetOutputReport(h, (PVOID)data, len)) return true;
    DWORD written = 0;
    return WriteFile(h, data, len, &written, nullptr) && written == len;
}

// --------------------------------------------------------------------------
// 设备读取
// --------------------------------------------------------------------------
static DeviceState make_offline(const wchar_t* detail) {
    DeviceState s;
    s.detail = detail;
    return s;
}

static DeviceState read_rapoo(DWORD timeout_ms) {
    ULONGLONG deadline = GetTickCount64() + timeout_ms;
    while (true) {
        {
            std::lock_guard<std::mutex> lk(g_rapoo_mu);
            if (g_rapoo_has && GetTickCount64() - g_rapoo_ts <= 10000)
                return g_rapoo_state;
        }
        if (GetTickCount64() >= deadline) break;
        Sleep(50);
    }
    {
        std::lock_guard<std::mutex> lk(g_rapoo_mu);
        if (g_rapoo_has) return g_rapoo_state;   // 用最后一次已知值
    }
    return make_offline(L"没收到雷柏状态报文（请动一下鼠标）");
}

static DeviceState read_hp() {
    if (g_hp_path.empty())
        g_hp_path = find_hid_path(0x3554, 0xFA09, 0xFF02, 0x0002);
    if (g_hp_path.empty())
        return make_offline(L"没找到惠普 GT87 接收器（请插好 2.4G dongle）");

    HANDLE wr = open_write(g_hp_path);
    HANDLE rd = open_read(g_hp_path);
    if (wr == INVALID_HANDLE_VALUE || rd == INVALID_HANDLE_VALUE) {
        if (wr != INVALID_HANDLE_VALUE) CloseHandle(wr);
        if (rd != INVALID_HANDLE_VALUE) CloseHandle(rd);
        g_hp_path.clear();
        return make_offline(L"打开惠普接收器失败");
    }

    DeviceState s;
    s.online = true;
    BYTE out[20] = {0x13, 0x05, 0x01, 0x00};
    BYTE buf[128];
    for (int attempt = 0; attempt < 2 && s.level < 0; ++attempt) {
        if (!send_report(wr, out, sizeof(out))) break;
        ULONGLONG deadline = GetTickCount64() + 800;
        while (GetTickCount64() < deadline) {
            DWORD got = 0;
            if (!read_report(rd, buf, sizeof(buf), 150, &got)) continue;
            if (got >= 8 && buf[0] == 0x13 && buf[1] == 0x0A) {
                if (buf[2] != 1) {
                    s.detail = L"接收器返回错误状态";
                    break;
                }
                int len = buf[4];
                if (len >= 2 && got >= (DWORD)(5 + len)) {
                    int level = buf[6];
                    if (level >= 0 && level <= 100) s.level = level;
                    else s.detail = L"电量字节异常";
                    int st = buf[7];
                    s.charging = (st & 0x10) ? 1 : 0;
                    s.status = s.charging ? L"充电中" : L"未充电";
                    break;
                }
            }
            if (buf[0] == 0x13 && buf[1] == 0x0A) break;
        }
    }
    CloseHandle(rd);
    CloseHandle(wr);
    if (s.level < 0 && s.detail.empty())
        s.detail = L"接收器没有回复（请按一下键盘唤醒）";
    return s;
}

static DeviceState read_asus() {
    const USHORT pids[] = {0x1B84, 0x1B82, 0x1B86};
    if (g_asus_path.empty()) {
        for (USHORT pid : pids) {
            g_asus_path = find_hid_path(0x0B05, pid, 0xFF00, 0x0001);
            if (!g_asus_path.empty()) break;
        }
    }
    if (g_asus_path.empty())
        return make_offline(L"没找到 ROG 破风接收器（请插好 2.4G dongle）");

    HANDLE wr = open_write(g_asus_path);
    HANDLE rd = open_read(g_asus_path);
    if (wr == INVALID_HANDLE_VALUE || rd == INVALID_HANDLE_VALUE) {
        if (wr != INVALID_HANDLE_VALUE) CloseHandle(wr);
        if (rd != INVALID_HANDLE_VALUE) CloseHandle(rd);
        g_asus_path.clear();
        return make_offline(L"打开 ROG 接收器失败");
    }

    DeviceState s;
    s.online = true;
    BYTE out[65] = {0xCC, 0x12, 0x07, 0x00, 0x00};
    BYTE buf[128];
    for (int attempt = 0; attempt < 2 && s.level < 0; ++attempt) {
        if (!send_report(wr, out, sizeof(out))) break;
        ULONGLONG deadline = GetTickCount64() + 800;
        while (GetTickCount64() < deadline) {
            DWORD got = 0;
            if (!read_report(rd, buf, sizeof(buf), 150, &got)) continue;
            if (got < 2) continue;
            const BYTE* p = (buf[0] == 0xCC) ? buf + 1 : buf;
            if (p[0] == 0x12 && p[1] == 0x07 && got >= 7) {
                int level = p[5];
                if (level == 0xFE) {
                    s.detail = L"耳机上报电量为无效值";
                } else if (level <= 100) {
                    s.level = level;
                } else {
                    s.detail = L"电量字节异常";
                }
                break;
            }
        }
    }

    if (s.level >= 0) {
        BYTE q[65] = {0xCC, 0x12, 0x28, 0x00, 0x00};
        if (send_report(wr, q, sizeof(q))) {
            ULONGLONG deadline = GetTickCount64() + 800;
            while (GetTickCount64() < deadline) {
                DWORD got = 0;
                if (!read_report(rd, buf, sizeof(buf), 150, &got)) continue;
                if (got < 2) continue;
                const BYTE* p = (buf[0] == 0xCC) ? buf + 1 : buf;
                if (p[0] == 0x12 && p[1] == 0x28 && got >= 6) {
                    int raw = p[4];
                    if (raw == 0) { s.charging = 2; s.status = L"已充满"; }
                    else if (raw == 1) { s.charging = 0; s.status = L"未充电"; }
                    else if (raw == 2) { s.charging = 1; s.status = L"充电中"; }
                    break;
                }
            }
        }
    }

    CloseHandle(rd);
    CloseHandle(wr);
    if (s.level < 0 && s.detail.empty())
        s.detail = L"耳机没有回复（请确认耳机已开机）";
    return s;
}

// --------------------------------------------------------------------------
// 图标绘制（硬边像素，与 Python 版一致）
// --------------------------------------------------------------------------
static void put_px(std::vector<BYTE>& px, int size, int x, int y,
                   Rgb c, BYTE a = 255) {
    if (x < 0 || y < 0 || x >= size || y >= size) return;
    size_t i = ((size_t)y * size + x) * 4;
    px[i + 0] = (BYTE)c.b;
    px[i + 1] = (BYTE)c.g;
    px[i + 2] = (BYTE)c.r;
    px[i + 3] = a;
}

static Rgb level_color(int level) {
    if (level < 0) return {150, 155, 160};
    if (level >= 60) return {93, 220, 138};
    if (level >= 30) return {245, 196, 81};
    return {255, 107, 107};
}

static void draw_multi_rings(std::vector<BYTE>& px, int size,
                             const int levels[3]) {
    // levels = [内圈, 中圈, 外圈]，各自独立的 0-100 进度
    int thick = size <= 24 ? 1 : (size <= 48 ? 2 : 3);
    int gap = size <= 24 ? 1 : (size <= 48 ? 2 : 4);
    double margins[3] = {
        1.0,
        1.0 + thick + gap,
        1.0 + 2.0 * (thick + gap),
    };
    for (int layer = 0; layer < 3; ++layer) {
        double margin = margins[2 - layer];
        double cx = size / 2.0, cy = size / 2.0;
        double hw = size / 2.0 - margin;
        double r = std::max(2.0, (size / 2.0 - margin) * 0.25);
        r = std::min(r, hw - 0.5);
        Rgb track{78, 82, 88};
        Rgb color = level_color(levels[layer]);
        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                double dx = x + 0.5 - cx;
                double dy = y + 0.5 - cy;
                double qx = std::abs(dx) - (hw - r);
                double qy = std::abs(dy) - (hw - r);
                double dist = std::min(std::max(qx, qy), 0.0)
                              + std::hypot(std::max(qx, 0.0),
                                           std::max(qy, 0.0)) - r;
                if (dist < -thick || dist > 0.05) continue;
                double ang = std::atan2(dy, dx) * 180.0
                             / 3.141592653589793 + 90.0;
                if (ang < 0) ang += 360.0;
                bool lit = levels[layer] > 0
                           && ang <= 3.6 * levels[layer];
                put_px(px, size, x, y, lit ? color : track);
            }
        }
    }
}

static HICON make_icon(int size, const int levels[3]) {
    std::vector<BYTE> px((size_t)size * size * 4, 0);
    draw_multi_rings(px, size, levels);

    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = size;
    bi.bV5Height = -size;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;

    void* bits = nullptr;
    HBITMAP color_bmp = CreateDIBSection(
        nullptr, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS,
        &bits, nullptr, 0);
    if (!color_bmp) return nullptr;
    memcpy(bits, px.data(), px.size());

    int stride = ((size + 15) / 16) * 2;
    std::vector<BYTE> mask_bits((size_t)stride * size, 0);
    HBITMAP mask_bmp = CreateBitmap(size, size, 1, 1, mask_bits.data());
    ICONINFO ii{};
    ii.fIcon = TRUE;
    ii.hbmMask = mask_bmp;
    ii.hbmColor = color_bmp;
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(mask_bmp);
    DeleteObject(color_bmp);
    return icon;
}

// --------------------------------------------------------------------------
// 托盘
// --------------------------------------------------------------------------
struct Item {
    HICON icon = nullptr;
    bool owned = false;
    bool added = false;
    int last_levels[3] = {-999, -999, -999};
    int last_ring[3] = {-1, -1, -1};
    int last_size = 0;
};

static Item g_item;
static const wchar_t* kDeviceNames[3] = {
    L"雷柏 VT7", L"惠普 GT87", L"ROG 破风",
};

static NOTIFYICONDATAW make_nid() {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = WM_TRAY;
    return nid;
}

static void build_tip(const DeviceState st[3], wchar_t* tip, size_t cap) {
    std::wstring t;
    for (int layer = 0; layer < 3; ++layer) {
        int dev = g_cfg.ring[layer];
        const DeviceState& s = st[dev];
        t += kDeviceNames[dev];
        t += L"：";
        if (s.level >= 0) {
            t += std::to_wstring(s.level) + L"%";
            if (!s.status.empty()) {
                t += L" ";
                t += s.status;
            }
        } else {
            t += L"暂无数据";
        }
        if (layer < 2) t += L"\n";
    }
    wcsncpy(tip, t.c_str(), cap - 1);
    tip[cap - 1] = 0;
}

static void update_icons() {
    if (!g_hwnd) return;
    DeviceState st[3];
    {
        std::lock_guard<std::mutex> lk(g_state_mu);
        for (int i = 0; i < 3; ++i) st[i] = g_states[i];
    }
    int levels[3];
    for (int layer = 0; layer < 3; ++layer)
        levels[layer] = st[g_cfg.ring[layer]].level;

    int size = GetSystemMetrics(SM_CXSMICON);
    if (size < 16) size = 16;

    bool need = !g_item.icon
                || memcmp(levels, g_item.last_levels, sizeof(levels)) != 0
                || memcmp(g_cfg.ring, g_item.last_ring,
                          sizeof(g_cfg.ring)) != 0
                || size != g_item.last_size;
    HICON old = nullptr;
    bool old_owned = g_item.owned;
    if (need) {
        HICON h = make_icon(size, levels);
        if (h) {
            old = g_item.icon;
            g_item.icon = h;
            g_item.owned = true;
            memcpy(g_item.last_levels, levels, sizeof(levels));
            memcpy(g_item.last_ring, g_cfg.ring, sizeof(g_cfg.ring));
            g_item.last_size = size;
        }
    }
    if (!g_item.icon) {
        g_item.icon = LoadIconW(nullptr, IDI_APPLICATION);
        g_item.owned = false;
    }

    NOTIFYICONDATAW nid = make_nid();
    nid.hIcon = g_item.icon;
    build_tip(st, nid.szTip, 128);
    if (!g_item.added) {
        g_item.added = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
        debug_log(L"NIM_ADD 三环 -> %d", g_item.added ? 1 : 0);
    } else {
        Shell_NotifyIconW(NIM_MODIFY, &nid);
    }
    if (old && old_owned) DestroyIcon(old);
}

static void remove_icons() {
    if (g_item.added) {
        NOTIFYICONDATAW nid = make_nid();
        nid.uFlags = 0;
        Shell_NotifyIconW(NIM_DELETE, &nid);
        g_item.added = false;
    }
    if (g_item.icon) {
        if (g_item.owned) DestroyIcon(g_item.icon);
        g_item.icon = nullptr;
        g_item.owned = false;
    }
}

// --------------------------------------------------------------------------
// 线程
// --------------------------------------------------------------------------
static DWORD WINAPI listener_thread(LPVOID) {
    double backoff = 1.0;
    while (WaitForSingleObject(g_quit_event, 0) != WAIT_OBJECT_0) {
        if (g_rapoo_path.empty()) {
            g_rapoo_path = find_hid_path(0x24AE, 0x1413, 0xFF00, 0x0002);
            debug_log(L"Rapoo HID path: %ls", g_rapoo_path.c_str());
        }
        if (g_rapoo_path.empty()) {
            WaitForSingleObject(g_quit_event, (DWORD)(backoff * 1000));
            backoff = std::min(backoff * 2, 30.0);
            continue;
        }
        backoff = 1.0;
        HANDLE h = open_read(g_rapoo_path);
        if (h == INVALID_HANDLE_VALUE) {
            debug_log(L"Rapoo open failed: %lu", GetLastError());
            g_rapoo_path.clear();
            WaitForSingleObject(g_quit_event, 1000);
            continue;
        }
        debug_log(L"Rapoo listener opened");
        while (WaitForSingleObject(g_quit_event, 0) != WAIT_OBJECT_0) {
            BYTE buf[64];
            DWORD got = 0;
            ULONGLONG t0 = GetTickCount64();
            if (read_report(h, buf, sizeof(buf), 500, &got)) {
                debug_log(L"Rapoo report len=%lu id=0x%02X", got, buf[0]);
                if (got > 8 && buf[0] == 0x07) {
                    DeviceState s;
                    s.online = true;
                    int level = buf[8];
                    s.level = (level <= 100) ? level : -1;
                    int ch = buf[7];
                    s.charging = ch;
                    s.status = ch == 2 ? L"充电中"
                               : ch == 1 ? L"未充电" : L"";
                    {
                        std::lock_guard<std::mutex> lk(g_rapoo_mu);
                        g_rapoo_state = s;
                        g_rapoo_ts = GetTickCount64();
                        g_rapoo_has = true;
                    }
                    {
                        std::lock_guard<std::mutex> lk(g_state_mu);
                        g_states[0] = s;
                    }
                    if (g_hwnd)
                        PostMessageW(g_hwnd, WM_RAPOO_UPDATE, 0, 0);
                }
            } else if (GetTickCount64() - t0 < 50) {
                break;      // 句柄已失效，退出内层重新打开
            }
        }
        CloseHandle(h);
        if (WaitForSingleObject(g_quit_event, 0) != WAIT_OBJECT_0) {
            g_rapoo_path.clear();
            WaitForSingleObject(g_quit_event, 1000);
        }
    }
    return 0;
}

static DWORD WINAPI refresh_thread(LPVOID) {
    HANDLE evs[2] = {g_quit_event, g_refresh_event};
    while (true) {
        DWORD w = WaitForMultipleObjects(2, evs, FALSE, INFINITE);
        if (w == WAIT_OBJECT_0) break;
        if (w != WAIT_OBJECT_0 + 1) continue;

        // 先更新惠普/ROG（通常 100ms 内），雷柏放最后，避免它休眠时拖慢另外两台
        DeviceState h = read_hp();
        DeviceState a = read_asus();
        {
            std::lock_guard<std::mutex> lk(g_state_mu);
            g_states[1] = h;
            g_states[2] = a;
        }
        if (!PostMessageW(g_hwnd, WM_REFRESH_DONE, 0, 0)) break;

        DeviceState r = read_rapoo(6500);
        {
            std::lock_guard<std::mutex> lk(g_state_mu);
            g_states[0] = r;
        }
        if (!PostMessageW(g_hwnd, WM_REFRESH_DONE, 0, 0)) break;
    }
    return 0;
}

// --------------------------------------------------------------------------
// 面板 / 配置
// --------------------------------------------------------------------------
static std::wstring find_pythonw() {
    if (!g_cfg.pythonw.empty() && GetFileAttributesW(g_cfg.pythonw.c_str())
                                      != INVALID_FILE_ATTRIBUTES)
        return g_cfg.pythonw;
    wchar_t buf[MAX_PATH];
    if (SearchPathW(nullptr, L"pythonw.exe", nullptr, MAX_PATH, buf, nullptr))
        return buf;
    if (SearchPathW(nullptr, L"python.exe", nullptr, MAX_PATH, buf, nullptr))
        return buf;
    return L"";
}

static void open_panel() {
    HWND ui = FindWindowW(nullptr, kUiTitle);
    if (ui) {
        ShowWindow(ui, SW_RESTORE);
        SetForegroundWindow(ui);
        return;
    }
    std::wstring py = find_pythonw();
    if (py.empty()) {
        MessageBoxW(g_hwnd, L"找不到 pythonw.exe，无法打开面板。\n"
                            L"请检查 tray_config.json 里的 pythonw 路径。",
                    L"设备电量", MB_ICONWARNING);
        return;
    }
    std::wstring script = g_dir + L"\\my_devices.py";
    std::wstring cmd = L"\"" + py + L"\" \"" + script + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW, nullptr, g_dir.c_str(), &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

static void reload_config() {
    int old_interval = g_cfg.interval;
    int old_ring[3] = {g_cfg.ring[0], g_cfg.ring[1], g_cfg.ring[2]};
    load_config(g_cfg);
    if (g_cfg.interval != old_interval && g_hwnd)
        SetTimer(g_hwnd, TIMER_ID, (UINT)g_cfg.interval * 1000, nullptr);
    if (memcmp(old_ring, g_cfg.ring, sizeof(old_ring)) != 0)
        update_icons();
    debug_log(L"reload: interval=%d ring=%d,%d,%d", g_cfg.interval,
              g_cfg.ring[0], g_cfg.ring[1], g_cfg.ring[2]);
}

static void request_refresh() { SetEvent(g_refresh_event); }

// --------------------------------------------------------------------------
// 窗口
// --------------------------------------------------------------------------
static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_TRAY) {
        UINT ev = LOWORD(lp);
        if (ev == WM_RBUTTONUP) {
            open_panel();
        } else if (ev == WM_LBUTTONUP || ev == WM_LBUTTONDBLCLK) {
            request_refresh();
        }
        return 0;
    }
    if (msg == WM_REFRESH_DONE || msg == WM_RAPOO_UPDATE) {
        update_icons();
        return 0;
    }
    if (msg == WM_TIMER && wp == TIMER_ID) {
        request_refresh();
        return 0;
    }
    if (msg == g_msg_quit) {
        remove_icons();
        remove_pid();
        DestroyWindow(hwnd);
        return 0;
    }
    if (msg == g_msg_reload) {
        reload_config();
        return 0;
    }
    if (msg == g_taskbar_created) {
        g_item.added = false;
        update_icons();
        return 0;
    }
    if (msg == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static bool create_window() {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = g_inst;
    wc.lpszClassName = kClassName;
    if (!RegisterClassExW(&wc)) {
        DWORD err = GetLastError();
        if (err != ERROR_CLASS_ALREADY_EXISTS) {
            debug_log(L"RegisterClassExW failed: %lu", err);
            return false;
        }
    }
    g_hwnd = CreateWindowExW(0, kClassName, kAppTitle, 0, 0, 0, 0, 0,
                             nullptr, nullptr, g_inst, nullptr);
    if (!g_hwnd) {
        debug_log(L"CreateWindowExW failed: %lu", GetLastError());
        return false;
    }
    return true;
}

// --------------------------------------------------------------------------
// 测试模式
// --------------------------------------------------------------------------
static int run_once() {
    load_config(g_cfg);
    g_quit_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_listener_thread = CreateThread(nullptr, 0, listener_thread, nullptr, 0,
                                     nullptr);
    DeviceState r = read_rapoo(6500);
    DeviceState h = read_hp();
    DeviceState a = read_asus();
    auto line = [](const wchar_t* name, const DeviceState& s) {
        if (s.level >= 0) {
            std::wstring t = std::wstring(name) + L"：" +
                             std::to_wstring(s.level) + L"%";
            if (!s.status.empty()) t += L"  " + s.status;
            print_line(t);
        } else {
            print_line(std::wstring(name) + L"：读取失败 —— " + s.detail);
        }
    };
    line(L"雷柏 VT7", r);
    line(L"惠普 GT87", h);
    line(L"ROG 破风", a);
    SetEvent(g_quit_event);
    if (g_listener_thread) {
        WaitForSingleObject(g_listener_thread, 2000);
        CloseHandle(g_listener_thread);
    }
    CloseHandle(g_quit_event);
    return 0;
}

static int post_message_to_tray(const wchar_t* name) {
    HWND h = FindWindowW(kClassName, nullptr);
    if (!h) return 1;
    UINT m = RegisterWindowMessageW(name);
    PostMessageW(h, m, 0, 0);
    return 0;
}

// --------------------------------------------------------------------------
// 入口
// --------------------------------------------------------------------------
int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmdline, int) {
    g_inst = inst;
    g_dir = module_dir();

    for (const wchar_t* p = cmdline; p && *p; ++p) {
        if (wcsstr(p, L"--debug")) { g_debug = true; break; }
    }
    if (g_debug) {
        HANDLE e = GetStdHandle(STD_ERROR_HANDLE);
        if (!e || e == INVALID_HANDLE_VALUE
            || GetFileType(e) == FILE_TYPE_UNKNOWN)
            AllocConsole();
    }

    bool quit = wcsstr(cmdline, L"--quit") != nullptr;
    bool once = wcsstr(cmdline, L"--once") != nullptr;
    bool reload = wcsstr(cmdline, L"--reload") != nullptr;

    if (quit)
        return post_message_to_tray(kQuitMsgName);
    if (reload)
        return post_message_to_tray(kReloadMsgName);
    if (once)
        return run_once();

    // 单实例
    HANDLE mutex = CreateMutexW(nullptr, FALSE, kMutexName);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND h = FindWindowW(kClassName, nullptr);
        if (h) {
            UINT m = RegisterWindowMessageW(kReloadMsgName);
            PostMessageW(h, m, 0, 0);
            debug_log(L"托盘已在运行，通知它重读配置");
            return 0;
        }
        cleanup_native_zombie();
    }

    SetProcessDPIAware();
    load_config(g_cfg);
    write_pid();

    g_taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
    g_msg_reload = RegisterWindowMessageW(kReloadMsgName);
    g_msg_quit = RegisterWindowMessageW(kQuitMsgName);
    g_refresh_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_quit_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!create_window()) return 1;

    {
        std::lock_guard<std::mutex> lk(g_state_mu);
        for (int i = 0; i < 3; ++i)
            g_states[i].detail = L"正在读取…";
    }
    update_icons();

    g_listener_thread = CreateThread(nullptr, 0, listener_thread, nullptr, 0,
                                     nullptr);
    g_refresh_thread = CreateThread(nullptr, 0, refresh_thread, nullptr, 0,
                                    nullptr);
    SetTimer(g_hwnd, TIMER_ID, (UINT)g_cfg.interval * 1000, nullptr);
    request_refresh();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // 退出清理
    SetEvent(g_quit_event);
    if (g_refresh_thread) {
        WaitForSingleObject(g_refresh_thread, 1000);
        CloseHandle(g_refresh_thread);
    }
    if (g_listener_thread) {
        WaitForSingleObject(g_listener_thread, 1000);
        CloseHandle(g_listener_thread);
    }
    if (g_refresh_event) CloseHandle(g_refresh_event);
    if (g_quit_event) CloseHandle(g_quit_event);
    remove_pid();
    remove_icons();
    if (mutex) CloseHandle(mutex);
    return 0;
}
