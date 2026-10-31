# transport

HTTP 传输层适配器。只做一件事：把 JSON 发到 URL，拿回状态码 / body / 错误。

- 接口：[`include/agent/transport/http_transport.h`](include/agent/transport/http_transport.h)
- 当前是纯接口（INTERFACE library `agent_transport`），没有平台实现。
- 实现计划（按宿主选择，接口不变）：
  - CLI / 服务端：libcurl（Windows 用 Schannel，Linux/macOS 用系统 TLS）；
  - UE 宿主：`FHttpModule` 实现（阶段 B，不把 libcurl 链进插件）。
- 单测用 `tests/support/fake_http_transport.h`，不联网。

不在这里做：JSON 解析、重试策略、连接池、流式——这些属于上层（model adapter / agent runner）。
