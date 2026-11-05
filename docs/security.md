# 安全与密钥管理

## 原则

- **API key 不进入仓库、配置文件、日志、trace、截图和聊天记录。**
- 当前 CLI 只从环境变量读取 key（默认 `DEEPSEEK_API_KEY`），`CliConfig` 里只保存变量名 `api_key_env`。
- `ParseCliConfig` 会拒绝包含 `api_key` / `key` / `token` 字段的配置文件，防止把 key 写进 config。

## 威胁与对策

| 威胁 | 后果 | 对策 |
|---|---|---|
| 提交到仓库 | key 泄露、被盗刷 | `.gitignore` 排除 `config.local.json` / `.env`；提交前检查；配置解析拒绝 key 字段 |
| 日志 / trace | key 出现在日志 | 不打印 `Authorization` header；HTTP 层只记录状态码、耗时和错误类型 |
| 本机明文文件 | 本机被读取后泄露 | 优先环境变量；需要持久化时用 OS 凭据库（Windows DPAPI / Credential Manager、macOS Keychain、Linux Secret Service），不要自研加密 |
| 客户端 / UE 打包 | key 可被逆向提取 | **不在客户端内置长期 key**；走后端代理 / 网关，客户端只拿短期 token |
| 进程环境 | 同用户或子进程可见 | 用独立 key、最小权限账号、限额、定期轮换 |

## 工具与信息流安全

- 工具默认只读，`/workspace` 限定可访问根目录，`/tools off` 关闭工具。
- `read_file` 只接受 workspace 内相对路径；拒绝绝对路径、`..` 越界、symlink/junction 逃逸。
- Windows 上打开文件后通过 `GetFinalPathNameByHandleW` 对句柄做二次路径复核，降低 check→open TOCTOU 风险。
- 敏感文件拒绝（启发式，不是完整防护）：`.env` / `.env.*`、`config.local.json`、`.git`、`.docker`、`.kube`、`id_rsa` / `id_ed25519` / `id_ecdsa`、`.git-credentials`、`.netrc`、`.npmrc`、`.pypirc`、`.htpasswd`、`*.key` / `*.pem` / `*.pfx` / `*.p12` / `*.jks` / `*.ppk` / `*.kdbx`。
- 单文件有读取上限，且只接受 UTF-8 文本；二进制/GBK/UTF-16 会返回错误。
- 工具结果进入模型上下文后视为不可信内容；文件里的“忽略之前指令”等 prompt injection 可能影响模型决策，不能作为授权依据。
- `/trace` 写文件前会按 UTF-8 字符边界截断内容并脱敏 `sk-...`；仍应把 trace 目录视为可能含敏感业务数据。
- 不要把 workspace 指向 HOME、仓库根或包含凭据的目录；敏感目录只应通过显式 workspace 白名单开放。

## 配置信任边界

- `endpoint` 必须使用 `https://`，仅严格匹配 `localhost`、`127.0.0.1`、`[::1]` 的主机允许 `http://`，拒绝 URL 用户信息、异常端口和控制字符。
- CLI 会自动加载 cwd 的 `config.json` / `config.local.json`；若 cwd 配置修改 `endpoint`、`api_key_env`、`model` 或 `system_prompt`，默认拒绝，必须用 `--config <path>` 显式指定可信配置。
- WinHTTP 禁用自动重定向；3xx 返回上层处理，不转发 Authorization 和请求正文。单次请求遵守总时限，响应体默认上限 8 MiB（包括错误响应）。
- 观察者回调异常不会中断 Agent 执行；工具授权必须由 `ToolPolicy` 等策略执行，不能依赖日志回调抛异常。
- 不要在不可信仓库中运行 `agent-cli` 并同时持有高价值环境变量；把 key 放在独立进程环境或代理网关。
- 工具输出和配置文件内容都可能被 prompt injection 利用；授权动作（写文件、执行命令）必须走独立审批，不要只靠模型判断。

## 为什么不做"自研加密模块"

1. 本地加密的密钥最终必须能被程序拿到；同机/同用户攻击者同样能拿到，属于混淆而非安全。
2. 自研加密容易在 IV、模式、密钥派生上出错，收益低、风险高。
3. 如果确实要本地保存，正确做法是用 OS 凭据库（DPAPI / Keychain / Secret Service）封装一个 `SecretStore` 接口，而不是自己写 AES。

## 后续（按需）

- `SecretStore` 抽象：优先环境变量，其次 OS 凭据库；
- 提交前 secret 扫描（pre-commit hook）；
- 服务端 / UE 场景的模型网关：key 只在服务端，客户端拿短期 token。
