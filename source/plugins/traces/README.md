# traces（JSONL 事件 trace 插件）

`FileTracer` 实现 `agent::core::Tracer`，把 AgentSystem 事件逐行写入：

```text
<trace-dir>/<session-id>.jsonl
```

`FileTracer` 负责事件格式化和脱敏，输出交给可替换的 `TraceSink`；默认 `JsonlFileSink` 写文件。

trace 插件默认开启，默认目录 `traces/`。

## CLI

```text
/trace
/trace show
/trace show <file>
/stats today
/stats all
```

- `/trace`：查看当前 trace；
- `/trace show [file]`：查看当前或指定 trace；文件名会回退到 trace 目录解析；
- `/stats today|all`：聚合 trace usage。

`/stats`（无参数）显示当前进程内统计。