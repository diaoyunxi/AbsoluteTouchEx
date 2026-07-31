// attest.cpp — AbsoluteTouchEx 验证 / 自诊断工具（现代化重构版 v2.0.0）
//
// 用途：
//   1. 被动观察者：注册为鼠标 raw input 的接收者，将收到的绝对坐标鼠标事件
//      （MOUSE_MOVE_ABSOLUTE，通常由 atdll 通过 SendInput 投出）的坐标打印为
//      [ABS] 行，用于验证 atdll 的绝对鼠标输入是否真正送达系统。
//   2. 自诊断（本版新增）：启动即枚举系统全部原始输入设备，明确告诉你这台
//      机器是否存在受支持的"精密触控板 (Digitizer: Touch Pad)"；并直接订阅
//      触控板 HID 报告，把触点报告实时打印为 [TOUCH] 行。这样"attest 无任何
//      输出"时，能立刻区分到底是：
//        a) 设备本身不是精密触控板（枚举结论会写明）；
//        b) 设备有报告但 atdll 没注入 / 没解析（有 [TOUCH] 但无 [ABS]）；
//        c) 忘了按 SHIFT+F6 开启绝对模式（仅有 [TOUCH]，无 [ABS]）。
//
// 相比旧版的改进：
//   1. 窗口改用 HWND_MESSAGE（message-only），真正隐藏，不再误用
//      WS_OVERLAPPEDWINDOW 弹出可见窗口；
//   2. win32_error 改为公有继承 std::exception 并重写 what()；
//   3. 补全 ATOM / HWND 判空与全局 try/catch，错误可见；
//   4. 新增设备枚举与触控板 HID 订阅，便于定位"无输出"根因。

// 本程序不依赖 atdll，也不链接 Detours，仅使用系统 Raw Input API（user32）。

#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <string>
#include <vector>
#include <Windows.h>
#include <hidusage.h>

// 异常：包装 Win32 GetLastError()
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

// 把 usage page / usage 翻译成可读名称，并标记是否为受支持的精密触控板
static void AT_DescribeHidUsage(USHORT page, USHORT usage, std::string &out, bool &isTouchPad)
{
    isTouchPad = false;
    char buf[64];
    if (page == HID_USAGE_PAGE_GENERIC) {
        switch (usage) {
        case HID_USAGE_GENERIC_MOUSE:    out = "鼠标(Mouse)"; break;
        case HID_USAGE_GENERIC_KEYBOARD: out = "键盘(Keyboard)"; break;
        case HID_USAGE_GENERIC_X:        out = "X"; break;
        case HID_USAGE_GENERIC_Y:        out = "Y"; break;
        default:
            sprintf_s(buf, "usage=0x%04X", usage);
            out = buf;
        }
    } else if (page == HID_USAGE_PAGE_DIGITIZER) {
        switch (usage) {
        case HID_USAGE_DIGITIZER_TOUCH_PAD:
            out = "精密触控板(Precision Touchpad)";
            isTouchPad = true;
            break;
        case HID_USAGE_DIGITIZER_TOUCH_SCREEN: out = "触摸屏(Touch Screen)"; break;
        case HID_USAGE_DIGITIZER_PEN:          out = "手写笔(Pen)"; break;
        default:
            sprintf_s(buf, "usage=0x%04X", usage);
            out = buf;
        }
    } else {
        sprintf_s(buf, "page=0x%04X usage=0x%04X", page, usage);
        out = buf;
    }
}

// 枚举系统原始输入设备，输出诊断结论
static void AT_EnumerateRawInputDevices()
{
    std::cout << "----- 设备诊断 -----" << std::endl;

    UINT num = 0;
    if (GetRawInputDeviceList(nullptr, &num, sizeof(RAWINPUTDEVICELIST)) == (UINT)-1) {
        std::cerr << "[诊断] 枚举设备失败: 0x" << std::hex << GetLastError() << std::endl;
        return;
    }
    if (num == 0) {
        std::cout << "[诊断] 系统没有注册任何原始输入设备。" << std::endl;
        return;
    }

    std::vector<RAWINPUTDEVICELIST> devs(num);
    if (GetRawInputDeviceList(devs.data(), &num, sizeof(RAWINPUTDEVICELIST)) == (UINT)-1) {
        std::cerr << "[诊断] 枚举设备失败: 0x" << std::hex << GetLastError() << std::endl;
        return;
    }

    bool foundTouchPad = false;
    std::cout << "[诊断] 共检测到 " << num << " 个原始输入设备：" << std::endl;

    for (const RAWINPUTDEVICELIST &d : devs) {
        if (d.hDevice == nullptr) continue;

        RID_DEVICE_INFO info{};
        info.cbSize = sizeof(RID_DEVICE_INFO);
        UINT sz = sizeof(RID_DEVICE_INFO);
        if (GetRawInputDeviceInfoW(d.hDevice, RIDI_DEVICEINFO, &info, &sz) == (UINT)-1)
            continue;

        if (info.dwType == RIM_TYPEHID) {
            std::string name;
            bool isTouchPad = false;
            AT_DescribeHidUsage(info.hid.usUsagePage, info.hid.usUsage, name, isTouchPad);
            if (isTouchPad) foundTouchPad = true;
            std::cout << "  - HID      " << name
                      << "  (page=0x" << std::hex << info.hid.usUsagePage
                      << ", usage=0x" << info.hid.usUsage << ")" << std::endl;
        } else if (info.dwType == RIM_TYPEMOUSE) {
            std::cout << "  - 鼠标     id=" << std::dec << info.mouse.dwId << std::endl;
        } else if (info.dwType == RIM_TYPEKEYBOARD) {
            std::cout << "  - 键盘     type=" << info.keyboard.dwType << std::endl;
        } else {
            std::cout << "  - 未知类型 " << info.dwType << std::endl;
        }
    }

    std::cout << "----- 诊断结论 -----" << std::endl;
    if (foundTouchPad) {
        std::cout << "[OK] 已检测到受支持的精密触控板，设备本身没问题。" << std::endl;
        std::cout << "     请移动手指：若出现 [TOUCH] 行说明触控板报告正常送达；" << std::endl;
        std::cout << "     若只有 [TOUCH] 而无 [ABS]，请先按 SHIFT+F6 开启绝对模式，" << std::endl;
        std::cout << "     仍无 [ABS] 则查看 DebugView 中 atdll 的日志（注入/解析失败）。" << std::endl;
    } else {
        std::cout << "[FAIL] 未检测到精密触控板 (Digitizer: Touch Pad)。" << std::endl;
        std::cout << "       AbsoluteTouchEx 需要 Windows 精密触控板；普通鼠标或老式" << std::endl;
        std::cout << "       (Synaptics 等) 触控板不被支持——这就是 attest 无任何输出的根本原因。" << std::endl;
        std::cout << "       请在 设置 -> 设备 -> 触控板 顶部确认是否显示" << std::endl;
        std::cout << "       \"你的电脑具有精密触控板\"。" << std::endl;
    }
    std::cout << "--------------------" << std::endl;
}

// 处理鼠标 raw input：仅打印绝对坐标（来自 atdll 的 SendInput）
static void AT_HandleMouseRawInput(HRAWINPUT hInput, RAWINPUTHEADER hdr)
{
    auto input = AT_GetRawInput(hInput, hdr);
    if (input->data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) {
        std::cout << "[ABS] " << input->data.mouse.lLastX << ", "
                  << input->data.mouse.lLastY << std::endl;
    }
}

// 处理触控板 HID raw input：打印报告概况，用于证明设备是否在吐数据
// 限流：触控板报告频率极高（约 60+/秒），不节流会淹没关键的 [ABS] 行
static void AT_HandleTouchpadRawInput(HRAWINPUT hInput, RAWINPUTHEADER hdr)
{
    static ULONGLONG s_lastPrint = 0;
    ULONGLONG now = GetTickCount64();
    if (now - s_lastPrint < 200)
        return;
    s_lastPrint = now;

    auto input = AT_GetRawInput(hInput, hdr);
    std::cout << "[TOUCH] device=" << hdr.hDevice
              << " sizeHid=" << input->data.hid.dwSizeHid
              << " count=" << input->data.hid.dwCount << std::endl;
}

static LRESULT CALLBACK AT_WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_DESTROY:
        PostQuitMessage(0);
        break;
    case WM_INPUT: {
        try {
            HRAWINPUT hInput = reinterpret_cast<HRAWINPUT>(lParam);
            RAWINPUTHEADER hdr = AT_GetRawInputHeader(hInput);
            if (hdr.dwType == RIM_TYPEMOUSE) {
                AT_HandleMouseRawInput(hInput, hdr);
            } else if (hdr.dwType == RIM_TYPEHID) {
                AT_HandleTouchpadRawInput(hInput, hdr);
            }
        } catch (const std::exception &e) {
            std::cerr << "[错误] " << e.what() << std::endl;
        }
        break;
    }
    default:
        return DefWindowProcW(hWnd, message, wParam, lParam);
    }
    return 0;
}

static ATOM AT_RegisterClass(HINSTANCE hInstance, LPCWSTR className)
{
    WNDCLASSEXW wcex{};
    wcex.cbSize = sizeof(WNDCLASSEXW);
    wcex.lpfnWndProc = AT_WndProc;
    wcex.hInstance = hInstance;
    wcex.lpszClassName = className;
    ATOM atom = RegisterClassExW(&wcex);
    if (atom == 0) throw win32_error();
    return atom;
}

// 创建 message-only 窗口（HWND_MESSAGE）：真正隐藏，不出现在桌面/任务栏
static HWND AT_CreateWindow(HINSTANCE hInstance, LPCWSTR className, LPCWSTR title)
{
    HWND hWnd = CreateWindowExW(
        0, className, title, 0,
        0, 0, 0, 0,
        HWND_MESSAGE, nullptr, hInstance, nullptr);
    if (hWnd == nullptr) throw win32_error();
    return hWnd;
}

// 注册一个 raw input 设备为输入接收者（RIDEV_INPUTSINK：即使窗口非前台也接收）
static void AT_RegisterRawInput(HWND hWnd, USHORT usagePage, USHORT usage, const char *label)
{
    RAWINPUTDEVICE dev;
    dev.usUsagePage = usagePage;
    dev.usUsage = usage;
    dev.dwFlags = RIDEV_INPUTSINK;
    dev.hwndTarget = hWnd;
    if (!RegisterRawInputDevices(&dev, 1, sizeof(RAWINPUTDEVICE))) {
        std::cerr << "[警告] 注册 " << label << " 接收失败: 0x"
                  << std::hex << GetLastError() << std::endl;
    }
}

int main()
{
    try {
        HINSTANCE hInstance = GetModuleHandleW(nullptr);
        AT_RegisterClass(hInstance, L"ATWndCls");
        HWND hWnd = AT_CreateWindow(hInstance, L"ATWndCls", L"AbsoluteTouch Test");

        // 鼠标：用于观察 atdll 投出的绝对坐标（[ABS]）
        AT_RegisterRawInput(hWnd, HID_USAGE_PAGE_GENERIC, HID_USAGE_GENERIC_MOUSE, "鼠标");
        // 触控板 HID：用于直接观察设备是否在吐报告（[TOUCH]）
        AT_RegisterRawInput(hWnd, HID_USAGE_PAGE_DIGITIZER, HID_USAGE_DIGITIZER_TOUCH_PAD, "触控板");

        // 启动即做设备诊断，明确本机是否具备受支持的精密触控板
        AT_EnumerateRawInputDevices();

        std::cout << "AbsoluteTouchEx attest: listening for absolute mouse input..." << std::endl;
        std::cout << "提示：移动手指应出现 [TOUCH]；按 SHIFT+F6 开启绝对模式后" << std::endl;
        std::cout << "      由 atdll 注入的绝对坐标会以 [ABS] 显示。" << std::endl;

        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        return static_cast<int>(msg.wParam);
    } catch (const win32_error &e) {
        std::cerr << "Error 0x" << std::hex << e.code() << std::endl;
        return 1;
    } catch (const std::exception &e) {
        std::cerr << "Exception: " << e.what() << std::endl;
        return 1;
    }
}
