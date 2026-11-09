# Memory（长期记忆）契约

`Memory` 是 agent 的跨轮次/跨会话记忆层，补足 `Conversation`（最近逐字对话）与 `ContextProvider`
（当前世界状态）。默认关闭：未配置 `MemoryStore` 或未启用 `MemoryPolicy` 时，行为与旧版本完全一致。

## 1. 分层

| 层 | 组件 | 状态 |
|---|---|---|
| 短期 | `Conversation` + `ContextPolicy` | 已有 |
| 中期 | `Summarizer`（压缩旧轮次） | 接口 + 规则实现已提供（会话压缩在 Phase 2 接入） |
| 长期 | `MemoryStore`（结构化记忆 + 检索） | 接口 + InMemory/Noop 实现已提供（持久化由 Host） |

## 2. 契约

- [`core/memory.h`](../source/core/include/agent/core/memory.h)：`MemoryKind` / `MemoryScope` /
  `MemoryRecord` / `MemoryQuery` / `MemoryHit` / `MemoryStore`。
- [`core/summarizer.h`](../source/core/include/agent/core/summarizer.h)：`SummaryRequest` /
  `SummaryResult` / `Summarizer`。
- [`runtime/memory_policy.h`](../source/runtime/include/agent/runtime/memory_policy.h)：`MemoryPolicy`。
- `RunnerDependencies.memory`：可选的 `MemoryStore`；`AgentProfile.memory_policy`：召回策略。

`payload_json` 是给 Host 做精确查询/更新的结构化数据，core 不解析（与 `ToolSpec.parameters_json` 一致）。

### 作用域

`MemoryScope{kind, id}` 的 `kind` 约定：`session` / `agent` / `user` / `world`。

- UE NPC：`agent:<npc_id>`（自己记得的事）、`user:<player_id>`（玩家档案）、`world:<world_id>`（共享记忆）；
- CLI：`user:<name>` 或 `session:<id>`；
- 召回时 `MemoryPolicy.recall_scopes` 通常填当前 agent + world（共享），空表示不限制。

## 3. Runner 注入流程（Phase 1）

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

## 4. 内置实现

- `agent::plugins::memory::NoopMemoryStore`：空实现（写入静默成功、召回为空）。
- `agent::plugins::memory::InMemoryMemoryStore`：进程内确定性实现，用于测试/CLI 演示/UE 早期接入。
  召回规则：文本或 tag 命中优先，其次 `importance`、`updated_at`、`id`；支持 tags（AND）、kinds、scopes 过滤与
  `limit` / `max_chars` 预算。中文没有分词：query 包含某个 tag 也算命中，Host 应写入有意义的 tags。
- `agent::plugins::memory::RuleBasedSummarizer`：保留最近 N 条非空非 tool 消息的规则摘要，确定性、零模型调用。

## 5. Host 接入示例（伪代码）

```cpp
auto store = std::make_shared<MySqliteMemoryStore>("/path/to/agent-memory.db");
dependencies.memory = store;

runtime::AgentProfile profile;
profile.memory_policy.enable_recall = true;
profile.memory_policy.recall_scopes = {
    {"agent", "npc-01"},
    {"world", "village-01"},
};

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

## 6. 测试

`tests/memory_test.cpp` 覆盖：存取/更新、scope/tag/文本召回、预算截断、forget/clear、Noop 行为、
规则摘要（保留最近、排除 tool、预算）、Runner 注入位置与预算、关闭记忆时零注入、召回异常降级。

## 7. 后续（Phase 2 / F4）

- 会话压缩接入 Session（超阈值 → `Summarizer` → 摘要消息替换旧轮次）；
- `ModelSummarizer`（独立便宜模型、异步/后台触发）、`memory_write`/`memory_search` 可选工具；
- SQLite / UE SaveGame 持久化、向量/embedding 检索、`scope=world` 共享与传播；
- 记忆写入的隐私/保留/删除策略，以及 eval 的"长期记忆效果"任务。
