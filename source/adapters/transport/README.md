# transport

HTTP 传输层适配器。只做一件事：把 JSON 发到 URL，拿回状态码 / body / 错误。

- 接口：[`include/agent/transport/http_transport.h`](include/agent/transport/http_transport.h)
- 实现（接口不变，按平台/宿主选择）：
  - **Windows（临时）**：`WinHttpTransport`（`src/winhttp_transport.cpp`），用系统 WinHTTP，无需第三方依赖；
  - **跨平台（正式）**：libcurl（Windows 用 Schannel，Linux/macOS 用系统 TLS）；
  - **UE 宿主**：`FHttpModule` 实现（阶段 B，不把 libcurl/WinHTTP 链进插件）。
- `agent_transport` 在 Windows 上是 STATIC 库并链接 `winhttp`；其他平台暂为 INTERFACE 库。
- 单测：`tests/http_transport_test.cpp`（FakeHttpTransport，不联网）、`tests/winhttp_transport_test.cpp`（非法 URL 错误路径，不联网）。

不在这里做：JSON 解析、重试策略、连接池、流式——这些属于上层（model adapter / agent runner）。
