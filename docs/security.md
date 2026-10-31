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

## 为什么不做"自研加密模块"

1. 本地加密的密钥最终必须能被程序拿到；同机/同用户攻击者同样能拿到，属于混淆而非安全。
2. 自研加密容易在 IV、模式、密钥派生上出错，收益低、风险高。
3. 如果确实要本地保存，正确做法是用 OS 凭据库（DPAPI / Keychain / Secret Service）封装一个 `SecretStore` 接口，而不是自己写 AES。

## 后续（按需）

- `SecretStore` 抽象：优先环境变量，其次 OS 凭据库；
- 提交前 secret 扫描（pre-commit hook）；
- 服务端 / UE 场景的模型网关：key 只在服务端，客户端拿短期 token。
