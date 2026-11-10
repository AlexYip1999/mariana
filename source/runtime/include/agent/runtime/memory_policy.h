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

    // 会话压缩（Phase 2）：历史超过阈值时把旧轮次压成一条摘要 System 消息
    bool enable_compaction = false;
    // 0 = 不触发；按存储历史的字符数估算
    std::size_t compact_trigger_chars = 0;
    // 保留最近 N 个 user turn 原文（0 按 1 处理）
    std::size_t compact_keep_recent_turns = 6;
    // 摘要输出字符上限；0 = 由 Summarizer 决定
    std::size_t compact_summary_max_chars = 2000;
};

} // namespace agent::runtime
