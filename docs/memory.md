# Memory（长期记忆）契约

`Memory` 是 agent 的跨轮次/跨会话记忆层，补足 `Conversation`（最近逐字对话）与 `ContextProvider`
（当前世界状态）。默认关闭：未配置 `MemoryStore`/`Summarizer` 或未启用 `MemoryPolicy` 时，行为与旧版本完全一致。

## 1. 分层

| 层 | 组件 | 状态 |
|---|---|---|
| 短期 | `Conversation` + `ContextPolicy` | 已有 |
| 中期 | `Summarizer`（压缩旧轮次） | 接口 + 规则/模型实现已提供；Session 压缩已接入（Phase 2） |
| 长期 | `MemoryStore`（结构化记忆 + 检索） | 接口 + InMemory/Noop 实现已提供（持久化由 Host） |

## 2. 契约

- [`core/memory.h`](../source/core/include/agent/core/memory.h)：`MemoryKind` / `MemoryScope` /
  `MemoryRecord` / `MemoryQuery` / `MemoryHit` / `MemoryStore`。
- [`core/summarizer.h`](../source/core/include/agent/core/summarizer.h)：`SummaryRequest` /
  `SummaryResult` / `Summarizer`；`kSummaryMessagePrefix` 是压缩摘要 System 消息的固定前缀。
- [`runtime/memory_policy.h`](../source/runtime/include/agent/runtime/memory_policy.h)：`MemoryPolicy`。
- `RunnerDependencies.memory` / `.summarizer`：可选的存储与摘要实现；`AgentProfile.memory_policy`：
  召回与压缩策略。

`payload_json` 是给 Host 做精确查询/更新的结构化数据，core 不解析（与 `ToolSpec.parameters_json` 一致）。

### 作用域

`MemoryScope{kind, id}` 的 `kind` 约定：`session` / `agent` / `user` / `world`。

- UE NPC：`agent:<npc_id>`（自己记得的事）、`user:<player_id>`（玩家档案）、`world:<world_id>`（共享记忆）；
- CLI：`user:<name>` 或 `session:<id>`；
- 召回时 `MemoryPolicy.recall_scopes` 通常填当前 agent + world（共享），空表示不限制。

## 3. 召回注入流程（Phase 1）

每轮 `Send` 开始、模型调用前：

1. 按 `ContextPolicy` 生成基础 messages 并估算字符数；
2. 若 `memory_policy.enable_recall` 且 `RunnerDependencies.memory` 非空：
   - 用 `MemoryQuery{text=user_input, scopes=recall_scopes, limit, max_chars}` 召回；
   - 结果按 `[Kind] text` 渲染，包成一条 `<memory>…</memory>` System 消息，
     插在最近一条 `User`（当前输入）之前；
   - 预算：先受 `profile.max_context_chars` 剩余预算约束，再受 `recall_budget_chars` 上限；
     超出按 UTF-8 边界截断。
3. ContextProvider 的 `<context>` 在 `<memory>` 之后注入（先事实记忆，后世界状态）。
4. 同一轮的多次模型调用（工具循环）复用同一份记忆，不重复召回。

**失败语义**：Memory 是增强项——`Recall` 抛异常或返回空时静默降级，不影响本轮；
与 ContextProvider 的 fail-closed 不同。写入失败由 Host 记录日志。

## 4. 会话压缩（Phase 2）

历史超过阈值时，Session 在每轮结束（提交/回滚之后）把最旧的轮次压成一条摘要 System 消息，
保留最近 K 个 user turn 原文；下一轮发送时摘要随前导 System 消息一起保留。

`MemoryPolicy` 压缩字段：

| 字段 | 说明 |
|---|---|
| `enable_compaction` | 是否启用（默认 false） |
| `compact_trigger_chars` | 存储历史字符数达到阈值时触发；0 = 不触发 |
| `compact_keep_recent_turns` | 保留最近 N 个 user turn 原文（0 按 1 处理） |
| `compact_summary_max_chars` | 摘要输出字符上限（默认 2000） |

行为细节：

- 摘要实现来自 `RunnerDependencies.summarizer`（`AgentRunner::Summarizer()` 暴露给 Session）；
- `RuleBasedSummarizer`：离线确定、零模型调用，可作默认/降级；
- `ModelSummarizer`：复用 `ChatModel`（可单独配便宜模型、temperature 0、`max_tokens` 封顶），
  可挂 `fallback`（如 `RuleBasedSummarizer`）；摘要调用使用 `turn_timeout_ms` 作为截止时间；
- 压缩失败 / 摘要为空：**保留原历史**，不影响本轮；
- 每次压缩会移除上一条摘要并生成新摘要（不累积重复摘要），`kSummaryMessagePrefix` 用于识别旧摘要；
- 压缩只改 `Conversation`，不改变 TurnRecord/工具执行记录。

## 5. 内置实现

- `agent::plugins::memory::NoopMemoryStore`：空实现（写入静默成功、召回为空）。
- `agent::plugins::memory::InMemoryMemoryStore`：进程内确定性实现，用于测试/CLI 演示/UE 早期接入。
  召回规则：文本或 tag 命中优先，其次 `importance`、`updated_at`、`id`；支持 tags（AND）、kinds、scopes 过滤与
  `limit` / `max_chars` 预算。中文没有分词：query 包含某个 tag 也算命中，Host 应写入有意义的 tags。
- `agent::plugins::memory::RuleBasedSummarizer`：保留最近 N 条非空非 tool 消息的规则摘要，确定性、零模型调用。
- `agent::plugins::memory::ModelSummarizer`：用 `ChatModel` 生成摘要，可配 fallback。

## 6. Host 接入示例（伪代码）

```cpp
auto store = std::make_shared<MySqliteMemoryStore>("/path/to/agent-memory.db");
auto summarizer = std::make_shared<agent::plugins::memory::ModelSummarizer>(
    cheap_model, agent::plugins::memory::ModelSummarizer::Config{}, fallback_summarizer);
dependencies.memory = store;
dependencies.summarizer = summarizer;

runtime::AgentProfile profile;
profile.memory_policy.enable_recall = true;
profile.memory_policy.recall_scopes = {
    {"agent", "npc-01"},
    {"world", "village-01"},
};
profile.memory_policy.enable_compaction = true;
profile.memory_policy.compact_trigger_chars = 24000;
profile.memory_policy.compact_keep_recent_turns = 6;

// 事件发生时由游戏逻辑显式写（推荐）：
core::MemoryRecord record;
record.id = "npc-01:phone:zhangsan";
record.kind = core::MemoryKind::Fact;
record.text = "张三的电话是 13800001111";
record.payload_json = R"({"subject":"张三","key":"phone","value":"13800001111"})";
record.tags = {"张三", "电话"};
record.scope = {"agent", "npc-01"};
record.importance = 0.8;
store->Remember(record, error);
```

## 7. 测试

`tests/memory_test.cpp`：存取/更新、scope/tag/文本召回、预算截断、forget/clear、Noop 行为、
规则摘要（保留最近、排除 tool、预算）、Runner 注入位置与预算、关闭记忆时零注入、召回异常降级。

`tests/session_memory_test.cpp`：Session 压缩（保留最近 K 轮、摘要消息替换旧轮次）、压缩失败/关闭时保留历史、
`ModelSummarizer` 的请求参数（temperature 0 / max_tokens）、截断与 fallback。

## 8. 后续（Phase 2 剩余 / F4）

- CLI 持久化：JSONL/SQLite `MemoryStore` + `--memory` 开关（跨 CLI 会话演示）；
- eval 增加"长期记忆"任务（跨轮/跨会话记忆问答），对应米哈游 JD 的"长期记忆效果"评测维度；
- `memory_search`/`memory_write` 可选工具、异步/后台压缩、`scope=world` 共享与传播；
- 记忆写入的隐私/保留/删除策略，向量/embedding 检索。
