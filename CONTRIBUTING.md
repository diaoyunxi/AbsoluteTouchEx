# 贡献指南

感谢你对 AbsoluteTouchEx 项目的关注！以下是参与贡献的指引。

## 开发环境

- **IDE：** Visual Studio 2019/2022（推荐）
- **平台：** Windows x86/x64
- **依赖：** Microsoft Detours 库、nlohmann/json

## 构建步骤

1. 克隆仓库：`git clone https://github.com/diaoyunxi/AbsoluteTouchEx.git`
2. 使用 Visual Studio 打开解决方案文件
3. 选择目标平台（x86 或 x64）
4. 构建解决方案（Ctrl+Shift+B）

## 代码规范

- 遵循 `.editorconfig` 中定义的代码风格
- C++ 代码使用现代 C++ 实践（C++17 及以上）
- 所有公共函数需添加注释说明
- 提交前确保编译通过且无警告

## 提交 Pull Request

1. Fork 本仓库
2. 创建功能分支：`git checkout -b feature/my-feature`
3. 提交变更：`git commit -m "feat: 添加新功能"`
4. 推送分支：`git push origin feature/my-feature`
5. 创建 Pull Request

## 提交信息规范

遵循 [Conventional Commits](https://www.conventionalcommits.org/) 规范：

- `feat:` 新功能
- `fix:` 修复 Bug
- `docs:` 文档更新
- `refactor:` 代码重构
- `chore:` 构建/工具变更

## 报告问题

请在 GitHub Issues 中报告问题，包含以下信息：

- Windows 版本
- 目标程序的位数（x86/x64）
- 复现步骤
- 期望行为与实际行为
