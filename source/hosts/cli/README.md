# cli（宿主 #1：命令行）

## 用法

```powershell
# 版本 / 帮助
.\build\vs2022\bin\Debug\agent-cli.exe --version
.\build\vs2022\bin\Debug\agent-cli.exe --help

# 生成一份本地覆盖模板（默认写 ./config.local.json；已存在时提示，不覆盖）
.\build\vs2022\bin\Debug\agent-cli.exe --init-config
# 或指定路径
.\build\vs2022\bin\Debug\agent-cli.exe --init-config --config .\my-config.local.json
# 写入后直接进入交互模式
.\build\vs2022\bin\Debug\agent-cli.exe --init-config --interactive

# 单轮问答（需要 DEEPSEEK_API_KEY）
$env:DEEPSEEK_API_KEY = "..."
.\build\vs2022\bin\Debug\agent-cli.exe "用一句话介绍 C++20"

# 交互式终端聊天（启动后直接输入，不需要把问题作为参数）
.\build\vs2022\bin\Debug\agent-cli.exe --interactive
# 命令：/help、/clear、/exit、/quit、/stats

# 自定义工具工作区 / 禁用工具
.\build\vs2022\bin\Debug\agent-cli.exe --workspace .\project "读取 README"
.\build\vs2022\bin\Debug\agent-cli.exe --no-tools "纯聊天，不允许工具"

# 性能/费用统计
# --stats：交互模式下每轮自动打印；单轮模式打印一次
# /stats：交互模式下手动查看当前会话统计
.\build\vs2022\bin\Debug\agent-cli.exe --stats --interactive
.\build\vs2022\bin\Debug\agent-cli.exe --stats "用一句话介绍 C++20"
```

## 工具

CLI 默认注册一个只读工具：

```text
read_file({"path":"relative/path.txt"})
```

规则：

- workspace 默认是启动 CLI 时的当前工作目录，可用 `--workspace <path>` 覆盖；`--no-tools` 完全禁用工具；
- 只允许 workspace 内的相对路径；
- 拒绝绝对路径、`..` 越界、symlink/junction 逃逸、目录和超过 256 KiB 的文件；
- 拒绝敏感文件：`.env*`、`config.local.json`、`.git`、`.docker`、`.kube`、私钥/证书文件等；
- 只接受 UTF-8 文本，二进制/GBK/UTF-16 返回错误；
- 模型通过 `tool_calls` 调用，结果以 `role=tool` 回填后继续推理；
- `max_steps` 默认 8，只计模型 Step；一次响应里的多个工具调用不受此限制；
- 单轮工具输出有 32 KiB 总预算，超限会截断并提示。

## 配置

配置分三层，后者按字段覆盖前者：

1. **内置默认值**（代码里的 `CliConfig`）；
2. **共享配置 `resources/config.json`**（提交到仓库，不含密钥；构建时复制到 exe 同目录）；
3. **本地覆盖 `config.local.json`**（gitignore，只写需要改的字段）。

不指定 `--config` 时的查找顺序：

- shared：`./config.json`，其次 `<exe 同目录>/config.json`（CMake 把 `resources/config.json` 复制到这里）；
- local：`./config.local.json`，其次 `<exe 同目录>/config.local.json`；
- `resources/config.json` 才是仓库里的共享模板；不是仓库根 `config.json`；
- cwd 配置若修改 `endpoint`、`api_key_env`、`model` 或 `system_prompt`，默认拒绝，需要 `--trust-workspace-config` 显式放行；
- `--config <path>` 显式指定时只加载该文件，不合并共享配置。

DeepSeek 相关：

- `endpoint` 必须使用 `https://`（仅 localhost 允许 http）；
- `model` 当前为 `deepseek-flash`（或 `deepseek-v4-pro`）；
- API key 只从环境变量读，默认变量名 `DEEPSEEK_API_KEY`，可用 `api_key_env` 改名；**不要把 key 写进任何配置文件**，解析器会拒绝 `api_key` / `key` / `token` 字段。

配置项：`provider`、`cost_currency`、`endpoint`、`model`、`api_key_env`、`system_prompt`、`timeout_ms`、`max_tokens`、`temperature`、`tool_names`。

## 当前状态

- A2 CLI：参数解析、配置分层、单轮/交互、`tool_names` 白名单、`--workspace`/`--no-tools`、`--stats` 已完成；
- 组合根抽为 `RunHost(args, HostEnvironment&, in/out/err)`：`HostEnvironment` 注入 env、当前目录、transport；`main` 只做控制台/参数 OS 适配；
- 组合根构造 `AgentRunner`，通过 `AgentExecutor&` 注入 `RunCli`/`RunInteractive`；
- 工具链路有 FakeHttpTransport 的端到端测试（tool_calls → read_file → role=tool → final）；
- **Windows 已接入临时 `WinHttpTransport`**，可以直接真实调用（需要可用网络和 API key）；
- 跨平台正式实现待换成 libcurl；UE 宿主会用 `FHttpModule` 单独实现 `HttpTransport`。
