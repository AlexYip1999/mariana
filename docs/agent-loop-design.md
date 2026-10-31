# Agent 循环（A2）设计草案

- 状态：草案，待评审
- 目标版本：A2
- 关联：`ChatModel`、CLI host、DeepSeek Chat Completions

## 1. 背景

A1 已经完成：

- CLI 单轮问答；
- `--interactive` 多轮终端聊天；
- 配置分层（`resources/config.json` + `config.local.json`）；
- OpenAI 兼容的 `OpenAiChatModel` + 临时 `WinHttpTransport`。

A1 的每一次用户输入只对应一次 `ChatModel::Chat()`。A2 要让模型可以：

1. 请求调用工具；
2. 读取工具结果；
3. 继续决策，直到给出最终回答。

这就是 Agent 循环。A2 只做**单 Agent、同步、单会话**的最小闭环，为未来的异步、多会话、Router/Worker 留出接口。

## 2. 目标与非目标

### 目标

- 一个可测试的默认 ReAct 执行器（`AgentRunner`），支持 `max_steps` 限制；
- 通过 `AgentExecutor` 接口隔离执行策略，避免 host/core 绑死在单一流程上；
- 工具通过统一接口注册和调用；
- 模型请求/响应支持 `tool_calls` 和 `tool` 消息；
- 第一个工具：`read_file`（只读、限制在工作区）；
- 离线测试不依赖网络和真实模型；
- 每轮决策和工具调用可观测（Tracer 接口，先做空实现或简单日志）。

### 非目标（A2 不做）

- Router / Worker 双层架构；
- 异步 ReAct / 事件队列；
- 多 Agent 协作、跨会话上下文；
- Watchdog、投机执行；
- 会话持久化；
- 写文件、执行命令等危险工具；
- UE 宿主接入。

这些留给 A3/A4，A2 只保证接口不挡路。

## 3. 分层与职责

```text
hosts/cli/ue/im
        │  产生用户事件 / 展示输出
        ▼
agent（编排层）
  AgentExecutor 接口 / AgentRunner（ReAct 默认实现）
  ToolRegistry / AgentProfile / ContextProvider / Tracer 实现
        │
        ▼
core（接口与数据）
  ChatModel / ChatMessage / Tool / ToolResult / ToolSpec / Tracer 接口
        │
        ├── adapters/model（OpenAI 序列化、tool_calls 解析）
        ├── tools（read_file 等具体工具）
        └── adapters/transport（HTTP）
```

原则：

- `core` 不依赖 nlohmann/json、不带平台/网络头文件；
- 工具参数和工具结果在 core 里用 **JSON 字符串**传递，具体工具自己解析；
- host 只负责输入/输出，不实现循环；
- `AgentRunner` 不读 stdin、不读全局配置，依赖通过构造参数注入。

> 实现位置可以在 A2 评审后微调：如果 `source/core` 过重，就新增 `source/agent` 库；接口归属不变。

## 4. 数据模型

### 4.1 消息

```cpp
enum class ChatRole
{
    System,
    User,
    Assistant,
    Tool,
};

struct ToolCall
{
    std::string id;
    std::string name;
    std::string arguments_json;
};

struct ChatMessage
{
    ChatRole role = ChatRole::User;
    std::string content;
    // assistant 消息可能同时带 content 和 tool_calls
    std::vector<ToolCall> tool_calls;
    // role == Tool 时必填
    std::string tool_call_id;
};
```

### 4.2 请求与响应

```cpp
struct ToolSpec
{
    std::string name;
    std::string description;
    std::string parameters_json;  // JSON Schema 字符串
};

struct ChatRequest
{
    std::string model;
    std::vector<ChatMessage> messages;
    std::vector<ToolSpec> tools;
    double temperature = 0.0;
    int max_tokens = 1024;
};

struct ChatResponse
{
    bool ok = false;
    std::string content;
    std::vector<ToolCall> tool_calls;
    std::string error;
};
```

说明：

- `tools` 为空时就是普通聊天，A1 行为不变；
- `tool_calls` 为空表示模型给了最终回答；
- `content` 与 `tool_calls` 同时存在是合法的，最终回答优先取 `content`。

### 4.3 工具

```cpp
struct ToolResult
{
    bool ok = true;
    std::string content;
    std::string error;
};

class Tool
{
  public:
    virtual ~Tool() = default;

    virtual std::string Name() const = 0;
    virtual std::string Description() const = 0;
    virtual std::string ParametersJson() const = 0;
    virtual ToolResult Execute(const std::string& arguments_json) = 0;
};
```

- 工具名全局唯一，建议 snake_case（`read_file`）；
- `ParametersJson` 返回 JSON Schema，用于生成 OpenAI `tools` 数组；
- 工具自己解析 `arguments_json`，失败时返回 `ok=false` + 可读错误；
- 所有工具执行必须设置上限：文件大小、输出长度、超时。

### 4.4 注册表

```cpp
class ToolRegistry
{
  public:
    void Register(std::shared_ptr<Tool> tool);
    Tool* Find(const std::string& name) const;
    std::vector<ToolSpec> Specs() const;
};
```

- 注册重复名称直接报错/拒绝；
- `Specs()` 只返回允许本轮使用的工具；
- 查找失败时由 `AgentRunner` 转成工具错误结果回给模型，而不是直接崩溃。

### 4.5 Profile 与会话

```cpp
struct AgentProfile
{
    std::string system_prompt;
    std::vector<std::string> tool_names;
    int max_steps = 8;
};

struct Conversation
{
    std::vector<ChatMessage> messages;
};
```

- `Conversation` 是可序列化的会话状态，未来可被 Router 分配/持久化；
- `AgentProfile` 是场景配置：代码助手、问答、自动化流程只是工具集和提示词不同；
- 单 Agent 就是“一个 Conversation + 一个 AgentRunner”。

### 4.6 Tracer

```cpp
class Tracer
{
  public:
    virtual ~Tracer() = default;

    virtual void OnModelRequest(const ChatRequest& request) = 0;
    virtual void OnModelResponse(const ChatResponse& response) = 0;
    virtual void OnToolCall(const ToolCall& call) = 0;
    virtual void OnToolResult(const ToolCall& call, const ToolResult& result) = 0;
};
```

A2 先提供 `NullTracer`；A2.2 再加文件 trace（JSONL，写入 `traces/`）。

## 5. AgentRunner（默认 ReAct 执行器）

```cpp
struct AgentRunResult
{
    bool ok = false;
    std::string content;
    std::string error;
    int steps = 0;
};

class AgentRunner
{
  public:
    AgentRunner(ChatModel& model, const ToolRegistry& tools, Tracer& tracer);

    AgentRunResult Run(const AgentProfile& profile, Conversation& conversation, const std::string& user_input);
};
```

### 5.1 单轮流程

```text
1. 把 user_input 追加为 User 消息
2. 构造 ChatRequest：
   - model / temperature / max_tokens 由调用方或 Profile 提供
   - messages = conversation.messages
   - tools = 按 profile.tool_names 从 registry 取 Specs()
3. Tracer::OnModelRequest
4. response = model.Chat(request)
5. Tracer::OnModelResponse
6. 把 assistant 消息（content + tool_calls）追加到 conversation
7. 如果 tool_calls 为空 → 返回 content，结束
8. 对每个 tool_call：
   - 查工具；找不到 → 生成错误 ToolResult
   - Tracer::OnToolCall
   - 执行工具
   - Tracer::OnToolResult
   - 追加 role=Tool、tool_call_id、content/error 的消息
9. steps += 1；若 steps >= max_steps → 返回错误；否则回到 2（不重复追加 user）
```

### 5.2 终止条件

- 模型返回无 `tool_calls` → 成功；
- 达到 `max_steps` → 失败，错误信息明确；
- 模型调用失败 → 失败，保留已有会话；
- 工具失败不终止循环：把错误作为工具结果回给模型，让它选择重试或换方案；
- 连续工具失败次数可配置上限（A2 先固定 3）。

### 5.3 安全限制

- 第一版工具只读；
- `read_file` 只允许工作区相对路径，拒绝绝对路径和 `..` 越界；
- 单文件读取大小上限（例如 256KB）和总输出上限；
- 工具执行不能阻塞过久；
- 写操作、命令执行必须等到审批机制和沙箱设计完成后再加。

### 5.4 执行策略可插拔

`AgentRunner` 只是默认的 **ReAct 执行器**，不是所有应用的固定流程。A2 引入接口，但不实现其他执行器：

```cpp
class AgentExecutor
{
  public:
    virtual ~AgentExecutor() = default;

    virtual AgentRunResult Run(ChatModel& model, const ToolRegistry& tools, const AgentProfile& profile,
                               Conversation& conversation, const std::string& user_input) = 0;
};
```

候选执行器：

| 执行器 | 适用场景 | A2 |
|---|---|---|
| ReActExecutor（AgentRunner） | 通用工具调用、代码助手 | 默认实现 |
| PlanExecuteExecutor | 先规划再执行的复杂任务 | 后续 |
| StateMachineExecutor | NPC 对话/行为、游戏状态机 | 后续 |
| WorkflowExecutor | DAG / 低代码流程 | 后续 |

host 只依赖 `AgentExecutor`，不直接绑 `AgentRunner`。这样以后新增执行器不需要改 CLI/UE host。

### 5.5 应用流程与上下文结构不同

不同应用的流程和上下文本来就不一样，不能用同一个 Profile 硬套：

| 应用 | 上下文结构 | 典型流程 |
|---|---|---|
| 游戏引擎助手 | 项目文件、API 文档、编辑器选中内容、构建错误 | ReAct + 工具 + 人工确认 |
| NPC Agent | 世界状态、角色设定、玩家关系、记忆 | 状态机/行为树；模型只在决策点调用 |
| 自动化流程 | 任务状态、审批记录、外部系统数据 | DAG / 审批流；模型是其中节点 |
| 知识问答 | 检索结果、引用、权限 | 检索 → 重排 → 生成，可能没有工具循环 |

建议引入：

```cpp
class ContextProvider
{
  public:
    virtual ~ContextProvider() = default;

    virtual std::string BuildContext(const AgentContextRequest& request) = 0;
};
```

- 每个应用注册自己的 ContextProvider；
- `AgentProfile` 选择工具集、ContextProvider、执行器和提示词；
- 上下文先用 JSON 字符串承载，避免 core 绑定具体领域结构。

### 5.6 未来：低代码 Flow 层

低代码拼装的目标是“节点图 + 执行器注册”，而不是把流程写死在 C++ 里：

```json
{
  "version": 1,
  "nodes": [
    {"id": "input", "type": "input"},
    {"id": "ctx", "type": "context", "provider": "ue_editor"},
    {"id": "llm", "type": "model", "model": "deepseek-flash"},
    {"id": "check", "type": "condition", "expr": "need_file"},
    {"id": "read", "type": "tool", "name": "read_file"},
    {"id": "out", "type": "output"}
  ],
  "edges": []
}
```

候选节点类型：

- `input` / `output`
- `model`
- `tool`
- `condition`
- `context`
- `sub-agent`
- `human-approval`

低代码层要成立，需要先有稳定契约：

- 节点输入/输出端口类型；
- 上下文 schema；
- 工具/执行器注册表；
- 错误、超时、取消传播；
- trace 与调试能力。

**A2 不实现 Flow 层**，只保证原语（消息、工具、执行器、上下文、trace）不挡路。

### 5.7 与 DeepSeek 插件思想的关系

方向一致，但层次不同：

- **DeepSeek function calling / tools**：能力插件化的协议；
- **DeepSeek Harness skills / plugins / 事件源**：host 侧扩展；
- **Super-agent 的 Router / Worker / 事件订阅**：编排与执行分离。

我们的映射：

| 概念 | 对应 |
|---|---|
| 能力插件 | `Tool` / 未来 `ContextProvider` / 事件源 |
| 执行策略插件 | `AgentExecutor`（`AgentRunner` 是默认实现） |
| 编排层 | 未来 Flow 图 / Router |
| 宿主插件 | CLI / UE / IM host |

所以插件化解决“能力怎么接”，执行器/Flow 解决“流程怎么编排”，两者互补。

## 6. OpenAI / DeepSeek 协议

请求体在 A1 基础上增加：

```json
{
  "tools": [
    {
      "type": "function",
      "function": {
        "name": "read_file",
        "description": "Read a text file inside the workspace",
        "parameters": {
          "type": "object",
          "properties": {
            "path": {"type": "string"}
          },
          "required": ["path"]
        }
      }
    }
  ],
  "tool_choice": "auto"
}
```

响应解析：

- `choices[0].message.tool_calls[]` → `ToolCall { id, function.name, function.arguments }`；
- `arguments` 是 **JSON 字符串**，不要二次解析成对象再存 core；
- `finish_reason == "tool_calls"` 仅用于日志/测试，循环判断以 `tool_calls` 是否为空为准。

工具结果回传：

```json
{
  "role": "tool",
  "tool_call_id": "call_xxx",
  "content": "文件内容或错误信息"
}
```

A2 开工前先做一次真实 API 验证（见 10.3）。

## 7. 错误处理

| 情况 | 行为 |
|---|---|
| 模型 HTTP/JSON 错误 | 返回失败，不追加 assistant 消息 |
| 模型没有返回 content 也没有 tool_calls | 视为失败，错误信息明确 |
| 未知工具名 | 生成错误 ToolResult，回给模型 |
| 工具参数 JSON 非法 | 工具返回 `ok=false`，回给模型 |
| 工具执行失败 | 回给模型；连续失败超过阈值则终止 |
| max_steps 达到 | 返回失败，附带已完成的消息 |
| 会话历史过长 | A2 先不做裁剪，记录 TODO；A3 加 token 预算 |

## 8. 可观测性

A2 先实现 Tracer 接口 + NullTracer，A2.2 加文件 trace：

```text
traces/
  2026-11-01T...jsonl
```

事件字段建议：

- `event`: `model_request` / `model_response` / `tool_call` / `tool_result`
- `step`
- `duration_ms`
- `tool_name` / `tool_call_id`
- `ok` / `error`
- `usage`（如果响应带 token 统计）

禁止记录 API key、Authorization header、完整密钥。

## 9. 测试策略

`FakeChatModel`：按预设脚本依次返回响应，同时记录收到的 `ChatRequest`。

`FakeTool`：记录调用参数，返回固定结果或错误。

测试用例：

1. 无工具调用 → 一次回答，`steps=1`；
2. 一次 `tool_calls` → 执行工具 → 第二次模型返回最终答案；
3. 多工具调用 → 按顺序执行，所有结果都回传；
4. 未知工具 → 错误结果回传，模型可恢复；
5. 工具失败 → 错误回传，模型可恢复；
6. 达到 `max_steps` → 返回失败；
7. `read_file` 路径越界 → 拒绝；
8. 请求中的 `tools` 只包含 profile 允许的工具。

真实 API 测试单独做，不进入 ctest。

## 10. 里程碑

### A2.0 协议验证

- 用 curl 发一个带 `tools` 的请求；
- 确认返回 `tool_calls` 结构；
- 回传 `role=tool` 后模型能继续回答。

### A2.1 最小闭环

- core 数据模型扩展；
- `AgentExecutor` 接口 + `Tool` / `ToolRegistry` / `AgentRunner`（ReAct 默认实现）/ `AgentProfile` / `Tracer`；
- `read_file` 工具；
- FakeChatModel 离线测试；
- CLI 增加 `--agent`（或 interactive 走 AgentRunner）。

### A2.2 真机与可观测

- 真实 DeepSeek tool call 联调；
- 文件 Tracer；
- 错误/重试/限制完善。

### A2.3 演进预留

- 异步 Step 驱动接口（不实现）；
- Conversation 序列化；
- Router/Worker 需要的 session_id / 事件订阅字段。

## 11. 未来演进映射

| 未来能力 | A2 预留点 |
|---|---|
| 执行策略插件 | `AgentExecutor` 接口；`AgentRunner` 是默认 ReAct 实现 |
| 低代码 Flow | 原语稳定后，以节点/端口/上下文 schema 为基础单独做 |
| 应用上下文 | `ContextProvider` 接口；Profile 选择 provider |
| 异步 ReAct | `AgentRunner::Step()` 与循环驱动分离，未来用事件队列调用 Step |
| Router / Worker | `Conversation` + `AgentExecutor` 可实例化多个，Router 负责分配 |
| 多会话 | Conversation 可序列化、带 session_id |
| Watchdog | Tracer 事件 + Policy 钩子 |
| UE 宿主 | 只换 host，`AgentExecutor` 不变 |
| 会话持久化 | Conversation 通过 `MessageStore` 接口落盘 |

## 12. 已达成共识与待确认决策

### 已达成共识

- 单一 ReAct loop 只是默认执行策略，不是所有应用的业务流程；
- `AgentRunner` 定位为默认 ReAct 执行器，未来通过 `AgentExecutor` 接口替换；
- 不同应用（引擎助手/NPC/自动化/知识问答）使用不同上下文结构和 Flow；
- 低代码 Flow 是上层产品形态，依赖稳定原语后再做；
- 方向与 DeepSeek 的插件化 + 编排/执行分离一致。

### 待确认决策

1. `AgentExecutor` 接口在 A2 就引入，还是先写 `AgentRunner` 后续再抽；
2. `AgentRunner` 放在 `source/core` 还是新增 `source/agent`；
3. `Tool` 接口放 core（JSON 字符串）是否合适，还是放到独立的 tools 接口库；
4. 第一版是否允许一次响应多个 `tool_calls` 并行执行（建议先串行）；
5. `AgentProfile` 是否包含 model/temperature，还是复用 adapter 配置；
6. `ContextProvider` 的位置与数据类型（JSON 字符串 vs 强类型结构）；
7. trace 文件格式与落盘位置；
8. CLI `--agent` 与 `--interactive` 的关系（新增模式 vs interactive 默认走 Agent）；
9. Flow DSL 从什么时候开始设计（建议 A3 之后）。
