// atloader.cpp — AbsoluteTouchEx 加载器（现代化重构版 v2.0.0）
//
// 职责：将 atdll.dll 注入到目标进程。采用 Detours 的创建期注入
// （DetourCreateProcessWithDllExW），在目标进程入口前完成 DLL 加载。
//
// 相比旧版的改进：
//   1. 自动检测目标 exe 的位数（解析 PE 头 IMAGE_FILE_HEADER.Machine），
//      与加载器自身位数比对：匹配则注入，不匹配则给出清晰指引，避免盲目失败；
//   2. 全面使用宽字符 API，正确处理含空格 / 非 ASCII 路径；
//   3. 异常安全：捕获 win32_error 并以非零退出码返回，便于脚本判断；
//   4. 关闭进程 / 线程句柄，并在返回前等待注入完成（WaitForInputIdle）。

#include <exception>
#include <iostream>
#include <string>
#include <vector>
#include <Windows.h>
#include "detours.h"

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

enum class TargetArch { Unknown, X86, X64 };

// 读取整个文件到内存（带大小上限，避免异常大的文件）
static std::vector<BYTE> ReadFileBytes(const std::wstring &path)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) throw win32_error();

    LARGE_INTEGER fileSize;
    if (!GetFileSizeEx(h, &fileSize)) {
        CloseHandle(h);
        throw win32_error();
    }
    if (fileSize.QuadPart > (LONGLONG)256 * 1024 * 1024) {
        CloseHandle(h);
        throw std::runtime_error("target file too large");
    }

    std::vector<BYTE> buf(static_cast<size_t>(fileSize.QuadPart));
    DWORD read = 0;
    if (!ReadFile(h, buf.data(), static_cast<DWORD>(buf.size()), &read, nullptr) ||
        read != buf.size()) {
        CloseHandle(h);
        throw win32_error();
    }
    CloseHandle(h);
    return buf;
}

// 通过 PE 头判断目标 exe 的机器架构
static TargetArch DetectArch(const std::vector<BYTE> &buf)
{
    if (buf.size() < sizeof(IMAGE_DOS_HEADER))
        return TargetArch::Unknown;

    const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(buf.data());
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return TargetArch::Unknown;

    const size_t ntOffset = static_cast<size_t>(dos->e_lfanew);
    if (ntOffset + sizeof(IMAGE_NT_HEADERS64) > buf.size())
        return TargetArch::Unknown;

    const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(buf.data() + ntOffset);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return TargetArch::Unknown;

    switch (nt->FileHeader.Machine) {
    case IMAGE_FILE_MACHINE_I386:  return TargetArch::X86;
    case IMAGE_FILE_MACHINE_AMD64: return TargetArch::X64;
    default:                       return TargetArch::Unknown;
    }
}

// 注入并启动目标进程
static void InjectAndLaunch(const std::wstring &targetPath, const std::wstring &dllPath)
{
    // 注意：Detours 的 lpDllName 参数始终为 ANSI（LPCSTR），即使使用 W 版本的
    // 进程创建函数也是如此（引导代码内部用 LoadLibraryA）。因此需将 DLL 的宽
    // 路径转换为本地 ANSI 代码页字符串后再传入。
    int ansiLen = WideCharToMultiByte(CP_ACP, 0, dllPath.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (ansiLen == 0) throw win32_error();
    std::string dllAnsi(static_cast<size_t>(ansiLen), '\0');
    if (WideCharToMultiByte(CP_ACP, 0, dllPath.c_str(), -1, dllAnsi.data(), ansiLen, nullptr, nullptr) == 0)
        throw win32_error();

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    // DetourCreateProcessWithDllEx 在 UNICODE（本项目 CharacterSet=Unicode）下即 W 版本，
    // 进程路径可用宽字符（正确处理含空格 / 非 ASCII 路径）。
    if (!DetourCreateProcessWithDllEx(
            targetPath.c_str(),
            nullptr,
            nullptr,
            nullptr,
            TRUE,
            0,
            nullptr,
            nullptr,
            &si,
            &pi,
            dllAnsi.c_str(),
            nullptr)) {
        throw win32_error();
    }

    // 等待目标进程完成初始化（含 DLL 注入与窗口创建），确认注入生效
    WaitForInputIdle(pi.hProcess, 10000);

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
}

int wmain(int argc, wchar_t *argv[])
{
    try {
        wchar_t selfPathBuf[MAX_PATH];
        DWORD n = GetModuleFileNameW(nullptr, selfPathBuf, MAX_PATH);
        if (n == 0) throw win32_error();
        std::wstring selfPath(selfPathBuf, n);

        // 取自身所在目录
        std::wstring exeDir = selfPath.substr(0, selfPath.find_last_of(L'\\'));
        std::wstring dllPath = exeDir + L"\\atdll.dll";

        // 默认目标为同目录 attest.exe（验证工具），否则取命令行第一个参数
        std::wstring targetPath = exeDir + L"\\attest.exe";
        if (argc >= 2) {
            targetPath = argv[1];
        }

        // 自动检测目标位数
        TargetArch target = DetectArch(ReadFileBytes(targetPath));
        if (target == TargetArch::Unknown) {
            std::wcerr << L"无法确定目标程序的位数，请确认路径是否正确：" << targetPath << std::endl;
            return 1;
        }

#ifdef _WIN64
        TargetArch selfArch = TargetArch::X64;
#else
        TargetArch selfArch = TargetArch::X86;
#endif

        if (target != selfArch) {
            std::wcerr << L"目标程序为 "
                       << (target == TargetArch::X64 ? L"64" : L"32")
                       << L" 位，但当前加载器是 "
                       << (selfArch == TargetArch::X64 ? L"64" : L"32")
                       << L" 位。请使用对应位数目录下的 atloader.exe。" << std::endl;
            return 1;
        }

        InjectAndLaunch(targetPath, dllPath);
        std::wcout << L"已成功将 atdll.dll 注入到：" << targetPath << std::endl;
        std::wcout << L"绝对触控模式默认关闭，在目标程序中按 SHIFT+F6 开启。" << std::endl;
        return 0;
    } catch (const win32_error &e) {
        std::wcerr << L"注入失败，错误码：0x" << std::hex << e.code() << std::endl;
        return 1;
    } catch (const std::exception &e) {
        std::wcerr << L"发生异常：" << e.what() << std::endl;
        return 1;
    }
}
