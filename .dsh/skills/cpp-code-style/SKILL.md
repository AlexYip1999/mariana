---
name: cpp-code-style
description: 用于在 agent-core 项目中新增、修改、评审或提交 C++ 代码时。强制项目代码规范：Allman 大括号（右大括号必须独占一行）、switch 每个 case/default 必须有 {} 块、PascalCase 命名（类型/函数）、snake_case 命名（变量/文件，成员变量 m_ 前缀）、多字段结构体/函数/类/测试用例按语义分组空行、长布尔条件必须抽成命名函数、namespace 内部使用相对限定名、4 空格缩进、120 列，并在改完代码后运行 scripts/format.ps1、scripts/check-format.ps1、scripts/verify.ps1。任何 .h/.hpp/.cpp 改动后都必须使用。
---

# C++ 代码规范（agent-core）

## 何时使用

在 agent-core 仓库新增、修改、评审 C++ 代码后（`source/*/include/`、`source/*/src/`、`tests/` 下的 `.h` / `.hpp` / `.cpp`），或准备提交前；也用于全仓 / 局部可读性整理（按语义分组空行与注释）。

## 铁律

1. **右大括号 `}` 必须独占一行**（行首，保留缩进），包括：
   - 块级大括号（函数 / 控制流 / namespace 等）；
   - 多行花括号初始化列表里的每个嵌套元素（`{`、`}` 都独占一行）。
   - 例外：单行语句（以 `;` 结尾）可保持紧凑，如 `return {};`、`std::vector<int> v = {1, 2};`。
   - 例外：**空函数体写成 `{}` 单行**，如 `virtual void OnTurnStart() {}`。
   - 禁止：`if (ok) { Do(); }`、`} else {`、多行列表中的 `{agent::core::ChatRole::User, "hi"},`。
   - 允许：`};`、`}  // namespace agent::core`、`void f() {}`。
2. **左大括号同样换行**（Allman）。大括号位置由 `.clang-format` 决定，不要手调；空函数体例外，允许 `{}` 单行。
3. 缩进 4 空格、不使用 Tab、行宽 120。
4. 格式以 `clang-format` 的结果为准，不要凭感觉"看起来对齐"。
5. **长布尔条件必须抽成命名函数**，不要在 `if` / `while` / 三元表达式里堆长条件。
   - 触发条件：条件跨多行、混合 `&&` / `||` 超过 2～3 个，或需要注释才能看懂含义；
   - 简单单条件（如 `if (name.empty())`）不必抽；
   - 抽成 `Is...` / `Has...` / `Can...` 的 PascalCase 函数，名字表达业务语义，不要用 `CheckCondition()`；
   - 纯判断函数优先 `inline` 放头文件，或放 `.cpp` 的匿名命名空间；
   - 例：

```cpp
// 不好：条件难读，且无法复用
if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')
{
    // ...
}

// 好：抽成命名函数
namespace detail
{
inline bool IsToolNameCharacter(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
}
} // namespace detail

if (!detail::IsToolNameCharacter(c))
{
    return false;
}
```

初始化列表正确写法：

```cpp
request.messages = {
    {
        agent::core::ChatRole::System,
        "be brief",
    },
    {
        agent::core::ChatRole::User,
        "hi",
    },
};
```

错误写法（检查器会拦截）：

```cpp
request.messages = {
    {agent::core::ChatRole::System, "be brief"},
    {agent::core::ChatRole::User, "hi"},
};
```

6. **switch 的每个 `case`/`default` 必须用 `{}` 包成块**（即使只有一条 `return` / `break`）：
   - case 标签相对 `switch` 缩进一级（`.clang-format`: `IndentCaseLabels: true`）；
   - `{` 在标签下一行、与标签同缩进；`}` 独占一行；块内语句再缩进 4 空格；
   - 连续 fallthrough 标签（`case A:` 紧跟 `case B:`）共用最后一个标签后的块；
   - `.clang-format` 已配置 `BraceWrapping.AfterCaseLabel: true`；`scripts/check-format.ps1` 会检查缺失的块；
   - clang-format **不会自动插入** case 的 `{}`，新增 switch 时要手写。

   ```cpp
   switch (kind)
   {
       case MemoryKind::Fact:
       {
           return "Fact";
       }
       default:
       {
           return "Unknown";
       }
   }
   ```

## 长类型别名

`std::function` / 回调类型如果跨行、超过约 80 列，或作为公开字段/函数参数重复出现，优先用 `using` 起语义化别名。

```cpp
// 不好：字段声明被类型签名淹没
std::function<std::shared_ptr<runtime::AgentExecutor>(
    const std::filesystem::path&)> create_executor;

// 好：名字表达用途
using ExecutorFactory =
    std::function<std::shared_ptr<runtime::AgentExecutor>(const std::filesystem::path&)>;

ExecutorFactory create_executor;
```

规则：

- 别名名用 PascalCase，表达“这个回调是做什么的”，例如 `ExecutorFactory`、`TransportFactory`、`EnvironmentLookup`；
- 不要起 `FuncType1` / `CallbackAlias` 这种没有语义的名字；
- 短小、一次性的 `std::function<void()>` 参数可以不单独起别名；
- 别名放在使用它的命名空间内，避免污染全局；
- 别名只解决可读性，不改变类型、ABI 或性能。

## 结构体排版

多字段 `struct` 按语义分组，组间空一行；必要时加短注释。目标是让读者一眼看出“输入 / 参数 / 上下文 / 结果 / 错误”。这是下一节《语义空行分组》在结构体上的特例；函数、类与测试用例同样适用下一节的规则。

```cpp
struct ChatRequest
{
    // 模型与输入
    std::string model;
    std::vector<ChatMessage> messages;
    std::vector<ToolSpec> tools;

    // 采样参数
    double temperature = 0.0;
    int max_tokens = 1024;

    // 单次调用上下文：取消与 deadline
    CallContext call;
};

struct ChatResponse
{
    // 结果
    bool ok = false;
    std::string content;
    std::vector<ToolCall> tool_calls;

    // 用量与结束原因
    ChatUsage usage;
    std::string finish_reason;

    // 错误
    std::string error;
    CallError error_code = CallError::None;
};
```

规则：

- 触发条件：结构体字段较多、字段属于 2 类以上语义，或阅读时容易混淆；
- 字段少（≤3）或本身就是同一元组（如坐标、版本号）时，不强制分组、不硬塞注释；
- 不要为了“好看”重排有序列化/ABI/字段偏移要求的字段；只加空行与注释；
- 注释描述“这一组是什么”，不要逐字段复述字段名；
- 分组不改变字段顺序和初始化值。

## 语义空行分组（函数 / 类 / 测试）

结构体分组只是这条规则的特例。函数体、类成员与测试用例也要按语义用**一个空行**分段；只在阶段含义不直观时补一行 `//` 短注释（中文；若文件注释是英文则用英文）。目标是让读者一眼看出“前置检查 / 准备 / 主逻辑 / 结果处理”。

- **函数体**：按阶段分段，例如 入参与前置检查 → 准备/构造 → 主逻辑 → 结果与错误处理；每个函数最多 1～2 条阶段注释，不要逐句加空行，不要块首/块尾空行。
- **类**：成员变量与方法按角色分组（构造/重写、查询、内部状态等），组间空行 + 简短组注释；已有清晰注释的分组不要重复。
- **测试**：每个 `TEST_CASE` 按 arrange（夹具/模型/mock 准备）→ act（调用）→ assert（断言）分段；断言可按“结果 / 计数 / mock 交互 / 历史”等再分组，不要每条断言都空行。
- **不用分割线注释**（如 `// ----`、`// ====`）做分组：仓库统一用业务语义的 `//` 注释分组。

整理时的硬约束：

- **只允许新增空行与 `//` 注释**：不改代码、不移动/删除已有注释、不调整 include 与字段/成员顺序。
- 连续空行最多 1 行；行宽 ≤120；大括号风格与缩进保持不变（Allman、4 空格）。
- **不为改而改**：已经清晰的文件/函数/分组直接跳过；不要机械地“每行之间加空行”。
- 可读性整理单独提交，用 `style:` 前缀；提交前确认 `git diff` 只有 `+` 行（既有注释位置调整除外），并照常跑 format / check-format / verify。

## 改完代码后的固定流程（顺序不要换）

```powershell
# 本机使用 Windows PowerShell 5.1，且执行策略限制未签名脚本，必须带 -ExecutionPolicy Bypass
# 1. 自动格式化（会原地修改文件）
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\format.ps1

# 2. 格式检查，必须退出码 0
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\check-format.ps1

# 3. 格式 + 构建 + 测试 全套校验
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\verify.ps1
```

做不到全套时，至少执行：格式检查 + `cmake --build --preset debug` + `ctest --preset debug`。

提交前再执行 `git status`，确认没有 `build/`、`traces/`、密钥、本地配置等文件被加入。

## 提交规则（必须遵守）

- **绝不自动执行 `git commit` / `git push`**：改完代码、跑完 format/check/verify 后，汇总改动与测试结果，等待用户明确确认后再提交。
- 用户确认后再提交；提交信息用约定式前缀：`feat:` / `fix:` / `style:` / `test:` / `docs:` / `chore:`。

## 命名规范（类型/函数 PascalCase，变量 snake_case）

| 元素 | 规则 | 示例 |
|---|---|---|
| 类型：class / struct / enum / using 别名 | PascalCase | `ToolRegistry`、`RunState`、`HttpClient` |
| 函数 / 方法（含自由函数、测试辅助） | PascalCase | `BuildBanner()`、`ExecuteTool()` |
| 变量 / 参数（局部、函数参数） | snake_case | `max_steps`、`request_count` |
| 成员变量（类/对象） | `m_` + snake_case | `m_handler`、`m_request_count` |
| 常量 / 宏 | UPPER_SNAKE_CASE | `AGENT_CORE_VERSION`、`MAX_STEPS` |
| 命名空间 | 全小写 snake_case | `agent::core`、`agent::transport` |
| C++ 文件名（源文件 / 头文件） | 全小写 snake_case | `http_transport.h`、`fake_http_transport.h`、`version.cpp` |
| 枚举值 | PascalCase | `RunState::Waiting` |

### 文件命名

- C++ 源文件与头文件一律**全小写 snake_case**，不使用驼峰（`HttpTransport.h`）、中划线（`http-transport.h`）或空格。
- 文件名不要求和类名完全一致；类型名在文件内保持 PascalCase（`http_transport.h` 里放 `HttpTransport`）。
- 理由：Linux 大小写敏感、Windows/macOS 不敏感，全小写可以避免跨平台构建、CI 和 Git 大小写改名问题；同时与目录（`source/adapters/transport`）、命名空间（`agent::transport`）保持一致。
- 例外：`README.md`、`.clang-format` 等社区惯例文件名，以及脚本/文档文件名不强制 snake_case（脚本当前用 kebab-case，如 `check-format.ps1`；如需统一可另行决定）。

- 函数/方法用 PascalCase，变量/参数/成员用 snake_case；不要用小驼峰命名函数（`processData` 不行）。
- 缩写按单词处理：类型 `HttpClient`，函数 `ParseHttpResponse`。
- **接口不加 `I` 前缀**：抽象基类用角色名，具体实现加实现名。例如 `HttpTransport` → `CurlHttpTransport` / `UeHttpTransport`；`ChatModel` → `OpenAiChatModel`。文件名与类名一致（`http_transport.h`）。
- clang-format 只负责排版，**不检查命名**；命名靠评审与这条规则约束。
- 不要引入 UE 宏风格（`UPROPERTY` 等属于 UE 宿主层，core 层禁止）。

## 其他约定

- 头文件使用 `#pragma once`；include 分组与排序交给 clang-format。
- **函数/方法定义之间必须空一行**（包括空实现的 override，如 `NullTracer`）；不要写成连续贴在一起。
  - `.clang-format` 已配置 `SeparateDefinitionBlocks: Always`；
  - **空函数体写成 `{}` 单行**，如 `virtual void OnTurnStart() {}`；`.clang-format` 已配置 `AllowShortFunctionsOnASingleLine: Empty`。
  - **带初始化列表的构造函数**：初始化列表换行，`{}` 单独一行；多个初始化项时逗号放行首：

    ```cpp
    explicit FakeSteadyClock(std::vector<std::chrono::steady_clock::time_point> points)
        : m_points(std::move(points))
    {}

    OpenAiChatModel::OpenAiChatModel(transport::HttpTransport& transport, OpenAiConfig config)
        : m_transport(transport)
        , m_config(std::move(config))
    {}
    ```

    对应配置：`PackConstructorInitializers: Never`、`BreakConstructorInitializers: BeforeComma`、`BraceWrapping.SplitEmptyFunction: false`。
  - 非空函数体仍保持 Allman：`{` 和 `}` 各占一行。
- **namespace 内部使用相对限定名**：在 `namespace agent::...` 内引用 `agent` 下的兄弟命名空间时，写 `core::`、`model::`、`transport::`、`runtime::`、`plugins::`，不要重复 `agent::`。
  - 例：`namespace agent::runtime` 内写 `core::ChatModel`，而不是 `agent::core::ChatModel`；`source/integrations/transport` 的 include 根仍是 `agent/transport`。
  - `main.cpp` 等全局作用域、以及 `tests/` 里不在 `agent` 命名空间内的代码，仍使用完整 `agent::...`。
- **不要格式化或修改 `third_party/`、`build/`**。
- 不要修改 `.clang-format`、`scripts/*.ps1`，除非任务明确就是改规范；规范变更单独提交。
- `switch` 的 case/default 块规则见铁律 6；clang-format 不会自动补 `{}`，新增 case 要手动写块。
- core 层禁止 include UE / 平台 / 网络库头文件；依赖通过接口（见 `Agent项目-实施路径.md`）。

## 完成定义（DoD）

- [ ] `scripts/check-format.ps1` 通过（退出码 0）
- [ ] `scripts/verify.ps1` 通过（或格式、构建、测试分别通过）
- [ ] 块级 `}` 和多行初始化列表的 `}` 都在行首（单行语句 / `return {};` / 空函数 `{}` 除外）
- [ ] switch 每个 `case`/`default` 都有 `{}` 块（fallthrough 组共用最后一个标签的块）
- [ ] 命名符合规范：类型/函数 PascalCase，变量/文件 snake_case，成员变量 `m_` 前缀
- [ ] 长布尔条件已抽成命名函数（`Is...` / `Has...` / `Can...`）
- [ ] 函数/方法定义之间有空行（包括空实现的 override）
- [ ] 空函数体写成 `{}` 单行（`virtual void OnTurnStart() {}`）
- [ ] 带初始化列表的空构造函数写成初始化列表 + 单独一行 `{}`
- [ ] 多初始化项的逗号在行首（`, m_config(...)`）
- [ ] 多字段结构体按语义分组，组间空行，必要处有短注释
- [ ] 函数体按阶段用空行分段（每函数阶段注释 ≤2 条，不逐句空行、无块首/块尾空行）
- [ ] 类成员与测试用例按语义分组，组间空行（测试按 arrange / act / assert 分段）
- [ ] 可读性整理只新增空行与 `//` 注释，`git diff` 无删除行（既有注释位置调整除外）
- [ ] 长的 std::function / 回调类型已用语义化 `using` 别名
- [ ] `agent::...` namespace 内部使用相对限定名（`core::` / `model::` / `transport::` / `runtime::` / `plugins::`）
- [ ] `third_party/`、`build/` 未被改动
- [ ] `git status` 无意外文件
