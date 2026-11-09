# API 参考（公共头文件）

本文件只列**外部宿主需要直接使用的公共契约**；完整字段/注释以头文件为准。
命名空间：`agent::core` / `agent::runtime` / `agent::transport` / `agent::model` / `agent::plugins::*`。

## 1. 头文件索引

```
include/agent/core/
  message.h        ChatRole / ChatMessage / ToolCall
  tool.h           ToolSpec / ToolResult / Tool / IsValidToolName
  chat_model.h     ChatRequest / ChatResponse / ChatUsage / ChatModel
  call_context.h   CallContext / CallError（stop_token + deadline）
  context.h        AgentContextRequest / AgentContextResult / ContextProvider
  memory.h         MemoryKind / MemoryScope / MemoryRecord / MemoryQuery / MemoryHit / MemoryStore
  summarizer.h     SummaryRequest / SummaryResult / Summarizer（中期压缩）
  conversation.h   Conversation（短期历史）
  agent_system.h   AgentSystem / TurnEvent（观察者契约）
  system_registry.h SystemRegistry（观察者注册）
  tracer.h         Tracer（AgentSystem 的空基类）
  version.h        AGENT_CORE_VERSION / AGENT_CORE_ABI_VERSION / Version() / AbiVersion() / BuildBanner()
  stats_view.h     StatsView
  utf8.h           Utf8PrefixSize（UTF-8 安全截断）

include/agent/runtime/
  agent.h              AgentDescriptor / AgentId / AgentScope / AgentDependencies / Agent
  agent_profile.h      AgentProfile / RunOptions / MemoryPolicy
  memory_policy.h      MemoryPolicy（召回开关/上限/作用域）
  agent_session.h      AgentSession（Send / ClearHistory / Records）
  agent_run.h          RunOutcome / ToolExecutionRecord / TurnRecord / AgentRunResult
  agent_runner.h       RunnerDependencies / AgentRunner
  agent_executor.h     AgentExecutor（可替换执行策略）
  run_context.h        RunContext / RunBudget / RunStatus / RunErrorCode
  turn_transaction.h   TurnWorkspace / TurnTransaction
  turn_executor.h      ExecuteTurn（Session 与测试共用）
  tool_registry.h      ToolRegistry（注册/SpecsFor/Execute/Descriptor）
  tool_descriptor.h    ToolDescriptor / ToolAccessScope / ToolApprovalMode
  tool_policy.h        ToolPolicy / AllowAllToolPolicy / AllowListToolPolicy
  context_policy.h     ContextPolicy / KeepRecentTurnsPolicy
  tool_output_policy.h ToolOutputPolicy / TruncateToolOutputPolicy
  tool_dispatcher.h    ToolScheduler / InlineToolScheduler / ToolDispatcher
  composition.h        ValidateComposition
  service_registry.h   scoped ServiceRegistry（typeid）
  system_notifier.h    SystemNotifier（runtime 内部）
  execution_id.h       ExecutionId（NewSession / NewRun）

include/agent/transport/http_transport.h   HttpRequest / HttpResponse / HttpTransport
include/agent/model/openai_chat_model.h    OpenAiConfig / OpenAiChatModel
include/agent/plugins/traces/*             FileTracer / TraceSink / TraceUsage / TraceViewer
include/agent/plugins/memory/*             NoopMemoryStore / InMemoryMemoryStore / RuleBasedSummarizer
include/agent/plugins/metrics/*            MetricsCollector / CostEstimator / MetricsSnapshot
include/agent/plugins/tools/*              ReadFileTool / WorkspacePathPolicy
include/agent/hosts/reference/reference_host.h  参考 Host Adapter（非生产）
```

## 2. 关键类型与契约

### 2.1 Agent / Session / Runner

- `Agent` = 身份 + 初始 `AgentProfile` + 独立 `AgentSession` + `AgentExecutor` + AgentScope。
- `Agent::Send(input, RunOptions)`：**同步阻塞**，内部 ReAct 循环；可被 `stop_token`/deadline 取消。
  `RunOptions{stop_token, timeout, run_id}`；`run_id` 为空时 runtime 生成。
- `AgentSession`：同 Session 同时只允许一个 `Send`（Busy），不同 Session 可并行；
  `ClearHistory` / `SetProfile` / `Reconfigure` 在运行中返回 false。
- `AgentRunner`：默认执行器；构造时快照 `ToolRegistry`；依赖见 `RunnerDependencies`。
- 线程契约：**一个 Agent 只在一个 worker 线程上串行 Send**；core 对象不要跨线程共享
  （共享的 model/transport 必须线程安全或 per-Agent 实例）。

### 2.2 RunOutcome / 状态与错误

`RunStatus`：`Completed` / `Cancelled` / `Failed` / `BudgetExceeded` / `Busy`。

`RunErrorCode`：`None` / `Cancelled` / `DeadlineExceeded` / `InvalidInput` / `InvalidConfiguration` / `Busy` /
`ModelCallLimit` / `ToolCallLimit` / `ToolOutputLimit` / `ModelFailure` / `IncompleteResponse` /
`EmptyResponse` / `ToolDenied` / `ToolFailure` / `InvalidToolCall` / `ContextFailure` / `InternalError`。

历史提交规则（`TurnTransaction`）：

| 结束原因 | 对话历史 | 工具记录 |
|---|---|---|
| Completed | 提交完整本轮 | 保留 |
| Cancelled / Deadline / InternalError / InvalidConfiguration / ContextFailure | 回滚到本轮开始 | 保留已登记操作 |
| ModelFailure / IncompleteResponse / BudgetExceeded | 保留用户输入与完整工具批次 | 保留 |
| 预检失败 / Busy | 不开始本轮 | 无 |

### 2.3 Tool

```cpp
class Tool
{
    virtual std::string Name() const = 0;
    virtual std::string Description() const = 0;
    virtual std::string ParametersJson() const = 0;   // JSON Schema 字符串，core 不解析
    virtual ToolResult Execute(const std::string& arguments_json, const CallContext& context = {}) = 0;
};
```

- 注册：`ToolRegistry::Register(tool, ToolDescriptor, error)`；名字需满足 `IsValidToolName`。
- 治理：`ToolDescriptor.side_effects/access_scope` 需要 `ToolPolicy::AllowsSideEffects()` 显式授权；
  `approval != NotRequired` 暂不可执行。组合校验在每次 `Send` 前执行。
- 执行：`ToolDispatcher` 负责 allowlist、policy、预算、call id 去重/重放、输出截断、错误映射；
  具体调度由宿主 `ToolScheduler` 决定（默认 `InlineToolScheduler` 直接在当前线程执行）。

### 2.4 ContextProvider

见 [context-provider.md](context-provider.md)。挂载：`RunnerDependencies.context_provider`。

### 2.5 观察者

```cpp
class AgentSystem
{
    virtual void OnTurnStarted(const TurnEvent&) {}
    virtual void OnModelRequest(const ChatRequest&) {}
    virtual void OnModelResponse(const ChatResponse&) {}
    virtual void OnToolCall(const ToolCall&) {}
    virtual void OnToolResult(const ToolCall&, const ToolResult&) {}
    virtual void OnTurnEnded(const TurnEvent&) {}
    // 旧接口 OnTurnStart/OnTurnEnd 保留为兼容桥
};
```

- 通过 `SystemRegistry::Add(std::shared_ptr<AgentSystem>)` 注册；
- 同一执行线程内同步通知；遍历前快照；观察者异常被 runtime 隔离，不影响 RunStatus；
- `TurnFinished` 在 Session commit/rollback 之后发布。

### 2.6 ServiceRegistry

- scoped：HostScope → AgentScope → SessionScope，子 scope 可 fallback 查父 scope；
- 按 `std::type_index(typeid(T))` 存服务（**宿主必须开启 RTTI**）；
- 不用于 core 内部 service locator；Host 组合根显式注入依赖。

## 3. 典型组合根（宿主）

```cpp
// 1. Host 负责的依赖
auto transport = std::make_shared<MyHttpTransport>();           // UE: FHttpModule
auto model = std::make_shared<agent::model::OpenAiChatModel>(transport, config);

// 2. Agent 级依赖
agent::runtime::RunnerDependencies deps;
deps.model = model;
deps.tools = tool_registry;                                     // 注册好工具的描述符
deps.tool_policy = std::make_shared<agent::runtime::AllowListToolPolicy>(
    std::vector<std::string>{"query_world", "move_villager"}, true);
deps.context_policy = std::make_shared<agent::runtime::KeepRecentTurnsPolicy>();
deps.tool_output_policy = std::make_shared<agent::runtime::TruncateToolOutputPolicy>();
deps.tool_scheduler = std::make_shared<MyGameThreadToolScheduler>();   // UE: GameThread 编组
deps.systems = std::make_shared<agent::core::SystemRegistry>();         // 加 tracer/metrics
deps.context_provider = world_provider;                                 // 可选

// 3. Agent
agent::runtime::AgentDependencies agent_deps;
agent_deps.executor = std::make_shared<agent::runtime::AgentRunner>(deps);
agent::runtime::Agent agent(descriptor, profile, agent_deps, session_id);

// 4. 在 worker 线程同步调用；取消用 stop_source
agent::runtime::RunOptions options;
options.stop_token = stop_source.get_token();
options.run_id = "host-run-1";
const agent::runtime::AgentRunResult result = agent.Send("hello", options);
```

## 4. HTTP 传输契约（宿主实现）

```cpp
struct HttpRequest
{
    std::string url;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    int timeout_ms = 30000;
    std::size_t max_response_bytes = 8 * 1024 * 1024;
    std::stop_token stop_token;
};

struct HttpResponse
{
    int status = 0;
    std::string body;
    std::string error;   // 传输层失败（连接/超时/TLS/取消）
    bool Ok() const;
};

class HttpTransport { virtual HttpResponse PostJson(const HttpRequest& request) = 0; };
```

- 同步接口；超时/取消返回非空 `error`；
- 宿主不得自动跟随跨域重定向（避免 Authorization 泄露）；
- 响应体超过 `max_response_bytes` 应返回错误。

## 5. 版本与 ABI

- `AGENT_CORE_VERSION` / `AGENT_CORE_ABI_VERSION`：编译期默认值（头文件）；
- `Version()` / `AbiVersion()` / `BuildBanner()`：库内实际值；
- 宿主启动时比对 ABI，不一致拒绝启用并给出明确错误；升级流程见 [build-delivery.md](build-delivery.md)。
