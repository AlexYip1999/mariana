# 构建与交付（静态库 / 头文件）

本文说明如何把 agent-core 作为**宿主无关库**交付给外部宿主（首选 UE 插件）。CLI 宿主
（`agent-cli` / `agent_cli_lib`）不对外交付。

## 1. 交付物

```
agent-core/
├── include/agent/{core,runtime,transport,model,plugins/*}/...   # 公共头文件
├── lib/agent_core.lib
├── lib/agent_runtime.lib
├── lib/agent_model.lib
├── lib/agent_metrics.lib
├── lib/agent_tools.lib
├── lib/agent_traces.lib
├── lib/agent_transport.lib        # 仅 Windows；UE 用 FHttpTransport，通常不需要
└── share/doc/agent-core/          # README / LICENSE / NOTICE / docs
```

## 2. 两套构建预设

| preset | 工具链 | 用途 |
|---|---|---|
| `vs2022` | VS 当前默认 MSVC（本机为 14.44） | 本地开发 / CLI |
| `vs2022-ue` | **MSVC 14.38.33130 + Windows SDK 10.0.22621**（UE 5.6 工具链） | 交给 UE 宿主 / Shipping 的库 |

```powershell
# 本地开发（默认工具链）
cmake --preset vs2022
cmake --build --preset debug
ctest --preset debug

# UE 交付（固定 UE 工具链）
cmake --preset vs2022-ue
cmake --build --preset debug-ue
cmake --build --preset release-ue
```

> 不要混用工具链：用 14.44 编出的 `.lib` 链进 14.38 的 UE 目标可能出现
> `LNK2038: mismatch detected for 'RuntimeLibrary'` / STL 边界未定义行为。
> UE 5.6 的 Windows SDK 主版本是 10.0.22621，`vs2022-ue` 预设已固定该 SDK。

## 3. CRT 映射（UE）

| UE Target 配置 | CRT | 使用哪套库 |
|---|---|---|
| Debug（编辑器 Debug Target） | `/MDd`（Debug CRT） | `Debug` 库 |
| Development / Shipping | `/MD`（Release CRT） | `Release` 库 |

同时保证：C++20、RTTI 开、异常开（UE 模块里 `bUseRTTI = true` / `bEnableExceptions = true`）；
见 [hosts-unreal.md](hosts-unreal.md)。

## 4. 安装

```powershell
# 两套配置分别安装到不同目录，宿主按 Target 选
cmake --install build/vs2022-ue --config Debug   --prefix dist/agent-core/debug
cmake --install build/vs2022-ue --config Release --prefix dist/agent-core/release
```

安装内容：`include/`、`lib/`、`share/doc/agent-core/`（README、LICENSE、NOTICE 与本目录文档）。

## 5. 库清单与链接顺序（对外宿主）

UE 插件只需要这些库：

```
agent_model.lib / agent_traces.lib / agent_tools.lib / agent_metrics.lib
        ↓
agent_runtime.lib
        ↓
agent_core.lib
```

- `agent_transport.lib`（WinHTTP）**不需要**：宿主自己实现 `HttpTransport`（UE 用 `FHttpModule`），
  只需要链接接口头文件。`agent_model` 内部只依赖 `HttpTransport` 抽象。
- 静态库顺序对 MSVC 不严格（链接器可迭代查找），但建议按依赖方向从上层到下层列出；
  遇到 `LNK2019` 时先检查是否漏了 `agent_runtime.lib`/`agent_core.lib`。
- 不要链 `agent_cli_lib` / `agent-cli`：CLI 配置、工具目录与命令系统不属于通用运行时。

## 6. 版本 / ABI 校验（宿主必做）

- 编译期宏：`AGENT_CORE_VERSION`（如 `"0.1.0"`）、`AGENT_CORE_ABI_VERSION`（当前 `1`）。
- 运行期函数：`agent::core::Version()`、`agent::core::AbiVersion()`、`agent::core::BuildBanner()`。
- 宿主启动时建议：

```cpp
const int host_abi = AGENT_CORE_ABI_VERSION;      // 宿主编译时的头文件 ABI
if (agent::core::AbiVersion() != host_abi)        // 链接进来的库 ABI
{
    // 拒绝启用 agent 功能并打印明确错误
}
```

- 升级规则：公共头文件签名、枚举值、结构体布局或链接接口发生不兼容变化时
  同时修改 `version.h` 的 `AGENT_CORE_ABI_VERSION` 默认值并重新发布库；宿主升级库时重跑上述校验。

## 7. 源码集合交付（备选）

若不想管理静态库/CRT 矩阵，可把以下目录作为「源文件集合」编进宿主插件：

```
source/core/{include,src}
source/runtime/{include,src}
source/integrations/model/{include,src}
source/integrations/transport/{include}       # 只要接口头（宿主自实现）
source/plugins/{metrics,tools,traces}/{include,src}
source/hosts/reference/{include,src}          # 可选：参考 Host Adapter
```

代价：宿主编译配置必须与 core 一致（C++20 / RTTI / 异常 / 无第三方宏冲突）；
收益：完全避开 CRT/工具链/ABI 问题，适合 UE 插件早期阶段（F2/P0）。

## 8. 常见链接问题

| 现象 | 原因 | 处理 |
|---|---|---|
| `LNK2038 mismatch ... RuntimeLibrary` | CRT 不一致（`/MD` vs `/MDd`） | 宿主 Debug Target 用 Debug 库，其余用 Release 库 |
| `LNK2019 unresolved ... type_info` | 宿主关闭 RTTI（UE 默认关） | UE 模块 `bUseRTTI = true` |
| 异常在模块边界崩溃 | 宿主关闭异常（UE 默认关） | UE 模块 `bEnableExceptions = true` |
| `LNK2019 unresolved nlohmann::...` | 链接了静态库却链接不到 third_party 目标 | 静态库内已编入 nlohmann；检查是否误链了自编的 core 源文件 |
| 头文件冲突（`check`/`TEXT` 宏） | 把 UE 头文件混进 core 编译单元 | core 公共头不包含 UE 类型；不要反过来把 UE 头塞给 core |
