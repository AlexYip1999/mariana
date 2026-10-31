# model

模型适配器。引擎无关的 `ChatModel` 接口在 core，具体厂商协议实现在这里。

- `OpenAiChatModel`：OpenAI 兼容 `/chat/completions`（DeepSeek 也走这套），非流式。
- 通过构造函数注入 `HttpTransport&`，不直接依赖 libcurl / UE HTTP；
- 单测用 `tests/support/fake_http_transport.h`，离线验证请求构造与响应解析，不发真实请求。

使用示例：

```cpp
agent::model::OpenAiConfig config;
config.api_key = std::getenv("DEEPSEEK_API_KEY");
agent::model::OpenAiChatModel adapter(transport, config);

agent::core::ChatRequest request;
request.messages = {{agent::core::ChatRole::User, "hello"}};
const agent::core::ChatResponse response = adapter.Chat(request);
```
