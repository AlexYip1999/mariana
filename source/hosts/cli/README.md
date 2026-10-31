# cli（宿主 #1：命令行）

## 用法

```powershell
# 版本 / 帮助
.\build\vs2022\bin\Debug\agent-cli.exe --version
.\build\vs2022\bin\Debug\agent-cli.exe --help

# 生成一份本地覆盖模板（默认写 ./config.local.json，不覆盖已存在文件）
.\build\vs2022\bin\Debug\agent-cli.exe --init-config
# 或指定路径
.\build\vs2022\bin\Debug\agent-cli.exe --init-config --config .\my-config.local.json

# 单轮问答（需要 DEEPSEEK_API_KEY）
$env:DEEPSEEK_API_KEY = "..."
.\build\vs2022\bin\Debug\agent-cli.exe "用一句话介绍 C++20"

# 交互式终端聊天（启动后直接输入，不需要把问题作为参数）
.\build\vs2022\bin\Debug\agent-cli.exe --interactive
# 命令：/help、/clear、/exit
```

## 配置

配置分三层，后者按字段覆盖前者：

1. **内置默认值**（代码里的 `CliConfig`）；
2. **共享配置 `resources/config.json`**（提交到仓库，不含密钥；构建时复制到 exe 同目录）；
3. **本地覆盖 `config.local.json`**（gitignore，只写需要改的字段）。

不指定 `--config` 时的查找顺序：

- shared：`./config.json`，其次 `<exe 同目录>/config.json`（CMake 把 `resources/config.json` 复制到这里）；
- local：`./config.local.json`，其次 `<exe 同目录>/config.local.json`；
- CMake 构建时会把仓库根的 `config.json` 复制到 exe 同目录，所以 Rider/VS 直接跑也能读到共享配置；
- `--config <path>` 显式指定时只加载该文件，不合并共享配置。

DeepSeek 相关：

- `endpoint` 必须写完整地址：`https://api.deepseek.com/chat/completions`；
- `model` 当前为 `deepseek-flash`（或 `deepseek-v4-pro`）；
- API key 只从环境变量读，默认变量名 `DEEPSEEK_API_KEY`，可用 `api_key_env` 改名；**不要把 key 写进任何配置文件**，解析器会拒绝 `api_key` / `key` / `token` 字段。

配置项：`endpoint`、`model`、`api_key_env`、`system_prompt`、`timeout_ms`、`max_tokens`、`temperature`。

## 当前状态

- CLI 逻辑（参数解析、配置、单轮问答、错误输出）已完成，并用 `FakeHttpTransport` 做了离线单测；
- **Windows 已接入临时 `WinHttpTransport`**，可以直接真实调用（需要可用网络和 API key）；
- 跨平台正式实现待换成 libcurl；UE 宿主会用 `FHttpModule` 单独实现 `HttpTransport`。
