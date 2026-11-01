#pragma once

#include <cstddef>
#include <vector>

#include "agent/core/message.h"

namespace agent::runtime
{

/// 把完整 Conversation 转成一次模型请求的 messages。
class ContextPolicy
{
  public:
    virtual ~ContextPolicy() = default;

    virtual std::vector<core::ChatMessage> Prepare(const std::vector<core::ChatMessage>& messages,
                                                   std::size_t max_chars) const = 0;
};

/// 保留 system 前缀 + 从最近的 User 边界开始的完整轮次。
class KeepRecentTurnsPolicy final : public ContextPolicy
{
  public:
    std::vector<core::ChatMessage> Prepare(const std::vector<core::ChatMessage>& messages,
                                           std::size_t max_chars) const override;
};

} // namespace agent::runtime
