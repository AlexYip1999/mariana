# agent-eval 离线评测

`eval/` 提供 agent-core 的可复现离线评测：

- `eval/tasks.json`：12 条固定任务，覆盖正常 ReAct 循环（单读 / 顺序双读 / 多步）、工具错误恢复（缺文件 / 未知工具）、fatal 协议错误（非法工具名 / 空回复 / `finish_reason=length`）、预算终止（`ModelCallLimit` / `ToolCallLimit`）与长期记忆召回（单轮注入 / 跨轮 `follow_up_inputs`）。
- `source/eval/`：`agent-eval` 可执行文件（单文件实现）。逐条执行任务、按 `expect` 判定，输出每任务一行结果与汇总，可选写 JSON 报告。
- **mock 模式**使用脚本化模型（`ScriptedChatModel`），零网络、无外部依赖、结果确定（除 `latency_ms` 外完全可复现）。记忆任务另用 `RecordingChatModel` 装饰器记录每次 `ChatRequest`，供 `request_contains` 校验召回是否真的注入了 prompt。

## 构建与运行

```powershell
cmake --preset vs2022
cmake --build --preset debug
```

产物：`build/vs2022/bin/Debug/agent-eval.exe`（与 `agent-cli` 相同的输出目录约定）。

```powershell
# 离线 mock（默认阈值 0.7）
.\build\vs2022\bin\Debug\agent-eval.exe --tasks eval/tasks.json --mode mock

# 输出 JSON 报告 + 每条任务的额外细节
.\build\vs2022\bin\Debug\agent-eval.exe --tasks eval/tasks.json --mode mock `
    --report build\eval-report.json --verbose

# real 模式：真实 DeepSeek（需要网络与 DEEPSEEK_API_KEY；忽略 script）
$env:DEEPSEEK_API_KEY = "sk-..."
.\build\vs2022\bin\Debug\agent-eval.exe --tasks eval/tasks.json --mode real --min-pass-rate 0.0

# 通过 ctest 运行 mock 评测
ctest --preset debug -R agent_eval_mock
```

real 模式使用 WinHTTP + `OpenAiChatModel`（默认 endpoint `https://api.deepseek.com/chat/completions`、模型 `deepseek-flash`），用真实模型执行任务，`script` 被忽略。`expect` 仍按真实结果判定，因此通常配合 `--min-pass-rate 0` 用于观察轨迹；real 模式仅支持 Windows（非 Windows 直接以退出码 2 结束）。

## CLI 参数

| 参数 | 必填 | 默认值 | 说明 |
|---|---|---|---|
| `--tasks <path>` | 是 | — | 任务文件（JSON） |
| `--mode mock\|real` | 否 | `mock` | `mock` 脚本化离线；`real` 真实 API |
| `--min-pass-rate <ratio>` | 否 | `0.7` | 通过率阈值，取值 `[0, 1]` |
| `--report <json>` | 否 | — | 写 JSON 报告到指定路径 |
| `--workspace <dir>` | 否 | `<tasks 目录>/workspace` | fixture 工作区根目录 |
| `--verbose` / `-v` | 否 | — | 每条任务多输出一行 `status`/`error_code`/`tool_attempts`/`model_calls`/`tool_results_ok`/`request_contains` |
| `--help` / `-h` | 否 | — | 打印用法，退出码 0 |

**退出码**：`0` 通过率 >= 阈值；`1` 通过率 < 阈值；`2` 参数/任务文件/工作区/`DEEPSEEK_API_KEY`/写报告等输入或环境错误。

## 输出示例

```text
agent-eval: mode=mock tasks=12 workspace=E:\AI\uniagent\agent-core\eval\workspace
[PASS] single_read_notes kind=single_read steps=2 tools=[read_file] latency_ms=0 tokens=115
[PASS] sequential_double_read kind=sequential_read steps=3 tools=[read_file, read_file] latency_ms=0 tokens=180
...
[PASS] budget_tool_call_limit kind=tool_call_limit steps=1 tools=[read_file] latency_ms=0 tokens=55
[PASS] long_term_memory_injection kind=memory steps=1 tools=[] latency_ms=0 tokens=70
[PASS] long_term_memory_across_turns kind=memory_followup steps=1 tools=[] latency_ms=0 tokens=150
summary: passed=12 total=12 pass_rate=1.000 threshold=0.700
```

`[PASS]` / `[FAIL]` 行固定包含：ID、`kind`、`steps`、`tools`、`latency_ms`、`tokens`；失败时追加 `failure=<原因>`（多条断言不满足时以 `; ` 连接）。多轮任务的 `latency_ms` / `tokens` / `model_calls` 覆盖所有轮次，`steps` / `tools` / `tool_results_ok` 反映最后一轮。

## 任务 JSON schema

顶层既可以是任务数组，也可以是 `{"version": 1, "tasks": [...]}` 对象（两种都支持）。任务字段：

| 字段 | 类型 | 必填 | 说明 |
|---|---|---|---|
| `id` | string | 是 | 任务 ID，文件内唯一；同时作为 `RunOptions.run_id` |
| `kind` | string | 是 | 任务分类标签（如 `single_read`），仅用于输出与统计 |
| `description` | string | 否 | 说明文本；写入 `--report` |
| `input` | string | 是 | 传给 `Agent::Send` 的用户输入 |
| `system_prompt` | string | 否 | 覆盖默认系统提示词 |
| `follow_up_inputs` | array[string] | 否 | 多轮输入：首次 `input` 之后依次对同一 `Agent` 继续 `Send`；某一轮不是 `Completed` 即停止 |
| `max_steps` | int | 否 | 覆盖 `AgentProfile::max_steps`（默认 8）；超限产生 `ModelCallLimit` |
| `max_tool_calls` | int | 否 | 覆盖 `AgentProfile::max_tool_calls`（默认 64）；超限产生 `ToolCallLimit` |
| `max_tool_output_bytes` | int | 否 | 覆盖 `AgentProfile::max_tool_output_bytes`（默认 32768） |
| `memory` | array[object] | 否 | seed 长期记忆（字段见下表）；非空时写入 `InMemoryMemoryStore`、启用召回并挂到 `RunnerDependencies.memory` |
| `script` | array | mock 必需 | mock 模式按顺序返回的模型响应；real 模式忽略 |
| `expect` | object | 是 | 判定条件 |

`memory` 的每个元素是一条 `core::MemoryRecord`：

| 字段 | 类型 | 必填 | 默认 | 说明 |
|---|---|---|---|---|
| `id` | string | 是 | — | 记忆 ID；`Remember` 按 id 覆盖/新增 |
| `kind` | string | 否 | `"Fact"` | `Fact` / `Event` / `Episode` / `Preference` / `Relationship` / `Goal`；注入时渲染为 `[Kind] text` |
| `text` | string | 是 | — | 给模型看的自然语言内容（召回注入的正文） |
| `tags` | array[string] | 否 | `[]` | 召回命中用；中文没有分词，query 包含某个 tag 也算命中 |
| `importance` | number | 否 | `0.5` | 召回排序权重（文本命中原先，然后 importance、`updated_at`、id） |
| `confidence` | number | 否 | `1.0` | 可信度，原样写入记录 |
| `scope` | object | 否 | `{"kind": "cli", "id": "default"}` | 记忆作用域；`kind` 默认 `cli`、`id` 默认 `default` |
| `payload_json` | string | 否 | `""` | Host 精确查询用的结构化数据；query 文本命中它也算文本命中 |
| `created_at` / `updated_at` | integer | 否 | `0` | 时间戳（由任务文件提供，便于回放）；`updated_at` 参与召回排序 |

`script` 的每个元素是一条 `core::ChatResponse`：

| 字段 | 类型 | 默认 | 说明 |
|---|---|---|---|
| `ok` | bool | `true` | 模型调用是否成功；`false` 时按 `error_code` 失败 |
| `content` | string | `""` | 回答正文 |
| `tool_calls` | array | `[]` | `[{"id": string, "name": string, "arguments": object 或 JSON 字符串}]` |
| `finish_reason` | string | `"stop"` | `length`/`content_filter`/`insufficient_system_resource` 触发 `IncompleteResponse` |
| `usage` | object | 全 0 | `{prompt_tokens, completion_tokens, total_tokens}`，累计为 `tokens` 指标 |
| `error` | string | `""` | 失败文本 |
| `error_code` | string | `"None"` | `None` / `Cancelled` / `DeadlineExceeded` / `ModelFailure` / `ToolFailure` |

脚本耗尽后仍未结束的任务会收到 `ModelFailure`（"mock script exhausted"）。

`expect` 字段（`status` / `error_code` 之外均可缺省）：

| 字段 | 类型 | 说明 |
|---|---|---|
| `status` | string | 期望 `RunStatus`：`Completed` / `Cancelled` / `Failed` / `BudgetExceeded` / `Busy` |
| `error_code` | string | 期望 `RunErrorCode`（`ToString(RunErrorCode)` 全量取值，如 `None`、`InvalidToolCall`、`ModelCallLimit`） |
| `min_steps` | int | `steps` 下界 |
| `max_steps` | int | `steps` 上界 |
| `tools` | array[string] | 实际进入 `ToolDispatcher` 的工具名序列（包含被拒绝的调用；非法工具名在 dispatch 前就被拦截，不会出现） |
| `tool_results_ok` | array[bool] | 与 `tools` 一一对应的工具结果 `ok` 标志 |
| `content_contains` | string | 最终 `content` 必须包含的子串（失败任务通常没有 content） |
| `request_contains` | string | 整个任务运行期间，任一模型请求的任一 `message.content` 包含该子串即通过；用于验证 `<memory>` 召回真的注入了 prompt |

多轮语义（`follow_up_inputs` 非空）：`status` / `error_code` / `steps` / `tools` / `tool_results_ok` / `content_contains` 只校验**最后一轮** `Send` 的结果；`request_contains` 与 `model_calls` / `tokens` / `latency_ms` 覆盖**整个任务**（所有轮次）。某一轮不是 `Completed` 时停止后续轮次，最后一轮即该失败轮。

## fixture 工作区

默认工作区为 `<tasks 文件所在目录>/workspace`，可用 `--workspace <dir>` 覆盖。当前 fixture：

- `workspace/notes.md`：含 `NOTES_MARKER_42`
- `workspace/faq.txt`：含 `FAQ_MARKER_7`
- `workspace/guide.txt`：含 `GUIDE_MARKER_1` 与 `NEXT_FILE=notes.md`
- `workspace/missing.txt` **故意不存在**，用于缺失文件恢复用例

## 12 条任务覆盖矩阵

| # | id | kind | 覆盖点 |
|---|---|---|---|
| 1 | `single_read_notes` | single_read | 单次 `read_file` + 最终回答 |
| 2 | `sequential_double_read` | sequential_read | 两步顺序读两个文件 |
| 3 | `multi_step_guide_chain` | multi_step | 三步工具链（guide → notes → faq） |
| 4 | `recover_missing_file` | missing_file_recovery | 读缺失文件失败后回退成功（`tool_results_ok=[false,true]`） |
| 5 | `recover_unknown_tool` | unknown_tool_recovery | 未知工具被 `ToolDenied` 后恢复 |
| 6 | `fatal_invalid_tool_name` | invalid_tool_name | fatal `InvalidToolCall`（工具名非法，dispatch 前拦截） |
| 7 | `fatal_empty_response` | empty_response | fatal `EmptyResponse` |
| 8 | `fatal_incomplete_length` | incomplete_response | `finish_reason=length` → fatal `IncompleteResponse` |
| 9 | `budget_model_call_limit` | model_call_limit | `max_steps` 超限 → `BudgetExceeded`/`ModelCallLimit` |
| 10 | `budget_tool_call_limit` | tool_call_limit | `max_tool_calls` 超限 → `BudgetExceeded`/`ToolCallLimit` |
| 11 | `long_term_memory_injection` | memory | seed Fact 被召回并注入 prompt，最终回答含电话号码（`request_contains` + `content_contains`） |
| 12 | `long_term_memory_across_turns` | memory_followup | 同一 Agent 两轮：第二轮 follow-up 命中 Preference 召回，跨轮注入 `青绿色`（expectation 针对最后一轮） |

mock 模式下 12 条全部通过（`passed=12 total=12`）。

## 长期记忆评测

任务文件里的 `memory` 是**种子记忆**：`ExecuteTask` 为每个带种子的任务创建 `plugins::memory::InMemoryMemoryStore`，逐条 `Remember` 后挂到 `RunnerDependencies.memory`；`MakeTaskProfile` 在种子非空时设置 `profile.memory_policy.enable_recall = true`，且 `recall_scopes` 留空（不限制 scope）。任一 seed 写入失败按 `setup_error` 处理（该任务判失败，错误来自 `Remember`）。

召回注入链路（细节见 [`../docs/memory.md`](../docs/memory.md)）：每轮 `Send` 在第一次模型调用前按当前 user 输入召回，命中的记录渲染为 `[Kind] text`，包成一条 `<memory>…</memory>` System 消息插在最近一条 user 消息之前。例如种子 `{"kind": "Fact", "text": "用户的电话是 13800001111"}` 注入后形如：

```text
<memory>
[Fact] 用户的电话是 13800001111
</memory>
```

评测分两层：

- **`request_contains` 验证召回注入**：`RecordingChatModel` 装饰实际模型，记录每次 `core::ChatRequest` 拷贝；整个任务中途任一请求的任一 `message.content` 含期望子串即通过。mock 模式模型仍按 `script` 返回，所以 `content_contains` 本身不能证明模型用了记忆，`request_contains` 才能证明召回结果真的进入了 prompt。
- **`follow_up_inputs` 验证跨轮**：任务在同一 `Agent` 上连续 `Send`，每轮独立召回；第二轮输入命中 seed 的 tag 后，`青绿色` 被跨轮注入，最后一轮的回答与请求同时命中 `content_contains` / `request_contains`。

两条任务：

| id | 场景 | 关键点 |
|---|---|---|
| `long_term_memory_injection` | 单轮问电话 | seed Fact（tags `["phone"]`）；input 含 `phone` 命中 tag → 注入 `13800001111` |
| `long_term_memory_across_turns` | 两轮问颜色 | 首轮闲聊不召回；`follow_up_inputs[0]` 含 `color` 命中 tag → 第二轮注入 `青绿色`，expectation 只校验最后一轮 |

局限：mock 模式只验证「召回注入链路 + 判定逻辑」；真实模型是否真的使用记忆需 `--mode real` 观察。真实跨会话记忆（进程重启 / JSONL 持久化 / CLI `--memory`）不在本评测范围，属另一任务。

## `--report` JSON

`--report <path>` 写出聚合报告：顶层 `tool` / `mode` / `tasks_file` / `workspace` / `passed` / `total` / `pass_rate` / `threshold`，以及 `results[]`，每项含 `id` / `kind` / `description` / `passed` / `failure` / `status` / `error_code` / `run_error` / `steps` / `tool_attempts` / `tools` / `tool_results_ok` / `model_calls` / `tokens` / `request_contains` / `latency_ms`。其中 `request_contains` 是实测值：配置了 `expect.request_contains` 时为「整个任务是否有请求包含该子串」，未配置时恒为 `false`。写报告失败按输入/环境错误处理（退出码 2）。

## ctest 集成

`AGENT_CORE_BUILD_TESTS=ON`（默认）时，`source/eval/CMakeLists.txt` 注册测试：

```cmake
add_test(NAME agent_eval_mock
    COMMAND agent-eval --tasks ${CMAKE_SOURCE_DIR}/eval/tasks.json --mode mock --min-pass-rate 0.7
)
```

任务文件使用绝对路径，因此不依赖 ctest 的工作目录。
