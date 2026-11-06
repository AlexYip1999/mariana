# ContextProvider 契约（F1）

`ContextProvider` 是 Host 向 Agent 每轮注入「世界状态 / 检索结果 / 记忆摘要」的唯一入口。
core 只定义接口与注入位置，不解析内容、不依赖 JSON 库。

## 1. 接口

```cpp
struct AgentContextRequest
{
    // 关联
    std::string application;   // 预留；Runner 当前不填
    std::string session_id;
    std::string run_id;

    // 本轮输入
    std::string user_input;

    // 本轮可注入字符上限；0 = 不限（仅当 profile.max_context_chars = 0 时出现）
    std::size_t max_context_chars = 0;

    // 取消 / deadline：查询世界或检索时应协作检查
    CallContext call;
};

struct AgentContextResult
{
    bool ok = true;
    std::string context_json;  // core 不解析
    std::string error;         // ok=false 时的原因
};

class ContextProvider
{
  public:
    virtual AgentContextResult BuildContext(const AgentContextRequest& request) = 0;
};
```

挂载位置：`runtime::RunnerDependencies::context_provider`（`std::shared_ptr<core::ContextProvider>`）。
为空表示不注入，行为与旧版本一致。

## 2. 调用时机与注入位置

- **每轮只调用一次**（`Agent::Send` 开始、模型调用之前）；同一轮内多次模型调用（ReAct 工具循环）
  复用同一份上下文，避免每步重复查询世界。
- 注入形式：一条 `role = System` 的消息，内容为

  ```text
  <context>
  { ...context_json... }
  </context>
  ```

- 插入位置：**最近一条 `User` 消息（当前轮输入）之前**；没有 User 消息时追加到末尾。
  这样多轮历史保持原顺序，当前轮的上下文紧邻当前输入。

## 3. 预算（`max_context_chars`）

- `AgentProfile::max_context_chars == 0`：不限。请求 `max_context_chars = 0`，Provider 应返回完整内容。
- `> 0`：Runner 先按 `ContextPolicy` 生成请求消息并统计字符数，剩余预算减去 `<context>` 包装开销后
  作为 `request.max_context_chars` 传给 Provider；Provider 返回内容超长时按 UTF-8 边界截断。
- 没有剩余预算时：**跳过 Provider 调用**（本轮不注入），不会越过预算。
- 预算口径与 `ContextPolicy` 一致：消息 content/`tool_call_id`/`tool_calls` 的字节数近似。

## 4. 失败语义（fail-closed）

- `ok = false`、或 Provider 抛异常、或返回内容异常：本轮直接失败，
  `RunStatus::Failed` + `RunErrorCode::ContextFailure`，错误信息前缀 `context provider failed:`。
- 失败按「配置/内部错误」处理，**回滚本轮历史**（不写用户输入与工具批次），
  已发生的工具副作用记录保留在 `tool_executions`（与取消语义一致）。
- 不允许静默降级为「没有上下文」：Host 若想降级，应由 Provider 自己返回空内容 + `ok=true`。

## 5. 线程与异常

- `BuildContext` 在调用 `Agent::Send` 的线程（通常是 Host worker）执行，**不得访问引擎主线程对象**。
  UE 实现必须把世界查询投递到 GameThread 并等待（见 [hosts-unreal.md](hosts-unreal.md)）。
- 可以阻塞，但应遵守 `request.call` 的 stop_token 与 deadline；Runner 会在调用前后检查中断。
- 实现不得抛异常穿越（Runner 会捕获并转成 `ContextFailure`，但宿主应自己记录日志）。

## 6. UE 世界快照示例（伪代码）

```cpp
agent::core::AgentContextResult UeWorldProvider::BuildContext(const agent::core::AgentContextRequest& request)
{
    // 1. worker -> GameThread 取快照（与 FMarianaHttpTransport 同一套 Promise 模式）
    FWorldSnapshot snapshot = GameThreadBridge::RunOnGameThreadAndWait([this] { return CaptureSnapshot(); });
    // 2. 序列化为 JSON（不含密钥/不可见信息）
    agent::core::AgentContextResult result;
    result.context_json = SerializeSnapshot(snapshot);
    return result;
}
```

快照建议字段：时间、自己（位置/职业/需求/当前动作）、资源账本、感知范围内对象、最近事件、可执行动作列表。
**只放模型决策需要的信息**；不要放密钥、全图隐藏信息或可被 prompt injection 利用的文本。

## 7. 后续演进（未做）

- 多 Provider 的 `ContextPipeline`（按顺序注入、失败 fail-closed）——当前单 Provider 已满足 F1 MVP；
- Memory 检索结果与摘要在 pipeline 中的顺序；
- token 级预算（当前按字符近似，无 tokenizer 依赖）。
