# traces（JSONL 事件 trace 插件）

`FileTracer` 实现 `agent::core::Tracer`，把 AgentSystem 事件逐行写入：

```text
<trace-dir>/<session-id>.jsonl
```

事件：

- `turn_start`
- `model_request`
- `model_response`（usage / finish_reason / duration）
- `tool_call`
- `tool_result`（ok / duration）
- `turn_end`（ok / error / steps）

内容会：

- 截断到 2000 字符；
- 把 `sk-...` 形式的 key 替换为 `[REDACTED]`。

CLI：

```powershell
agent-cli --trace "hello"
agent-cli --trace-dir .\traces "hello"
```

默认 trace 目录是 `traces/`。
