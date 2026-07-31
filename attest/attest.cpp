// attest.cpp — AbsoluteTouchEx 验证工具（现代化重构版 v2.0.0）
//
// 用途：作为被动观察者，注册为鼠标 raw input 的接收者，将收到的
// 绝对坐标鼠标事件（MOUSE_MOVE_ABSOLUTE）的坐标打印到标准输出，
// 用于验证 atdll 通过 SendInput 投出的绝对鼠标输入是否真正送达系统。
//
// 相比旧版的改进：
//   1. 窗口改用 HWND_MESSAGE（message-only），真正隐藏，不再误用
//      WS_OVERLAPPEDWINDOW 弹出可见窗口；
//   2. win32_error 改为公有继承 std::exception 并重写 what()；
//   3. 补全 ATOM / HWND 判空与全局 try/catch，错误可见。
//
// 本程序不依赖 atdll，也不链接 Detours，仅使用系统 Raw Input API。

#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
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

// 处理鼠标 raw input，打印绝对坐标
static void AT_HandleRawInput(LPARAM lParam)
{
    HRAWINPUT hInput = reinterpret_cast<HRAWINPUT>(lParam);
    RAWINPUTHEADER hdr = AT_GetRawInputHeader(hInput);
    if (hdr.dwType != RIM_TYPEMOUSE)
        return;

    auto input = AT_GetRawInput(hInput, hdr);
    if (input->data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) {
        std::cout << input->data.mouse.lLastX << ", "
                  << input->data.mouse.lLastY << std::endl;
    }
}

static LRESULT CALLBACK AT_WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_DESTROY:
        PostQuitMessage(0);
        break;
    case WM_INPUT:
        AT_HandleRawInput(lParam);
        break;
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

int main()
{
    try {
        HINSTANCE hInstance = GetModuleHandleW(nullptr);
        AT_RegisterClass(hInstance, L"ATWndCls");
        HWND hWnd = AT_CreateWindow(hInstance, L"ATWndCls", L"AbsoluteTouch Test");

        RAWINPUTDEVICE dev;
        dev.usUsagePage = HID_USAGE_PAGE_GENERIC;
        dev.usUsage = HID_USAGE_GENERIC_MOUSE;
        dev.dwFlags = RIDEV_INPUTSINK;
        dev.hwndTarget = hWnd;
        if (!RegisterRawInputDevices(&dev, 1, sizeof(RAWINPUTDEVICE)))
            throw win32_error();

        std::cout << "AbsoluteTouchEx attest: listening for absolute mouse input..." << std::endl;

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
