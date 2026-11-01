#pragma once

#include <string>

#include "agent/core/message.h"
#include "agent/core/tool.h"

namespace agent::runtime
{

/// 工具审批/权限策略；Runner 在执行前询问。
class ToolPolicy
{
  public:
    virtual ~ToolPolicy() = default;

    virtual bool Allow(const core::ToolCall& call, std::string& reason) const = 0;
};

/// 默认放行；只适合可信 host。
class AllowAllToolPolicy final : public ToolPolicy
{
  public:
    bool Allow(const core::ToolCall&, std::string&) const override
    {
        return true;
    }
};

} // namespace agent::runtime
