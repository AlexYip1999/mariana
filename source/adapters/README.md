# adapters

平台 / 协议适配器目录，每个子模块同样遵循 `include/` + `src/` + `CMakeLists.txt`：

- `transport/`：HTTP 传输。`HttpTransport` 接口 + Windows 临时 `WinHttpTransport`；跨平台正式实现将换 libcurl，UE 宿主用 FHttpModule
- `model/`：模型适配。`ModelAdapter` 接口在 core，`OpenAiModelAdapter` 已实现（OpenAI 兼容 / DeepSeek）
- `storage/`：内存 / JSON / SQLite 存储（待建）

A1 当前进度：`transport`（WinHTTP 临时）+ `model` + CLI 单轮问答 + 离线单测已完成，下一步真实联调与 A2 工具循环。
