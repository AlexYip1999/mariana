# agent-core

跨平台、宿主无关的 Agent 核心。同一份 core 计划支撑 CLI、UE 编辑器/运行时、服务端等宿主；UE 接入只是宿主适配阶段（阶段 B），不是起点。

**当前状态：A0 骨架 + A1 接口起步** —— 可构建、`agent-cli --version`、CTest 覆盖 core + transport 接口。下一步 A1：接平台 HTTP 实现 + 模型适配器。

## 目录

| 路径 | 说明 |
|---|---|
| `source/` | 源码根目录：每个模块自包含 `include/` + `src/` + `CMakeLists.txt`（见 `source/README.md`） |
| `source/core/` | 引擎无关核心：`include/agent/` 公共 API + `src/` 实现 |
| `source/adapters/` | 平台 / 协议适配器（transport 接口已定义；model、storage 待建） |
| `source/adapters/transport/` | 传输层：`HttpTransport` 接口（CLI 用 libcurl、UE 用 FHttpModule） |
| `source/hosts/cli/` | 宿主 #1：命令行（`src/main.cpp`） |
| `tests/` | 单元测试（doctest） |
| `scripts/` | 格式化 / 检查 / 校验脚本（PowerShell） |
| `third_party/` | 单头依赖（nlohmann/json、doctest） |
| `.dsh/skills/` | 项目级 DSH skill（`cpp-code-style` 代码规范） |
| `docs/learning/` | 边做边学笔记（U0…，模板见 README） |
| `eval/` | 评测任务与脚本（A3 起） |

## 构建

要求：Windows + Visual Studio 2022（含 C++ 工作负载；CMake 组件默认随 VS 安装）。

### 方式一：VS 2022 打开文件夹（推荐）

VS 2022 → 文件 → 打开 → 文件夹 → 选择本目录。VS 会自动读取 `CMakePresets.json`（`vs2022`），选择 `agent-cli` 即可构建/调试。

### 方式二：命令行

CMake 未在 PATH 时，用 VS 自带路径：

```powershell
$cmake = "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$ctest = "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe"

& $cmake --preset vs2022
& $cmake --build --preset debug
& $ctest --preset debug
& ".\build\vs2022\bin\Debug\agent-cli.exe" --version
```

## 代码规范

- [`.clang-format`](.clang-format)：基于 Microsoft 风格 + **Allman** 大括号。
- **块级右大括号 `}` 必须独占一行**（行首，保留缩进）；花括号初始化（`= { ... };`、`return {};`）不算；禁止 `if (...) { ... }`、`} else {` 这类写法。
- 4 空格缩进、无 Tab、行宽 120；`third_party/` 和 `build/` 不参与格式化。
- **命名**：类型与函数/方法 PascalCase；变量/参数 snake_case；成员变量 `m_` + snake_case；常量/宏 UPPER_SNAKE_CASE；命名空间全小写 snake_case；**C++ 源文件/头文件全小写 snake_case**（不用驼峰/中划线）。clang-format 只负责排版，命名靠评审与 `cpp-code-style` skill 约束。
- 修改 C++ 代码后的固定流程（本机是 Windows PowerShell 5.1 + 执行策略限制，需带 `-ExecutionPolicy Bypass`）：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\format.ps1        # 自动格式化（原地修改）
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\check-format.ps1  # 必须退出码 0
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\verify.ps1        # 格式 + 构建 + ctest
```

> clang-format 由脚本自动定位：优先 `$env:CLANG_FORMAT`，其次 PATH，最后 Visual Studio 自带 LLVM（本机 `VC\Tools\Llvm\x64\bin\clang-format.exe`）。

- **提交需确认**：改完并跑完检查后先汇报改动与测试结果，等明确确认后再 `git commit`；不要自动提交，也不自动 push。

项目级 skill `cpp-code-style` 位于 `.dsh/skills/cpp-code-style/`，DSH 会话会自动加载，提示 agent 按上述流程检查。

## 下一步（对应学习计划 U1）

- `source/adapters/transport/`：`HttpTransport` 接口已就绪；平台实现待接入（CLI 用 libcurl、UE 宿主用 FHttpModule）
- `source/adapters/model/`：`ModelAdapter` + `OpenAiModelAdapter`（DeepSeek）
- `source/hosts/cli/`：CLI 单轮问答 + `config.json`

学习与构建节奏见工作区文档：`../Agent项目-学习与构建计划.md`、`../Agent项目-实施路径.md`。

## 第三方

- [nlohmann/json](https://github.com/nlohmann/json) 3.11.2（MIT），单头文件
- [doctest](https://github.com/doctest/doctest) 2.4.6（MIT），单头文件

## License

项目许可证待定；第三方组件按其各自许可证使用（见 `third_party/`）。
