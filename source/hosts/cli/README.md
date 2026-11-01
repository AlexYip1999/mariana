# cli（宿主 #1：命令行）

CLI 是交互式 app。启动参数只保留最小 bootstrap；trace 插件默认开启。

## 启动

```powershell
# 帮助 / 版本
.\build\vs2022\bin\Debug\agent-cli.exe --help
.\build\vs2022\bin\Debug\agent-cli.exe --version

# 启动交互
$env:DEEPSEEK_API_KEY = "..."
.\build\vs2022\bin\Debug\agent-cli.exe

# 显式配置
.\build\vs2022\bin\Debug\agent-cli.exe --config .\my-config.json
```

启动只接受：

```text
--help
--version
--config <path>
```

## 交互命令

| 命令 | 作用 |
|---|---|
| `/help` | 命令列表 |
| `/version` | 版本 |
| `/config show` | 当前配置 |
| `/config init` | 写 `config.local.json` |
| `/workspace <path>` | 切换工具工作区 |
| `/tools on\|off` | 开/关工具 |
| `/trace` | 查看当前 trace |
| `/trace show [file]` | 查看当前或指定 trace |
| `/stats` | 当前会话统计 |
| `/stats auto on\|off` | 每轮自动打印统计 |
| `/stats today\|all` | 聚合 trace usage |
| `/clear` | 清空历史 |
| `/exit` / `/quit` | 退出 |

示例：

```text
> /workspace .\project
> /tools on
> 读取 README.md 并总结
> /trace
> /stats today
> /exit
```

## Trace

trace 默认开启，目录为 `traces/`。

```text
> /trace
```

查看当前进程的 trace。

```text
> /trace show
> /trace show some-session.jsonl
```

`/trace show <name>` 先按当前目录解析，找不到再回退到 `traces/`。

## 工具

默认注册一个只读工具：

```text
read_file({"path":"relative/path.txt"})
```

规则：

- workspace 默认是当前工作目录，可用 `/workspace <path>` 覆盖；
- `/tools off` 清空工具；`/tools on` 重新注册 `read_file`；
- 只允许 workspace 内的相对路径；
- 拒绝绝对路径、`..` 越界、symlink/junction 逃逸、目录和超过 256 KiB 的文件；
- 拒绝敏感文件：`.env*`、`config.local.json`、`.git`、`.docker`、`.kube`、私钥/证书文件等；
- 只接受 UTF-8 文本，二进制/GBK/UTF-16 返回错误；
- 模型通过 `tool_calls` 调用，结果以 `role=tool` 回填后继续推理；
- `max_steps` 默认 8，只计模型 Step；
- 单轮工具输出有 32 KiB 总预算。

## 配置

配置分三层，后者按字段覆盖前者：

1. **内置默认值**；
2. **共享配置 `resources/config.json`**；
3. **本地覆盖 `config.local.json`**。

不指定 `--config` 时的查找顺序：

- shared：`./config.json`，其次 `<exe 同目录>/config.json`；
- local：`./config.local.json`，其次 `<exe 同目录>/config.local.json`；
- cwd 配置若修改 `endpoint`、`api_key_env`、`model` 或 `system_prompt`，默认拒绝；
- `--config <path>` 显式指定时只加载该文件。

配置项：`provider`、`cost_currency`、`endpoint`、`model`、`api_key_env`、`system_prompt`、`timeout_ms`、`max_tokens`、`max_steps`、`temperature`、`max_context_chars`、`tool_names`。

API key 只从环境变量读，默认 `DEEPSEEK_API_KEY`。

## 当前状态

- A2 CLI：最小启动参数 + `CommandRegistry` 交互命令插件；
- 命令实现为 `CliCommand`，`/help` 自动列出注册命令；
- 组合根抽为 `RunHost(args, HostEnvironment&, in/out/err)`；
- 组合根构造 `AgentRunner`，通过 `AgentExecutor&` 注入 `RunInteractive`；
- trace 默认开启，`/trace` 查看当前 trace；
- `--stats` / `--session` 等旧参数已移除。
