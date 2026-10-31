# adapters

平台 / 协议适配器目录，每个子模块同样遵循 `include/` + `src/` + `CMakeLists.txt`：

- `transport/`：HTTP 传输。`HttpTransport` 接口已定义；CLI/服务端计划用 libcurl，UE 宿主用 FHttpModule
- `model/`：OpenAI 兼容模型适配器、本地模型适配（待建）
- `storage/`：内存 / JSON / SQLite 存储（待建）

A1 当前进度：`transport` 接口 + FakeHttpTransport + 单测已完成，下一步接平台实现与 model adapter。
