# Memory（长期记忆）契约

`Memory` 是 agent 的跨轮次/跨会话记忆层，补足 `Conversation`（最近逐字对话）与 `ContextProvider`
（当前世界状态）。默认关闭：未配置 `MemoryStore`/`Summarizer` 或未启用 `MemoryPolicy` 时，行为与旧版本完全一致。

## 1. 分层

| 层 | 组件 | 状态 |
|---|---|---|
| 短期 | `Conversation` + `ContextPolicy` | 已有 |
| 中期 | `Summarizer`（压缩旧轮次） | 接口 + 规则/模型实现已提供；Session 压缩已接入（Phase 2） |
| 长期 | `MemoryStore`（结构化记忆 + 检索） | 接口 + InMemory/Noop/Jsonl 实现已提供（SQLite/UE SaveGame 由 Host） |

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
- CLI：`user:<name>` 或 `session:<id>`；CLI 的 `--memory` 默认使用共享作用域 `cli:default`；
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

## 5. CLI 持久化（JSONL）

`agent::plugins::memory::JsonlMemoryStore` 是随包提供的追加式 JSONL 实现，供 CLI（以及需要简单文件持久化的 Host）使用：

- 打开：`JsonlMemoryStore::Open(path, error)`；父目录不存在时自动创建，文件不存在时创建，然后逐行重放；
- 追加式操作日志（每行一个 JSON）：`{"op":"remember","record":{...}}` / `{"op":"forget","id":"..."}` /
  `{"op":"clear_scope","scope":{...}}`；
- 写入失败返回 `false` 并写 `error`，内存状态不变；重放遇到**带换行**的非法行时 `Open` 失败并报告行号
  （撕裂尾行的例外见下）；
- `Records()` / `Size()` / `Path()` 供 Host 展示；召回语义与 `InMemoryMemoryStore` 完全一致。
- **撕裂尾行容错**：`Open` 读取时，若文件最后一行没有换行结尾（进程崩溃/断电时只写了一半）：
  - 该行是完整 JSON：正常重放，并在文件尾补写一个换行，保证后续追加不会与它粘成一行；
  - 该行解析失败（被截断）：视为撕裂行忽略（不报错），并用一次压缩重写文件清掉残留；
  - 其它**带换行**的非法行仍然报错：`failed to parse memory jsonl at line <N>: <detail>`（行号从 1 开始）。
- **日志压缩**：`bool Compact(std::string& error)` 把当前记录重写成 remember 行（写 `path + ".tmp"` →
  flush/close → 替换原文件（Windows 下先 remove 再 rename）→ 重新以 append 打开），锁内执行；
  成功后操作计数清零；失败返回 `false` 并写 `error`，内存状态不变（原文件不变，或保留 `.tmp` 快照）。
- **自动压缩**：`Remember` / `Forget` / `ClearScope` 成功后累计操作数，当
  `ops >= 128 && ops >= 2 * records + 16` 时自动触发一次 `Compact`，避免日志相对记录数无限膨胀。
  自动压缩失败会被忽略（数据已落盘，下次写操作再试），不影响本次操作的返回值；手动 `Compact` 失败则返回错误。
- JSONL 仍然是**追加日志**：单次更新/删除只追加新行，长期运行的体积由自动/手动 `Compact` 控制；
  需要固定体积或人工排障时可在 Host 侧显式调用 `Compact`。

CLI 接入：

```text
agent-cli --memory .\memory.jsonl       # 启动参数；也可在配置里写 "memory_path"
> /memory                               # 路径 + 记录数
> /memory list                          # 列出 cli:default 作用域的记忆
> /memory add 张三的电话是 13800001111   # 写入一条 Fact（自动生成 id）
> /memory clear                         # 清空 cli:default 作用域
```

- 开启后 `AgentFactory` 打开 `enable_recall`，召回作用域固定为 `cli:default`，因此新会话会加载同一 JSONL 并自动召回；
- 打开失败（路径不可用/文件损坏）会拒绝启动，避免静默丢记忆；单次写入失败由 `/memory add` 输出错误；
- `Compact` 由 Store 内置（自动 + 手动），Host 无需自行重写文件；`/memory` 相关命令只改追加日志。

## 6. 内置实现

- `agent::plugins::memory::NoopMemoryStore`：空实现（写入静默成功、召回为空）。
- `agent::plugins::memory::InMemoryMemoryStore`：进程内确定性实现，用于测试/CLI 演示/UE 早期接入。
  召回规则：文本或 tag 命中优先，其次 `importance`、`updated_at`、`id`；支持 tags（AND）、kinds、scopes 过滤与
  `limit` / `max_chars` 预算。中文没有分词：query 包含某个 tag 也算命中，Host 应写入有意义的 tags。
- `agent::plugins::memory::JsonlMemoryStore`：JSONL 追加日志实现，重启后重放恢复，自带撕裂尾行修复与
  自动/手动 `Compact`（见上节）。
- `agent::plugins::memory::RuleBasedSummarizer`：保留最近 N 条非空非 tool 消息的规则摘要，确定性、零模型调用。
- `agent::plugins::memory::ModelSummarizer`：用 `ChatModel` 生成摘要，可配 fallback。

## 7. Host 接入示例（伪代码）

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

## 8. 测试

`tests/memory_test.cpp`：存取/更新、scope/tag/文本召回、预算截断、forget/clear、Noop 行为、
规则摘要（保留最近、排除 tool、预算）、Runner 注入位置与预算、关闭记忆时零注入、召回异常降级。

`tests/jsonl_memory_store_test.cpp`：JSONL 打开/建目录、重开往返、更新、forget、clear_scope、
排序/预算、非法行失败、撕裂尾行修复、完整尾行补换行、带换行非法行报错、手动/自动压缩。

`tests/session_memory_test.cpp`：Session 压缩（保留最近 K 轮、摘要消息替换旧轮次）、压缩失败/关闭时保留历史、
`ModelSummarizer` 的请求参数（temperature 0 / max_tokens）、截断与 fallback。

`eval/tasks.json` 的 `long_term_memory_injection` / `long_term_memory_across_turns`：
seed 记忆后验证召回注入到模型请求（`request_contains`）与跨轮回答。

## 9. 后续（F4）

- UE 持久化：SQLite / `USaveGame` 版 `MemoryStore`；
- `memory_search`/`memory_write` 可选工具、异步/后台压缩、`scope=world` 共享与传播；
- 记忆写入的隐私/保留/删除策略，向量/embedding 检索。
