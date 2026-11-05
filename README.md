# 马里亚纳之心（Mariana Heart）

跨平台、宿主无关的 Agent 运行时。同一份 core 计划支撑 CLI、UE 编辑器/运行时、服务端等宿主；UE 接入只是宿主适配阶段（阶段 B），不是起点。

仓库：[AlexYip1999/mariana](https://github.com/AlexYip1999/mariana)
本地目录为 `agent-core`；代码命名空间暂用 `agent::`，后续统一改名。

**当前状态：A2.5（F0 运行时收口）已完成，A3（F1）进行中** —— Windows 已接入临时 WinHTTP 传输；已实现 `Agent`/`AgentSession`/`AgentRunner`、scoped `ServiceRegistry`、ReAct 工具循环、`read_file`、取消/预算/并发硬化、Turn 事务、结构化 TurnEvent、ToolDescriptor、组合校验、CallError/RunErrorCode 分层、HostServices/AgentFactory/RunLoop、metrics、trace、CLI `CommandRegistry` 交互。CTest 覆盖 core + runtime + tools + metrics + model + transport + CLI。

仓库里**还没有 UE 插件**；`hosts/unreal/` 只是规划。

## 目录

| 路径 | 说明 |
|---|---|
| `source/` | 源码根目录：每个模块自包含 `include/` + `src/` + `CMakeLists.txt`（见 `source/README.md`） |
| `source/core/` | 引擎无关核心：`include/agent/` 公共 API + `src/` 实现 |
| `source/integrations/` | 外部集成：模型、传输、存储等平台/协议实现 |
| `source/integrations/transport/` | 传输层：`HttpTransport` 接口 + Windows 临时 `WinHttpTransport`；跨平台将换 libcurl |
| `source/integrations/model/` | 模型层：`ChatModel` 接口在 core，`OpenAiChatModel` 在 integrations |
| `source/runtime/` | 通用 Agent 运行时：`Agent`（身份 + `AgentScope` + 会话 + `AgentDependencies`）、scoped `ServiceRegistry`、`AgentSession`（`Send`）、`AgentExecutor`、`AgentRunner`、`ExecuteTurn`、`RunContext`、`TurnTransaction`、`TurnWorkspace`、`RunOutcome`、`TurnEvent`、`ToolDispatcher`、`ToolDescriptor`、`ValidateComposition`、`ToolRegistry`、`ToolPolicy`/`ContextPolicy`/`ToolOutputPolicy` |
| `source/plugins/tools/` | 内置工具实现：`ReadFileTool`（WorkspacePathPolicy、句柄级复核、敏感文件拒绝、UTF-8 校验、大小限制） |
| `source/plugins/traces/` | JSONL trace 插件：默认开启，记录 turn/model/tool/usage，内容截断与 key 脱敏；usage 聚合与 trace viewer |
| `source/plugins/metrics/` | metrics 插件：token/速度/缓存/上下文/费用统计 |
| `source/hosts/cli/` | 宿主 #1：命令行（`agent-cli`，交互式；启动仅 `--help/--version/--config`，其他用命令） |
| `tests/` | 单元测试（doctest） |
| `scripts/` | 格式化 / 检查 / 校验脚本（PowerShell） |
| `third_party/` | 单头依赖（nlohmann/json、doctest） |
| `.dsh/skills/` | 项目级 DSH skill（`cpp-code-style` 代码规范） |
| `eval/` | 评测任务与脚本（A3 起） |

## 运行时架构

运行配置由 Session 单独持有，Runner 通过 `RunnerDependencies` 显式装配；`RunContext` 贯穿取消、截止时间和预算。`TurnTransaction` 由 Session 持有并统一处理历史提交/回滚；`TurnEvent` 在 commit/rollback 后由 Session 发布，`SystemNotifier` 在 runtime 层快照分发并隔离观察者异常。`ToolDispatcher` 处理策略、预算、去重、重放事件和执行记录。同一 Session 拒绝并发和重入，不同 Session 可并行；metrics / trace 由宿主按会话创建。宿主事件队列/UI 线程投递留到后续阶段。

完整对象模型、生命周期和线程契约见 [运行时架构](docs/runtime-architecture.md)。

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
- **右大括号 `}` 必须独占一行**（行首，保留缩进）：块级大括号 + 多行初始化列表的嵌套元素（后者 clang-format 不会自动展开，由 `check-format.ps1` 检查）；单行语句（`return {};`、`= {1, 2};`）例外；禁止 `if (...) { ... }`、`} else {`、多行列表里的 `{..., ...},`。
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

## 安全（密钥与工具）

- API key 不写进仓库 / 配置文件 / 日志；CLI 只从环境变量 `DEEPSEEK_API_KEY` 读取。
- 配置文件里的 `api_key` / `key` / `token` 字段会被解析器拒绝。
- `endpoint` 必须是 `https://`；仅主机严格匹配 `localhost`、`127.0.0.1`、`[::1]` 时允许 HTTP。
- 自动加载的 cwd 配置若修改 `endpoint` / `api_key_env` / `model` / `system_prompt`，默认拒绝；需要时用 `--config <path>` 显式指定可信配置。
- HTTP 不自动跟随重定向，避免把 API key 和正文转发到其他地址；请求有总时限，响应默认限制为 8 MiB。
- 工具默认只能读 workspace 内的文本文件；拒绝 `.env`、`config.local.json`、`.git`、私钥等敏感文件。
- 本地持久化请用 OS 凭据库，不要自研加密；UE / 打包客户端不要内置长期 key，走代理网关。
- 详细威胁模型见 [`docs/security.md`](docs/security.md)。

## 配置

- `resources/config.json`（提交到仓库）：共享默认值，不含密钥；构建时复制到 exe 同目录；
- `config.local.json`（gitignore）：本机覆盖，只写需要改的字段，字段级覆盖 shared；
- 内置默认值兜底；`--config <path>` 显式指定时只加载该文件；
- `tool_names` 为空数组表示禁用工具；非空名字必须存在于 ToolRegistry（CLI 当前只注册 `read_file`）；
- `max_context_chars` 控制发送给模型的 conversation 字符预算（0 = 不裁剪，默认 120000）；
- `timeout_ms` 是每次模型 HTTP 请求的总时限（默认 30000 ms，必须为正整数），包含响应正文接收；
- `turn_timeout_ms` 限制整轮总耗时（默认 120000 ms）；`max_tool_calls` / `max_tool_output_bytes` 限制工具尝试次数和正文总输出（默认 64 / 32768）；
- runtime 宿主可向 `Agent::Send(input, stop_token)` 传入取消信号，贯穿模型、工具调度和 HTTP；CLI 当前没有交互式取消命令；
- trace 插件默认开启；`/trace` 查看当前 trace，`/trace show <file>` 查看指定 trace；
- `/stats today|all` 聚合 trace usage；

## 设计文档

- [框架设计](docs/framework.md)
- [Agent 循环设计](docs/agent-loop-design.md)
- [运行时架构](docs/runtime-architecture.md)
- [安全与密钥管理](docs/security.md)

## 下一步

- 真实 API 联调：设置 `DEEPSEEK_API_KEY` 后启动 `agent-cli`，在交互提示符输入“读取 README.md 并总结”；
- file tracer / usage 持久化已实现；继续验证真实服务下的错误处理与使用体验；
- 跨平台正式实现：把 `WinHttpTransport` 替换为 libcurl（接口不变，UE 宿主仍用 FHttpModule）；
- 之后进入 F1/A3：ContextProvider、Memory、Eval。

CLI 的配置与用法见 [`source/hosts/cli/README.md`](source/hosts/cli/README.md)，共享默认见 [`resources/config.json`](resources/config.json)。

## 第三方

- [nlohmann/json](https://github.com/nlohmann/json) 3.11.2（MIT），单头文件
- [doctest](https://github.com/doctest/doctest) 2.4.6（MIT），单头文件

## License

项目许可证待定；第三方组件按其各自许可证使用（见 `third_party/`）。
