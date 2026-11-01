#pragma once

#include <cstddef>
#include <string>

#include "agent/core/message.h"
#include "agent/core/tool.h"

namespace agent::runtime
{

struct ToolOutputBudget
{
    std::size_t max_bytes = 32 * 1024;
    std::size_t used_bytes = 0;
};

/// 单轮工具输出裁剪策略。
class ToolOutputPolicy
{
  public:
    virtual ~ToolOutputPolicy() = default;

    virtual std::string Limit(const core::ToolCall& call, std::string content, ToolOutputBudget& budget) const = 0;
};

/// 默认按单轮总预算截断。
class TruncateToolOutputPolicy final : public ToolOutputPolicy
{
  public:
    std::string Limit(const core::ToolCall& call, std::string content, ToolOutputBudget& budget) const override;
};

} // namespace agent::runtime
