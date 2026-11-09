#pragma once

#include <cstddef>
#include <vector>

#include "agent/core/memory.h"

namespace agent::runtime
{

/// 长期记忆的通用策略；默认关闭，因此未配置 Memory 的 Host 行为不变。
struct MemoryPolicy
{
    // 召回（每轮一次，注入 <memory> System 消息）
    bool enable_recall = false;
    std::size_t recall_limit = 8;
    // 0 = 只受 profile.max_context_chars 限制
    std::size_t recall_budget_chars = 2000;
    // 空 = 不限制 scope（由 MemoryStore 决定）；Host 通常填当前 agent + 共享 world
    std::vector<core::MemoryScope> recall_scopes;
};

} // namespace agent::runtime
