# transport

HTTP 传输层适配器。只做一件事：把 JSON 发到 URL，拿回状态码 / body / 错误。

- 接口：[`include/agent/transport/http_transport.h`](include/agent/transport/http_transport.h)
- 实现（接口不变，按平台/宿主选择）：
  - **Windows（临时）**：`WinHttpTransport`（`src/winhttp_transport.cpp`），用系统 WinHTTP，无需第三方依赖；
  - **跨平台（正式）**：libcurl（Windows 用 Schannel，Linux/macOS 用系统 TLS）；
  - **UE 宿主**：`FHttpModule` 实现（阶段 B，不把 libcurl/WinHTTP 链进插件）。
- `agent_transport` 在 Windows 上是 STATIC 库并链接 `winhttp`；其他平台暂为 INTERFACE 库。
- 单测：`tests/http_transport_test.cpp` 使用 FakeHttpTransport；`tests/winhttp_transport_test.cpp` 使用本机 loopback 服务，覆盖正常响应、重定向拒绝、大小上限、总时限和取消，不访问外网。

`PostJson` 保持同步返回；WinHTTP 内部使用异步 I/O，以便在总时限到达或收到 `std::stop_token` 取消信号时关闭请求。回调上下文和读写缓冲区保留到最终 `HANDLE_CLOSING` 通知，避免取消后的悬空访问。

- `timeout_ms`：整个请求的总时限，默认 30000 ms，必须大于零。
- `max_response_bytes`：响应体上限，默认 8 MiB，必须大于零，包括非 2xx 和 chunked 响应。
- `stop_token`：可选取消信号；取消或超时不会把不完整正文当作成功响应返回。
- 自动重定向始终禁用；3xx 状态直接返回上层，调用方应配置最终 endpoint。

不在这里做：JSON 解析、重试策略、连接池、流式——这些属于上层（model adapter / agent runner）。
