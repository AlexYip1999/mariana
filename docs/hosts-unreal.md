# UE 宿主接入指南（F2 / B1）

本文是 agent-core 嵌入 UE 插件（Runtime 模块）的交接文档：讲清楚**我们提供什么、UE 侧要写什么、
线程/生命周期/安全约束、以及最小落地步骤**。UE 侧代码由宿主工程实现（`Plugins/MarianaAgent/`），
本仓库只提供 core/runtime/库/接口与参考 Host Adapter。

参考实现：`source/hosts/reference/`（纯 C++：worker + `stop_source` + RunHandle/generation +
主线程 ToolScheduler + 迟到结果抑制）。UE 侧把其中的「主线程队列」换成 GameThread、把「HTTP transport」
换成 `FHttpModule`，结构可以直接照搬。

## 1. 交付形态（二选一）

| 方式 | 做法 | 适用 |
|---|---|---|
| **静态库**（推荐 F2） | `cmake --install build/vs2022-ue --config Release --prefix ...`，把 `include/` + `lib/*.lib` 拷进 `Plugins/<Plugin>/ThirdParty/mariana/` | 库稳定后；注意 CRT/工具链（见 [build-delivery.md](build-delivery.md)） |
| **源码集合**（推荐起步） | 把 `source/core|runtime|integrations/model|plugins/{metrics,tools,traces}` 的 `include/src` 加进插件编译 | 早期迭代；完全避开 CRT/ABI/工具链矩阵 |

无论哪种：**不要在 UE 里再写一套 Runner**；UE 只是 Host（组合根 + 线程 + 工具编组 + UI）。
**不要链接 `agent_transport`（WinHTTP）**：UE 自己实现 `HttpTransport`。**不要链 `agent_cli_lib`**。

## 2. Build.cs 关键配置

```csharp
PublicDependencyModuleNames.AddRange(new[] {
    "Core", "CoreUObject", "Engine", "Json", "JsonUtilities", "HTTP", "Projects", "DeveloperSettings"
});

bUseRTTI = true;              // ServiceRegistry 使用 typeid
bEnableExceptions = true;     // core 内部 try/catch + 静态库 ABI 一致性
CppStandard = CppStandardVersion.Cpp20;

PublicDefinitions.Add("AGENT_CORE_VERSION=\"0.1.0\"");   // 与库的实际版本一致
PublicDefinitions.Add("AGENT_CORE_ABI_VERSION=1");        // 与库的 ABI 一致
PublicIncludePaths.Add(Path.Combine(PluginDirectory, "ThirdParty", "mariana", "include"));
PublicAdditionalLibraries.Add(... "agent_core.lib");      // runtime/model/metrics/tools/traces
```

- **CRT 映射**：UE Debug Target → `Debug` 库（`/MDd`）；Development/Shipping → `Release` 库（`/MD`）。
- **工具链**：与库统一（`vs2022-ue` 预设：MSVC 14.38.33130 + WinSDK 10.0.22621）。
- **公共头文件不含 nlohmann**：不要为了 core 在 UE 模块里 include `nlohmann/json`；工具参数用
  `FJsonSerializer` / `TSharedPtr<FJsonObject>` 解析（与接入方案 §2.1 一致）。

## 3. UE 侧必须实现的 Host Adapter（清单）

| 适配器 | 接口 | 关键点 |
|---|---|---|
| `FMarianaHttpTransport` | `agent::transport::HttpTransport::PostJson` | worker 里构造请求 → 投递 GameThread `FHttpModule::CreateRequest` → `TPromise<HttpResponse>`；映射超时/TLS/HTTP 错误；PIE 停止 `CancelRequest` |
| `FMarianaToolScheduler` | `agent::runtime::ToolScheduler::Execute` | worker 调工具 → `AsyncTask(ENamedThreads::GameThread)` 执行 UObject 操作 → Promise 回结果；**GameThread 不能反过来等 worker**；等待要可超时/可取消 |
| `UMarianaSubsystem` | `UGameInstanceSubsystem` | 组装 HostScope/AgentFactory/每个 Agent 的 Runner；`SendAsync`/`Cancel`；PIE `Deinitialize` 清理 |
| `UMarianaAgentComponent` | 挂 `AVillagerCharacter` | 决策节拍（2–5s）、观测 → `SendAsync`、解析动作 → GameThread 执行、失败/超时回退本地行为 |
| `FMarianaTracer`（可选） | `core::AgentSystem` / `core::Tracer` | 转发 `UE_LOG(LogMariana)` + 内存 ring buffer；也可直接用 `plugins/traces` 的 `FileTracer` 写 `Saved/Logs/*.jsonl` |
| `FMarianaContextProvider` | `agent::core::ContextProvider` | 世界快照 JSON；**必须 gameThread 取数**（worker → GT → Promise）；失败返回 `ok=false` 即 fail-closed |
| 配置/密钥 | `UDeveloperSettings` + 环境变量 | endpoint/model/timeout 用设置；API key 只从环境变量/用户设置读，**不进 Content、不进 trace、不打包** |

### 3.1 SendAsync / RunHandle / 迟到抑制（core 不提供，Host 负责）

core 的 `Agent::Send` 是**同步可取消**的：`Send(input, RunOptions{stop_token, timeout, run_id})`。
Host 侧：

```cpp
RunHandle Host::SendAsync(AgentId id, std::string input, TurnCallback on_done)
{
    // 1. 忙则立即回 Busy（每个 Agent 串行）
    // 2. generation++，创建 std::stop_source
    // 3. worker 线程：result = agent->Send(input, {stop_source.get_token(), timeout, run_id});
    // 4. 完成后比较 generation，过期直接丢弃（迟到结果抑制）
    // 5. on_done 投递到 GameThread 队列，由主循环执行
}
bool Host::Cancel(RunHandle handle) { /* 校验 generation 后 request_stop() */ }
```

- `RunOptions.run_id` 可在 Send 前预分配（本仓库已支持），用于把 `RunHandle{run_id, session_id, generation}`
  在异步返回前交给调用方。
- 取消**不撤销副作用**：已开始的工具调用会保留 `ToolExecutionRecord`；`approval` 未落地的写工具保持禁用。
- 每个 NPC 一个 Agent，一个 worker 串行执行；不同 Agent 可并行。共享 model/transport 必须线程安全，
  或 per-Agent 实例。销毁顺序：`request_stop → drain GameThread 任务 → 等待 in-flight Send →
  close trace → erase`。

## 4. 工具治理（UE 动作工具必读）

`ToolDescriptor`：

```cpp
struct ToolDescriptor
{
    std::string name;
    std::string description;
    bool side_effects = false;
    ToolAccessScope access_scope = ToolAccessScope::Read;   // Read/Write/Network/Host
    ToolApprovalMode approval = ToolApprovalMode::NotRequired;
    std::string thread_affinity = "worker";                 // inline / worker / host
};
```

组合校验（`ValidateComposition`）规则：

- `side_effects = true` 或 `access_scope != Read`：**必须**由 Host 的 `ToolPolicy::AllowsSideEffects()`
  显式返回 true，否则注册后整轮直接 `InvalidConfiguration`。
- `approval != NotRequired`：审批机制落地前仍然**拒绝执行**（当前不提供 approve 回调和 UI）。
- UE 游戏动作工具建议：`side_effects = true`、`access_scope = Write/Host`、`thread_affinity = "host"`，
  并用 `AllowListToolPolicy({...}, true)` 或自定义 `ToolPolicy`（只放行动作工具名）授权。

### 4.1 长动作工具的正确形态

**不要让一个工具阻塞整个 turn 等待 3 秒动画/寻路完成**（worker 被占住、取消/超时变难、GameThread 也可能被拖住）。
推荐：

- 工具 = 「下令即返回」：`move_to` 只下发移动意图并返回 `{status:"accepted"}`；
- 下一条 turn 通过世界观测看到执行结果（位置/动作状态）；
- 如果必须等待，用短超时 + 可取消，并让 GameThread 任务可 drain。

## 5. ContextProvider（世界快照）

- 挂载：`RunnerDependencies.context_provider`；每轮调用一次，失败 fail-closed（见 [context-provider.md](context-provider.md)）。
- 数据来源必须走 GameThread（UWorld/UObject 只能 GT 访问）；worker 等待 Promise，超时按失败处理。
- 快照建议：`{time, self{pos,job,needs,action}, resources, nearby[], events[], actions[]}`；
  不要放密钥、全图隐藏信息或可注入文本。
- 预算：`AgentProfile.max_context_chars` 控制注入上限；UE 侧按 NPC 决策频率/模型上下文自行设定。

## 6. 最小落地步骤（B0 验收清单）

1. 插件骨架：`Plugins/MarianaAgent/MarianaAgent.uplugin`（Runtime）+ `MarianaAgent.Build.cs`（上面的配置）；
   从 `dist/agent-core/{debug,release}` 拷 `include/` + `lib/`（或源码集合）。
2. 实现最小 `FMarianaHttpTransport`（GameThread 创建请求 + worker 等待）。
3. `UMarianaSubsystem`：构造 `OpenAiChatModel` + 一个 `Agent`（profile 里先不放工具），
   控制台命令 `Mariana.Ask "<text>"` → worker 调 `Send` → 结果回 GameThread 打印。
4. 启动时校验 `agent::core::AbiVersion()` 与 `AGENT_CORE_ABI_VERSION`，打印 `BuildBanner()`。
5. **验收**：PIE 里 `Mariana.Ask "你好"` 得到真实模型回复；断网/超时/错误 key 有可读错误不崩；
   连续 20 次成功；GameThread 无 >50ms 额外阻塞；PIE 启停 5 次无崩溃/无悬挂线程。

B1（单 NPC ReAct）随后接入：工具桥 → `FMarianaTool`/`RegisterTool` → `AllowListToolPolicy`（允许副作用）→
动作工具 `/ 世界快照 Provider` → `UMarianaAgentComponent` → 失败回退 `UAgentBrainComponent` 风格本地行为。

## 7. 线程模型（不变量，必须遵守）

```text
GameThread        UObject / UMG / Slate / 事务 / ToolScheduler 任务
AgentWorker       包一层同步 Agent::Send；每 Session 串行
HTTP              异步完成，唤醒 Worker
UI EventQueue     Notifier 只投递，GT 消费
```

- GameThread 不调用阻塞 `Agent::Send`；Worker 不碰 UObject；
- ToolScheduler 不得让 GameThread 等待自己（避免互相等死锁）；
- Worker → GameThread 任务必须可取消/可超时/可 drain；
- 迟到回调按 `{session_id, run_id, generation}` 丢弃；
- 观察者（`AgentSystem`）异常被 runtime 隔离，但 UE tracer 仍应自己吞异常。

## 8. 观测 / trace / metrics

- `FileTracer(trace_dir, session_id)`：写 JSONL（`Saved/Logs/Mariana/`），内容截断 + `sk-` 脱敏；
  Editor 面板可直接解析。
- 或实现 `AgentSystem` 子类转发 `UE_LOG`；`OnTurnEnded(const TurnEvent&)` 包含
  `session_id/run_id/status/error_code/steps/tool_attempts/replayed`。
- `plugins/metrics` 的 `MetricsCollector` 按 Agent/Session 创建即可统计 token/费用；
  Host 负责把 metrics 数据接到编辑器面板。

## 9. 配置与密钥

- UE 侧配置：`UDeveloperSettings`（per-user ini）存 endpoint/model/timeout/预算；
  **不要**复用 CLI 的 `CliConfig`（`agent::hosts::cli` 命名空间属于 CLI 宿主）。
- API key：环境变量 / 用户设置 / 操作系统凭据；Shipping 走代理网关，客户端只拿短期 token。
- 不要把 `resources/config.json`、`config.local.json`、trace 目录打进 Shipping 包。

## 10. 已知限制（当前阶段）

- 无流式输出（`ChatModel` 同步非流式）；无 SSE token 级 UI。
- core 不提供 `SendAsync` / AgentManager / 多 Agent 调度（按设计属于 Host；参考 adapter 是示例，不是生产调度器）。
- 无多工具并发（同一 turn 串行执行）；无模型调用重试/退避/token 硬上限（F1 后续）。
- `approval = Required/Async` 工具暂不可执行（审批 UI/回调未实现）。
- Memory：Phase 1/2 已提供 `MemoryStore`/`Summarizer` 契约、`Noop`/`InMemory` 实现、`<memory>` 召回注入、
  会话压缩与 `RuleBasedSummarizer`/`ModelSummarizer`（见 [memory.md](memory.md)）；CLI/UE 持久化
  （SQLite/SaveGame）、eval 长期记忆任务待后续阶段。
- Eval harness：`agent-eval` mock 模式已提供（10/10 离线可复现），real 模式需 `DEEPSEEK_API_KEY`。
