# AbsoluteTouchEx

AbsoluteTouchEx 让你像使用触摸屏一样使用触控板，获得**绝对光标定位**——
手指在触控板上的位置直接对应屏幕坐标，而非传统触控板的相对位移。它是
[AbsoluteTouch](https://github.com/apsun/AbsoluteTouch) 的下一代，解决了原版仅支持
Synaptics 触控板、在慢速机器上严重卡顿两大缺陷，兼容几乎所有 Windows 精密触控板
（Precision Touchpad），性能提升数个数量级。

本项目为 2026 年现代化重构版本（v2.0.0）。

## 警告

AbsoluteTouchEx 通过**注入目标进程**并解析触控板 HID 事件来工作，本质上是一个
"hack"。它**可能触发部分游戏的反作弊保护系统**。因此被封禁的后果由使用者自行承担，
作者概不负责。

## 工作原理（v2.0.0 重构）

旧版为了在目标程序中"回灌"合成的绝对坐标鼠标，采用了 `MAGIC_HANDLE(0)` 桥接 +
替换目标窗口 `WndProc` 的方案，必须 Hook 5 个 Win32 函数，强耦合目标程序的窗口
创建方式，且存在竞态与加载器锁风险。

新版改为更稳健、更标准的实现：

1. `atloader.exe` 以**创建期注入**（Detours `DetourCreateProcessWithDllEx`）将
   `atdll.dll` 注入目标进程；
2. `atdll.dll` 在独立工作线程中创建一个 **message-only 窗口**（`HWND_MESSAGE`），
   并将其注册为精密触控板 raw input 的接收者；
3. 工作线程的消息循环解析触控板 HID 多点触控报告，将物理坐标线性映射到
   `0..65535` 归一化坐标，再通过 Windows 官方推荐的 **`SendInput`**
   （`MOUSEEVENTF_ABSOLUTE`）投递绝对坐标鼠标移动；
4. 功能热键（`SHIFT+F6..F9`）也注册到自有窗口，**完全不触碰目标程序的 `WndProc`**。

由此移除了对 `RegisterRawInputDevices` / `GetRawInputData` / `CreateWindowExW` /
`Get/SetWindowLongPtr` 的全部 Hook，对目标程序**零侵入**，并且天然修复了
"启用后吞掉真实鼠标"的副作用——真实鼠标在绝对模式开启时依然可用。

## 运行

### 环境要求
- Windows 10 或更高版本
- 系统已安装 Windows 精密触控板驱动（设置 → 设备 → 触控板，顶部应显示
  "你的电脑具有精密触控板"；若未显示，本工具无法工作）

### 下载与启动
从 [Releases 页面](https://github.com/diaoyunxi/AbsoluteTouchEx/releases) 下载
打包文件并解压。其中按位数分目录提供：

```
AbsoluteTouchEx/
  x86/atloader.exe, atdll.dll, attest.exe   <- 用于 32 位目标程序
  x64/atloader.exe, atdll.dll, attest.exe   <- 用于 64 位目标程序
```

`atloader.exe` 会**自动检测目标程序的位数**：若与自身位数一致则注入；若不一致，
会明确提示你改用对应位数目录下的加载器。你仍应选择与目标程序（而非操作系统）
同位数目录中的加载器运行。

将 `atloader.exe` 与 `atdll.dll` 放在同一目录，然后执行：

```
atloader.exe <目标程序的完整路径>
```

例如，对 32 位的 osu!：

```
x86\atloader.exe %LocalAppData%\osu!\osu!.exe
```

> 目标程序（如 osu!）需要开启 **raw input 模式**，否则无法接收绝对鼠标输入。

启动后绝对触控模式默认**关闭**，在目标程序中按 `SHIFT+F6` 开启。

### 快捷键
| 快捷键 | 功能 |
|--------|------|
| `SHIFT+F6` | 绝对触控模式 开 / 关 |
| `SHIFT+F7` | 进入 / 退出校准模式 |
| `SHIFT+F8` | 加载校准配置 |
| `SHIFT+F9` | 保存校准配置 |

### 校准
按 `SHIFT+F7` 进入校准模式，在触控板上画出你想映射的区域（触碰左上角与右下角即可），
再次按 `SHIFT+F7` 应用。校准模式下光标不会移动，属正常现象。
按 `SHIFT+F9` 保存，下次运行时按 `SHIFT+F8` 加载。

## 配置文件

校准信息保存在同目录的 `atcalibration.json`（JSON 格式），示例：

```json
{
    "touch_area": {
        "left": -1000,
        "top": -1000,
        "right": 3000,
        "bottom": 2000
    }
}
```

文件采用异常安全解析：字段缺失或数值非法时会被忽略并输出调试日志，不会导致崩溃。
调试日志统一通过 `OutputDebugString` 输出，可用 DebugView 等工具查看，Release
版本不再向磁盘写日志文件。

## 构建

### 要求
- Visual Studio 2022（平台工具集 v143）；若使用 Visual Studio 2019，请将三个
  `.vcxproj` 中的 `<PlatformToolset>` 改回 `v142`
- Windows 10/11 SDK（提供 `hid.lib` / `hidpi.h`，**不需要** 单独安装 WDK）
- nlohmann/json 已作为单头文件包含在 `thirdparty/nlohmann/json.hpp`，无需额外下载

项目应可直接打开并构建，无需额外配置。

### 自动构建与发布
仓库已配置 GitHub Actions（`.github/workflows/build.yml`）：推送形如 `v*` 的
标签（或手动触发）时，会在 `windows-2022` runner 上自动编译 x86/x64 的 Release
版本，并打包为 `AbsoluteTouchEx.zip` 发布到 Releases。

> 注意：fork 仓库默认可能禁用 Actions，请到仓库 **Settings → Actions** 中启用
> "Allow all actions and reusable workflows" 后再推送标签触发。

## 相对旧版（v1.x）的主要改进

- **更稳健的注入策略**：移除脆弱的窗口 `WndProc` 替换与 `MAGIC_HANDLE` 桥接，
  改用 `SendInput` + message-only 窗口，对目标程序零侵入；
- **消除加载器锁风险**：重活移出 `DllMain`，改为独立工作线程处理；
- **现代化 C++20**：异常类修正为公有继承 `std::exception` 并重写 `what()`，
  全局状态加锁保护，配置改用 JSON 并做安全解析；
- **加载器健壮性**：自动检测目标位数、捕获异常、返回正确退出码、关闭句柄、
  等待注入完成；
- **构建修复**：修正 `detours32.lib` / `detours64.lib` 同时无条件链接导致的
  `LNK1112` 链接失败（按平台条件链接）；
- **验证工具修正**：`attest` 改用真正的 message-only 窗口（不再误弹可见窗口），
  补全错误处理。

## 许可证

见 [LICENSE.txt](LICENSE.txt)。
