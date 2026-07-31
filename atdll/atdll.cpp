// atdll.cpp — AbsoluteTouchEx 核心注入模块（现代化重构版 v2.0.0）
//
// 设计总览（相比旧版的改进）：
//   旧版为了把"合成的绝对坐标鼠标"送回目标程序，采用 MAGIC_HANDLE(0) 桥接 +
//   替换目标窗口 WndProc，为此 Hook 了 5 个 Win32 函数，强耦合目标程序的窗口
//   创建方式，且存在竞态与加载器锁风险。
//
//   新版采用 Windows 官方推荐的输入注入方式：
//     1. DLL 注入后，在独立工作线程中创建一个 message-only 窗口（HWND_MESSAGE）；
//     2. 将该窗口注册为触控板（Precision Touchpad）raw input 的接收者；
//     3. 工作线程的消息循环处理触控板事件：解析 HID 多点触控报告 ->
//        线性映射到 0..65535 归一化坐标 -> SendInput(MOUSEEVENTF_ABSOLUTE)
//        投递绝对坐标鼠标移动；
//     4. 热键（SHIFT+F6..F9）也注册到自有窗口，完全不触碰目标程序的 WndProc。
//
//   由此移除了 RegisterRawInputDevices / GetRawInputData / CreateWindowExW /
//   Get/SetWindowLongPtr 的全部 Hook，对目标程序零侵入，行为更标准、更稳健，
//   并且天然修复了"启用后吞掉真实鼠标"的副作用。
//
//   仅保留 Detours 的注入引导能力（DetourRestoreAfterWith / DetourIsHelperProcess
//   以及导出 DetourFinishHelperProcess），用于配合 atloader 的进程创建期注入。

#include <cstdarg>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>
#include <Windows.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <hidusage.h>
#include "detours.h"
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;

// 版本号
#define VERSION_STRING "2.0.0"

// HID 规范中未预定义的使用量
#define HID_USAGE_DIGITIZER_CONTACT_ID   0x51
#define HID_USAGE_DIGITIZER_CONTACT_COUNT 0x54

// 热键：绝对模式开关 / 校准模式 / 加载校准 / 保存校准
#define HOTKEY_ENABLE_ID       0xCAFE
#define HOTKEY_ENABLE_MOD      (MOD_SHIFT)
#define HOTKEY_ENABLE_VK       VK_F6

#define HOTKEY_CALIBRATION_ID  0xCAFF
#define HOTKEY_CALIBRATION_MOD (MOD_SHIFT)
#define HOTKEY_CALIBRATION_VK  VK_F7

#define HOTKEY_LOAD_ID         0xCAFD
#define HOTKEY_LOAD_MOD        (MOD_SHIFT)
#define HOTKEY_LOAD_VK         VK_F8

#define HOTKEY_SAVE_ID         0xCAFC
#define HOTKEY_SAVE_MOD        (MOD_SHIFT)
#define HOTKEY_SAVE_VK         VK_F9

// 校准配置文件（JSON）
#define CALIBRATION_FILE "atcalibration.json"

// 窗口类名
#define AT_WINDOW_CLASS L"AbsoluteTouchExClass"

// ----------------------------------------------------------------------------
// 日志：统一输出到 OutputDebugString（不写盘，Release 也可安全启用），
// 方便用 DebugView / 调试器查看，避免旧版 AllocConsole 在 DllMain 中的风险。
// ----------------------------------------------------------------------------
static void at_log(const wchar_t *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    wchar_t buf[1024];
    _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, args);
    va_end(args);
    OutputDebugStringW(buf);
    OutputDebugStringW(L"\n");
}

// ----------------------------------------------------------------------------
// 异常类型：修正旧版私有继承 std::exception 导致 catch(std::exception&) 接不住的
// 缺陷，并补全 what() 以输出可读信息。
// ----------------------------------------------------------------------------
class win32_error : public std::exception
{
public:
    explicit win32_error(DWORD code = GetLastError())
        : m_code(code), m_what("win32_error: 0x" + to_hex(code)) {}
    const char *what() const noexcept override { return m_what.c_str(); }
    DWORD code() const noexcept { return m_code; }
private:
    static std::string to_hex(DWORD code)
    {
        char tmp[16];
        sprintf_s(tmp, "0x%08X", code);
        return std::string(tmp);
    }
    DWORD m_code;
    std::string m_what;
};

class hid_error : public std::exception
{
public:
    explicit hid_error(NTSTATUS status)
        : m_code(status), m_what("hid_error: 0x" + to_hex(status)) {}
    const char *what() const noexcept override { return m_what.c_str(); }
    NTSTATUS code() const noexcept { return m_code; }
private:
    static std::string to_hex(NTSTATUS status)
    {
        char tmp[16];
        sprintf_s(tmp, "0x%08X", status);
        return std::string(tmp);
    }
    NTSTATUS m_code;
    std::string m_what;
};

// 用 unique_ptr 语义管理 malloc 分配的可变长结构
struct free_deleter { void operator()(void *ptr) const noexcept { free(ptr); } };
template <typename T> using malloc_ptr = std::unique_ptr<T, free_deleter>;

template <typename T>
static malloc_ptr<T> make_malloc(size_t size)
{
    T *ptr = static_cast<T *>(malloc(size));
    if (ptr == nullptr) throw std::bad_alloc();
    return malloc_ptr<T>(ptr);
}

// 单个触控触点的解析信息（来自 HID 报告描述符）
struct at_contact_info
{
    USHORT link;
    RECT touchArea;
};

// 一次触摸事件的数据
struct at_contact
{
    at_contact_info info;
    ULONG id;
    POINT point;
};

// 设备信息：触点区域、HID 偏移等，可跨事件复用，仅解析一次
struct at_device_info
{
    malloc_ptr<_HIDP_PREPARSED_DATA> preparsedData;
    USHORT linkContactCount;
    std::vector<at_contact_info> contactInfo;
};

// ----------------------------------------------------------------------------
// 全局状态
// ----------------------------------------------------------------------------
static bool g_enabled = false;             // 绝对输入模式是否开启
static bool g_inCalibrationMode = false;  // 是否处于校准模式
static std::optional<RECT> g_calibration; // 全局校准覆盖区域（若有）
static std::unordered_map<HANDLE, RECT> g_calibrationArea; // 校准时累积的包围盒
static std::unordered_map<HANDLE, at_device_info> g_devices; // 每设备 HID 信息缓存
static std::mutex g_devicesMutex;         // 保护 g_devices（配置加载可能跨线程）
static HWND g_hWnd = nullptr;             // 我们的 message-only 窗口
static HANDLE g_hThread = nullptr;        // 工作线程句柄

// ----------------------------------------------------------------------------
// Raw input / HID 辅助函数（这部分逻辑经实践验证正确，予以保留）
// ----------------------------------------------------------------------------
static RAWINPUTHEADER AT_GetRawInputHeader(HRAWINPUT hInput)
{
    RAWINPUTHEADER hdr;
    UINT size = sizeof(hdr);
    if (GetRawInputData(hInput, RID_HEADER, &hdr, &size, sizeof(RAWINPUTHEADER)) == (UINT)-1)
        throw win32_error();
    return hdr;
}

static malloc_ptr<RAWINPUT> AT_GetRawInput(HRAWINPUT hInput, RAWINPUTHEADER hdr)
{
    auto input = make_malloc<RAWINPUT>(hdr.dwSize);
    UINT size = hdr.dwSize;
    if (GetRawInputData(hInput, RID_INPUT, input.get(), &size, sizeof(RAWINPUTHEADER)) == (UINT)-1)
        throw win32_error();
    return input;
}

static std::vector<RAWINPUTDEVICELIST> AT_GetRawInputDeviceList()
{
    std::vector<RAWINPUTDEVICELIST> devices(64);
    while (true) {
        UINT numDevices = (UINT)devices.size();
        UINT ret = GetRawInputDeviceList(devices.data(), &numDevices, sizeof(RAWINPUTDEVICELIST));
        if (ret != (UINT)-1) {
            devices.resize(ret);
            return devices;
        } else if (GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
            devices.resize(numDevices);
        } else {
            throw win32_error();
        }
    }
}

static malloc_ptr<_HIDP_PREPARSED_DATA> AT_GetHidPreparsedData(HANDLE hDevice)
{
    UINT size = 0;
    if (GetRawInputDeviceInfoW(hDevice, RIDI_PREPARSEDDATA, nullptr, &size) == (UINT)-1)
        throw win32_error();
    auto preparsedData = make_malloc<_HIDP_PREPARSED_DATA>(size);
    if (GetRawInputDeviceInfoW(hDevice, RIDI_PREPARSEDDATA, preparsedData.get(), &size) == (UINT)-1)
        throw win32_error();
    return preparsedData;
}

static std::vector<HIDP_BUTTON_CAPS> AT_GetHidInputButtonCaps(PHIDP_PREPARSED_DATA preparsedData)
{
    NTSTATUS status;
    HIDP_CAPS caps;
    status = HidP_GetCaps(preparsedData, &caps);
    if (status != HIDP_STATUS_SUCCESS) throw hid_error(status);
    USHORT numCaps = caps.NumberInputButtonCaps;
    std::vector<HIDP_BUTTON_CAPS> buttonCaps(numCaps);
    status = HidP_GetButtonCaps(HidP_Input, buttonCaps.data(), &numCaps, preparsedData);
    if (status != HIDP_STATUS_SUCCESS) throw hid_error(status);
    buttonCaps.resize(numCaps);
    return buttonCaps;
}

static std::vector<HIDP_VALUE_CAPS> AT_GetHidInputValueCaps(PHIDP_PREPARSED_DATA preparsedData)
{
    NTSTATUS status;
    HIDP_CAPS caps;
    status = HidP_GetCaps(preparsedData, &caps);
    if (status != HIDP_STATUS_SUCCESS) throw hid_error(status);
    USHORT numCaps = caps.NumberInputValueCaps;
    std::vector<HIDP_VALUE_CAPS> valueCaps(numCaps);
    status = HidP_GetValueCaps(HidP_Input, valueCaps.data(), &numCaps, preparsedData);
    if (status != HIDP_STATUS_SUCCESS) throw hid_error(status);
    valueCaps.resize(numCaps);
    return valueCaps;
}

static bool AT_GetHidUsageButton(HIDP_REPORT_TYPE reportType, USAGE usagePage,
                                 USHORT linkCollection, USAGE usage,
                                 PHIDP_PREPARSED_DATA preparsedData,
                                 PBYTE report, ULONG reportLen)
{
    ULONG numUsages = HidP_MaxUsageListLength(reportType, usagePage, preparsedData);
    std::vector<USAGE> usages(numUsages);
    NTSTATUS status = HidP_GetUsages(reportType, usagePage, linkCollection,
                                     usages.data(), &numUsages, preparsedData,
                                     reinterpret_cast<PCHAR>(report), reportLen);
    if (status != HIDP_STATUS_SUCCESS) throw hid_error(status);
    usages.resize(numUsages);
    return std::find(usages.begin(), usages.end(), usage) != usages.end();
}

static ULONG AT_GetHidUsageLogicalValue(HIDP_REPORT_TYPE reportType, USAGE usagePage,
                                        USHORT linkCollection, USAGE usage,
                                        PHIDP_PREPARSED_DATA preparsedData,
                                        PBYTE report, ULONG reportLen)
{
    ULONG value;
    NTSTATUS status = HidP_GetUsageValue(reportType, usagePage, linkCollection, usage,
                                         &value, preparsedData,
                                         reinterpret_cast<PCHAR>(report), reportLen);
    if (status != HIDP_STATUS_SUCCESS) throw hid_error(status);
    return value;
}

static LONG AT_GetHidUsagePhysicalValue(HIDP_REPORT_TYPE reportType, USAGE usagePage,
                                        USHORT linkCollection, USAGE usage,
                                        PHIDP_PREPARSED_DATA preparsedData,
                                        PBYTE report, ULONG reportLen)
{
    LONG value;
    NTSTATUS status = HidP_GetScaledUsageValue(reportType, usagePage, linkCollection, usage,
                                               &value, preparsedData,
                                               reinterpret_cast<PCHAR>(report), reportLen);
    if (status != HIDP_STATUS_SUCCESS) throw hid_error(status);
    return value;
}

// 将触控板事件注册到指定窗口（RIDEV_INPUTSINK：即使窗口非前台也能接收）
static void AT_RegisterTouchpadInput(HWND hWnd)
{
    RAWINPUTDEVICE dev;
    dev.usUsagePage = HID_USAGE_PAGE_DIGITIZER;
    dev.usUsage = HID_USAGE_DIGITIZER_TOUCH_PAD;
    dev.dwFlags = RIDEV_INPUTSINK;
    dev.hwndTarget = hWnd;
    if (!RegisterRawInputDevices(&dev, 1, sizeof(RAWINPUTDEVICE)))
        throw win32_error();
}

// 将四个功能热键注册到自有窗口
static void AT_RegisterHotKeys(HWND hWnd)
{
    RegisterHotKey(hWnd, HOTKEY_ENABLE_ID, HOTKEY_ENABLE_MOD, HOTKEY_ENABLE_VK);
    RegisterHotKey(hWnd, HOTKEY_CALIBRATION_ID, HOTKEY_CALIBRATION_MOD, HOTKEY_CALIBRATION_VK);
    RegisterHotKey(hWnd, HOTKEY_LOAD_ID, HOTKEY_LOAD_MOD, HOTKEY_LOAD_VK);
    RegisterHotKey(hWnd, HOTKEY_SAVE_ID, HOTKEY_SAVE_MOD, HOTKEY_SAVE_VK);
}

// 触控板物理坐标 -> 屏幕归一化坐标（0..65535，与 MOUSE_MOVE_ABSOLUTE 一致）
static POINT AT_TouchpadToScreen(RECT touchpadRect, POINT touchpadPoint)
{
    LONG tpX = max(touchpadRect.left, min(touchpadRect.right, touchpadPoint.x));
    LONG tpY = max(touchpadRect.top, min(touchpadRect.bottom, touchpadPoint.y));

    LONG tpDeltaX = tpX - touchpadRect.left;
    LONG tpDeltaY = tpY - touchpadRect.top;

    // HID 规范中最大值是包含的，因此需要 +1
    LONG tpWidth = touchpadRect.right + 1 - touchpadRect.left;
    LONG tpHeight = touchpadRect.bottom + 1 - touchpadRect.top;

    LONG scDeltaX = (tpDeltaX << 16) / tpWidth;
    LONG scDeltaY = (tpDeltaY << 16) / tpHeight;

    return POINT{ scDeltaX, scDeltaY };
}

// 获取（并缓存）设备的 HID 解析信息
static at_device_info &AT_GetDeviceInfo(HANDLE hDevice)
{
    {
        std::lock_guard<std::mutex> lock(g_devicesMutex);
        auto it = g_devices.find(hDevice);
        if (it != g_devices.end()) return it->second;
    }

    at_device_info dev;
    std::optional<USHORT> linkContactCount;
    dev.preparsedData = AT_GetHidPreparsedData(hDevice);

    struct at_contact_info_tmp
    {
        bool hasContactID = false;
        bool hasTip = false;
        bool hasX = false;
        bool hasY = false;
        RECT touchArea{};
    };
    std::unordered_map<USHORT, at_contact_info_tmp> contacts;

    for (const HIDP_VALUE_CAPS &cap : AT_GetHidInputValueCaps(dev.preparsedData.get())) {
        if (cap.IsRange || !cap.IsAbsolute) continue;
        if (cap.UsagePage == HID_USAGE_PAGE_GENERIC) {
            if (cap.NotRange.Usage == HID_USAGE_GENERIC_X) {
                contacts[cap.LinkCollection].touchArea.left = cap.PhysicalMin;
                contacts[cap.LinkCollection].touchArea.right = cap.PhysicalMax;
                contacts[cap.LinkCollection].hasX = true;
            } else if (cap.NotRange.Usage == HID_USAGE_GENERIC_Y) {
                contacts[cap.LinkCollection].touchArea.top = cap.PhysicalMin;
                contacts[cap.LinkCollection].touchArea.bottom = cap.PhysicalMax;
                contacts[cap.LinkCollection].hasY = true;
            }
        } else if (cap.UsagePage == HID_USAGE_PAGE_DIGITIZER) {
            if (cap.NotRange.Usage == HID_USAGE_DIGITIZER_CONTACT_COUNT) {
                linkContactCount = cap.LinkCollection;
            } else if (cap.NotRange.Usage == HID_USAGE_DIGITIZER_CONTACT_ID) {
                contacts[cap.LinkCollection].hasContactID = true;
            }
        }
    }

    for (const HIDP_BUTTON_CAPS &cap : AT_GetHidInputButtonCaps(dev.preparsedData.get())) {
        if (cap.UsagePage == HID_USAGE_PAGE_DIGITIZER &&
            cap.NotRange.Usage == HID_USAGE_DIGITIZER_TIP_SWITCH) {
            contacts[cap.LinkCollection].hasTip = true;
        }
    }

    if (!linkContactCount.has_value())
        throw std::runtime_error("No contact count usage found");
    dev.linkContactCount = linkContactCount.value();

    for (const auto &kvp : contacts) {
        const at_contact_info_tmp &info = kvp.second;
        if (info.hasContactID && info.hasTip && info.hasX && info.hasY) {
            at_log(L"Contact for device %p: link=%d, touchArea={%d,%d,%d,%d}",
                   hDevice, kvp.first, info.touchArea.left, info.touchArea.top,
                   info.touchArea.right, info.touchArea.bottom);
            dev.contactInfo.push_back({ kvp.first, info.touchArea });
        }
    }

    std::lock_guard<std::mutex> lock(g_devicesMutex);
    return g_devices[hDevice] = std::move(dev);
}

// 从一次 raw input 事件解析所有触控触点
static std::vector<at_contact> AT_GetContacts(at_device_info &dev, RAWINPUT *input)
{
    std::vector<at_contact> contacts;
    DWORD sizeHid = input->data.hid.dwSizeHid;
    DWORD count = input->data.hid.dwCount;
    BYTE *rawData = input->data.hid.bRawData;
    if (count == 0) return contacts;

    ULONG numContacts = AT_GetHidUsageLogicalValue(
        HidP_Input, HID_USAGE_PAGE_DIGITIZER, dev.linkContactCount,
        HID_USAGE_DIGITIZER_CONTACT_COUNT, dev.preparsedData.get(), rawData, sizeHid);

    if (numContacts > dev.contactInfo.size()) {
        at_log(L"Device reported more contacts (%u) than links (%zu)", numContacts, dev.contactInfo.size());
        numContacts = static_cast<ULONG>(dev.contactInfo.size());
    }

    for (ULONG i = 0; i < numContacts; ++i) {
        at_contact_info &info = dev.contactInfo[i];
        bool tip = AT_GetHidUsageButton(
            HidP_Input, HID_USAGE_PAGE_DIGITIZER, info.link,
            HID_USAGE_DIGITIZER_TIP_SWITCH, dev.preparsedData.get(), rawData, sizeHid);
        if (!tip) continue;

        ULONG id = AT_GetHidUsageLogicalValue(
            HidP_Input, HID_USAGE_PAGE_DIGITIZER, info.link,
            HID_USAGE_DIGITIZER_CONTACT_ID, dev.preparsedData.get(), rawData, sizeHid);
        LONG x = AT_GetHidUsagePhysicalValue(
            HidP_Input, HID_USAGE_PAGE_GENERIC, info.link,
            HID_USAGE_GENERIC_X, dev.preparsedData.get(), rawData, sizeHid);
        LONG y = AT_GetHidUsagePhysicalValue(
            HidP_Input, HID_USAGE_PAGE_GENERIC, info.link,
            HID_USAGE_GENERIC_Y, dev.preparsedData.get(), rawData, sizeHid);

        contacts.push_back({ info, id, { x, y } });
    }
    return contacts;
}

// 选取主触点（跟踪上一次的主触点 id，使单指拖动稳定）
static ULONG g_primaryContactID = 0;
static at_contact AT_GetPrimaryContact(const std::vector<at_contact> &contacts)
{
    for (const at_contact &contact : contacts)
        if (contact.id == g_primaryContactID) return contact;
    g_primaryContactID = contacts[0].id;
    return contacts[0];
}

// 校准模式：累积触点的包围盒
static void AT_ExtendCalibrationArea(HANDLE hDevice, const std::vector<at_contact> &contacts)
{
    if (g_calibrationArea.find(hDevice) == g_calibrationArea.end()) {
        // 首次出现该设备时初始化为极值，避免与合法坐标 (0,0,0,0) 混淆
        g_calibrationArea[hDevice] = RECT{ LONG_MAX, LONG_MAX, LONG_MIN, LONG_MIN };
    }
    RECT &area = g_calibrationArea[hDevice];
    for (const at_contact &contact : contacts) {
        area.left = min(area.left, contact.point.x);
        area.top = min(area.top, contact.point.y);
        area.right = max(area.right, contact.point.x);
        area.bottom = max(area.bottom, contact.point.y);
    }
}

// 返回实际使用的触控区域：若有全局校准覆盖则用之，否则用设备默认区域
static RECT AT_GetTouchArea(at_contact &contact)
{
    if (g_calibration.has_value()) return g_calibration.value();
    return contact.info.touchArea;
}

// ----------------------------------------------------------------------------
// 配置（JSON）：用 nlohmann/json 读写，异常安全解析，校验数值范围
// ----------------------------------------------------------------------------
static void AT_SaveCalibration()
{
    if (!g_calibration.has_value()) {
        at_log(L"AT_SaveCalibration: no calibration to save");
        return;
    }
    nlohmann::json j;
    j["touch_area"]["left"] = g_calibration->left;
    j["touch_area"]["top"] = g_calibration->top;
    j["touch_area"]["right"] = g_calibration->right;
    j["touch_area"]["bottom"] = g_calibration->bottom;

    std::error_code ec;
    std::ofstream f(CALIBRATION_FILE, std::ios::binary);
    if (!f) { at_log(L"AT_SaveCalibration: cannot open file"); return; }
    f << j.dump(4);
    if (!f) { at_log(L"AT_SaveCalibration: write failed"); return; }
    at_log(L"Calibration saved to %hs", CALIBRATION_FILE);
}

static void AT_LoadCalibration()
{
    std::error_code ec;
    if (!fs::exists(CALIBRATION_FILE, ec)) {
        at_log(L"AT_LoadCalibration: file not found");
        return;
    }
    std::ifstream f(CALIBRATION_FILE, std::ios::binary);
    if (!f) { at_log(L"AT_LoadCalibration: cannot open file"); return; }

    nlohmann::json j;
    try {
        f >> j;
    } catch (const nlohmann::json::exception &e) {
        at_log(L"AT_LoadCalibration: JSON parse error: %hs", e.what());
        return;
    }
    if (!j.contains("touch_area")) {
        at_log(L"AT_LoadCalibration: missing 'touch_area'");
        return;
    }
    const auto &ta = j["touch_area"];
    const char *keys[] = { "left", "top", "right", "bottom" };
    for (const char *k : keys) {
        if (!ta.contains(k) || !ta[k].is_number_integer()) {
            at_log(L"AT_LoadCalibration: bad field '%hs'", k);
            return;
        }
    }
    RECT area;
    area.left = ta["left"].get<LONG>();
    area.top = ta["top"].get<LONG>();
    area.right = ta["right"].get<LONG>();
    area.bottom = ta["bottom"].get<LONG>();
    if (area.right <= area.left || area.bottom <= area.top) {
        at_log(L"AT_LoadCalibration: invalid rectangle");
        return;
    }
    g_calibration = area;
    at_log(L"Calibration loaded: {%d,%d,%d,%d}", area.left, area.top, area.right, area.bottom);
}

// 切换校准模式：退出时把累积包围盒提交为全局校准
static void AT_ToggleCalibrationMode()
{
    if (g_inCalibrationMode) {
        for (const auto &entry : g_calibrationArea) {
            g_calibration = entry.second;
        }
        g_calibrationArea.clear();
        at_log(L"Calibration committed");
    }
    g_inCalibrationMode = !g_inCalibrationMode;
}

// ----------------------------------------------------------------------------
// 输入注入：以绝对坐标方式投递鼠标移动（替代旧版 MAGIC_HANDLE 桥接）
// ----------------------------------------------------------------------------
static void AT_InjectAbsoluteMouse(LONG x, LONG y)
{
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dx = x;          // 0..65535 归一化，与 MOUSE_MOVE_ABSOLUTE 一致
    input.mi.dy = y;
    input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
    if (SendInput(1, &input, sizeof(INPUT)) != 1) {
        at_log(L"SendInput failed: %lu", GetLastError());
    }
}

// 处理一次触控板 WM_INPUT 事件：解析 -> 映射 -> 注入绝对鼠标
static void AT_HandleRawInput(HRAWINPUT hInput)
{
    RAWINPUTHEADER hdr = AT_GetRawInputHeader(hInput);
    if (hdr.dwType != RIM_TYPEHID) return; // 我们只注册了触控板

    at_device_info &dev = AT_GetDeviceInfo(hdr.hDevice);
    auto input = AT_GetRawInput(hInput, hdr);
    std::vector<at_contact> contacts = AT_GetContacts(dev, input.get());
    if (contacts.empty()) return;

    if (g_inCalibrationMode) {
        AT_ExtendCalibrationArea(hdr.hDevice, contacts);
        return;
    }

    at_contact contact = AT_GetPrimaryContact(contacts);
    RECT touchArea = AT_GetTouchArea(contact);
    POINT screenPoint = AT_TouchpadToScreen(touchArea, contact.point);
    at_log(L"Injecting absolute input at (%d,%d)", screenPoint.x, screenPoint.y);
    AT_InjectAbsoluteMouse(screenPoint.x, screenPoint.y);
}

// ----------------------------------------------------------------------------
// 自有窗口过程：处理触控板输入与热键，完全不触碰目标程序窗口
// ----------------------------------------------------------------------------
static LRESULT CALLBACK AT_WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_INPUT:
        if (g_enabled || g_inCalibrationMode) {
            try {
                AT_HandleRawInput(reinterpret_cast<HRAWINPUT>(lParam));
            } catch (const std::exception &e) {
                at_log(L"WndProc WM_INPUT error: %hs", e.what());
            }
        }
        break;
    case WM_HOTKEY:
        if (wParam == HOTKEY_ENABLE_ID) {
            g_enabled = !g_enabled;
            at_log(L"Absolute touch mode -> %s", g_enabled ? L"ON" : L"OFF");
        } else if (wParam == HOTKEY_CALIBRATION_ID) {
            AT_ToggleCalibrationMode();
            at_log(L"Calibration mode -> %s", g_inCalibrationMode ? L"ON" : L"OFF");
        } else if (wParam == HOTKEY_LOAD_ID) {
            AT_LoadCalibration();
        } else if (wParam == HOTKEY_SAVE_ID) {
            AT_SaveCalibration();
        }
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        break;
    default:
        return DefWindowProcW(hWnd, message, wParam, lParam);
    }
    return 0;
}

// 打印检测到的触控板信息（调试用）
static void AT_PrintSystemInfo()
{
    at_log(L"AbsoluteTouchEx v%s", TEXT(VERSION_STRING));
    try {
        for (const RAWINPUTDEVICELIST &dev : AT_GetRawInputDeviceList()) {
            if (dev.hDevice == nullptr) continue;
            UINT size = sizeof(RID_DEVICE_INFO);
            RID_DEVICE_INFO info;
            info.cbSize = sizeof(RID_DEVICE_INFO);
            if (GetRawInputDeviceInfoW(dev.hDevice, RIDI_DEVICEINFO, &info, &size) == (UINT)-1)
                continue;
            if (info.dwType == RIM_TYPEHID &&
                info.hid.usUsagePage == HID_USAGE_PAGE_DIGITIZER &&
                info.hid.usUsage == HID_USAGE_DIGITIZER_TOUCH_PAD) {
                at_device_info &d = AT_GetDeviceInfo(dev.hDevice);
                if (!d.contactInfo.empty())
                    at_log(L"Detected touchpad %p with %zu contacts", dev.hDevice, d.contactInfo.size());
                else
                    at_log(L"Detected touchpad %p but could not parse descriptor", dev.hDevice);
            }
        }
    } catch (const std::exception &e) {
        at_log(L"PrintSystemInfo error: %hs", e.what());
    }
}

// 工作线程：创建 message-only 窗口并运行消息循环
static DWORD WINAPI AT_WorkerThread(LPVOID)
{
    WNDCLASSEXW wcex{};
    wcex.cbSize = sizeof(WNDCLASSEXW);
    wcex.lpfnWndProc = AT_WndProc;
    wcex.hInstance = GetModuleHandleW(nullptr);
    wcex.lpszClassName = AT_WINDOW_CLASS;
    if (RegisterClassExW(&wcex) == 0) {
        at_log(L"RegisterClassExW failed: %lu", GetLastError());
        return 1;
    }

    g_hWnd = CreateWindowExW(
        0, AT_WINDOW_CLASS, nullptr, 0,
        0, 0, 0, 0,
        HWND_MESSAGE, nullptr, wcex.hInstance, nullptr);
    if (g_hWnd == nullptr) {
        at_log(L"CreateWindowExW failed: %lu", GetLastError());
        return 1;
    }

    try {
        AT_RegisterTouchpadInput(g_hWnd);
        AT_RegisterHotKeys(g_hWnd);
    } catch (const std::exception &e) {
        at_log(L"Registration error: %hs", e.what());
    }

    AT_PrintSystemInfo();
    at_log(L"AbsoluteTouchEx worker thread running");

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}

// ----------------------------------------------------------------------------
// DLL 入口：只做最小化的引导与线程启动，不在 DllMain 中执行重活/消息循环
// ----------------------------------------------------------------------------
BOOL APIENTRY DllMain(HINSTANCE hModule, DWORD dwReason, LPVOID lpReserved)
{
    (void)hModule;
    (void)lpReserved;

    if (DetourIsHelperProcess())
        return TRUE;

    switch (dwReason) {
    case DLL_PROCESS_ATTACH:
        DetourRestoreAfterWith();
        g_hThread = CreateThread(nullptr, 0, AT_WorkerThread, nullptr, 0, nullptr);
        if (g_hThread) {
            // 分离线程，不等待（避免 DllMain 死锁）；进程退出时线程随之结束
            CloseHandle(g_hThread);
            g_hThread = nullptr;
        } else {
            at_log(L"CreateThread failed: %lu", GetLastError());
        }
        return TRUE;
    case DLL_PROCESS_DETACH:
        if (g_hWnd) {
            PostMessageW(g_hWnd, WM_DESTROY, 0, 0);
            g_hWnd = nullptr;
        }
        return TRUE;
    default:
        return TRUE;
    }
}
