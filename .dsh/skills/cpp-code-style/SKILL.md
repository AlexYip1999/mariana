---
name: cpp-code-style
description: 用于在 agent-core 项目中新增、修改、评审或提交 C++ 代码时。强制项目代码规范：Allman 大括号（右大括号必须独占一行）、PascalCase 命名（类型/函数）、snake_case 命名（变量/文件，成员变量 m_ 前缀）、4 空格缩进、120 列，并在改完代码后运行 scripts/format.ps1、scripts/check-format.ps1、scripts/verify.ps1。任何 .h/.hpp/.cpp 改动后都必须使用。
---

# C++ 代码规范（agent-core）

## 何时使用

在 agent-core 仓库新增、修改、评审 C++ 代码后（`source/*/include/`、`source/*/src/`、`tests/` 下的 `.h` / `.hpp` / `.cpp`），或准备提交前。

## 铁律

1. **块级右大括号 `}` 必须独占一行**：位于行首（保留缩进），不允许出现在语句同一行。花括号初始化（`= { ... };`、`return {};`、`v{1, 2}`）不算块级大括号。
   - 禁止：`if (ok) { Do(); }`、`} else {`、`}}  // namespace`
   - 允许：`};`、`}  // namespace agent::core`
2. **左大括号同样换行**（Allman）。大括号位置由 `.clang-format` 决定，不要手调。
3. 缩进 4 空格、不使用 Tab、行宽 120。
4. 格式以 `clang-format` 的结果为准，不要凭感觉"看起来对齐"。

## 改完代码后的固定流程（顺序不要换）

```powershell
# 本机使用 Windows PowerShell 5.1，且执行策略限制未签名脚本，必须带 -ExecutionPolicy Bypass
# 1. 自动格式化（会原地修改文件）
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\format.ps1

# 2. 格式检查，必须退出码 0
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\check-format.ps1

# 3. 格式 + 构建 + 测试 全套校验
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\verify.ps1
```

做不到全套时，至少执行：格式检查 + `cmake --build --preset debug` + `ctest --preset debug`。

提交前再执行 `git status`，确认没有 `build/`、`traces/`、密钥、本地配置等文件被加入。

## 提交规则（必须遵守）

- **绝不自动执行 `git commit` / `git push`**：改完代码、跑完 format/check/verify 后，汇总改动与测试结果，等待用户明确确认后再提交。
- 用户确认后再提交；提交信息用约定式前缀：`feat:` / `fix:` / `style:` / `test:` / `docs:` / `chore:`。

## 命名规范（类型/函数 PascalCase，变量 snake_case）

| 元素 | 规则 | 示例 |
|---|---|---|
| 类型：class / struct / enum / using 别名 | PascalCase | `ToolRegistry`、`RunState`、`HttpClient` |
| 函数 / 方法（含自由函数、测试辅助） | PascalCase | `BuildBanner()`、`ExecuteTool()` |
| 变量 / 参数（局部、函数参数） | snake_case | `max_steps`、`request_count` |
| 成员变量（类/对象） | `m_` + snake_case | `m_handler`、`m_request_count` |
| 常量 / 宏 | UPPER_SNAKE_CASE | `AGENT_CORE_VERSION`、`MAX_STEPS` |
| 命名空间 | 全小写 snake_case | `agent::core`、`agent::transport` |
| C++ 文件名（源文件 / 头文件） | 全小写 snake_case | `http_transport.h`、`fake_http_transport.h`、`version.cpp` |
| 枚举值 | PascalCase | `RunState::Waiting` |

### 文件命名

- C++ 源文件与头文件一律**全小写 snake_case**，不使用驼峰（`HttpTransport.h`）、中划线（`http-transport.h`）或空格。
- 文件名不要求和类名完全一致；类型名在文件内保持 PascalCase（`http_transport.h` 里放 `HttpTransport`）。
- 理由：Linux 大小写敏感、Windows/macOS 不敏感，全小写可以避免跨平台构建、CI 和 Git 大小写改名问题；同时与目录（`source/adapters/transport`）、命名空间（`agent::transport`）保持一致。
- 例外：`README.md`、`.clang-format` 等社区惯例文件名，以及脚本/文档文件名不强制 snake_case（脚本当前用 kebab-case，如 `check-format.ps1`；如需统一可另行决定）。

- 函数/方法用 PascalCase，变量/参数/成员用 snake_case；不要用小驼峰命名函数（`processData` 不行）。
- 缩写按单词处理：类型 `HttpClient`，函数 `ParseHttpResponse`。
- **接口不加 `I` 前缀**：抽象基类用角色名，具体实现加实现名。例如 `HttpTransport` → `CurlHttpTransport` / `UeHttpTransport`；`ModelAdapter` → `OpenAiModelAdapter`。文件名与类名一致（`http_transport.h`）。
- clang-format 只负责排版，**不检查命名**；命名靠评审与这条规则约束。
- 不要引入 UE 宏风格（`UPROPERTY` 等属于 UE 宿主层，core 层禁止）。

## 其他约定

- 头文件使用 `#pragma once`；include 分组与排序交给 clang-format。
- **不要格式化或修改 `third_party/`、`build/`**。
- 不要修改 `.clang-format`、`scripts/*.ps1`，除非任务明确就是改规范；规范变更单独提交。
- core 层禁止 include UE / 平台 / 网络库头文件；依赖通过接口（见 `Agent项目-实施路径.md`）。

## 完成定义（DoD）

- [ ] `scripts/check-format.ps1` 通过（退出码 0）
- [ ] `scripts/verify.ps1` 通过（或格式、构建、测试分别通过）
- [ ] 块级 `}` 都在行首（初始化列表 / `return {};` 除外）
- [ ] 命名符合规范：类型/函数 PascalCase，变量/文件 snake_case，成员变量 `m_` 前缀
- [ ] `third_party/`、`build/` 未被改动
- [ ] `git status` 无意外文件
