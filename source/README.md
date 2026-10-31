# source

`source/` 是源码根目录，按模块组织。每个模块自包含，模块内目录约定：

```
<module>/
├── include/          # 对外头文件（可被其他模块 #include）
├── src/              # 实现（不对外暴露）
└── CMakeLists.txt    # 本模块的 target 定义
```

当前模块：

- `core/`：引擎无关的 agent 核心（`agent_core` 库，公共头文件 + 实现）
- `adapters/`：平台 / 协议适配（A1 起：`transport`、`model`、`storage`），当前占位
- `hosts/cli/`：宿主 #1，命令行（`agent-cli` 可执行文件）

后续会加入 `hosts/unreal/`、`hosts/android/`、`hosts/ios/` 等宿主模块。

## 规则

1. 依赖方向：`hosts` → `adapters` → `core`；core 不得反向 include 宿主或适配器。
2. 跨模块只通过目标（target）的 **PUBLIC include 目录** 与公开头文件依赖；不要 `#include "../../xxx/src/..."`。
3. 新增/改名模块时，同步更新根 `CMakeLists.txt` 的 `add_subdirectory`。
4. 每个模块只导出自有 `include/` 目录；`src/` 里的头文件视为私有实现。
