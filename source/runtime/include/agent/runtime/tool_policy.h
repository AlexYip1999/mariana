#pragma once

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "agent/core/message.h"
#include "agent/core/tool.h"

namespace agent::runtime
{

/// 工具审批/权限策略；Runner 在执行前询问。
/// 组合校验（ValidateComposition）另有一次性声明：带副作用/越权访问的工具必须由策略显式放行。
class ToolPolicy
{
  public:
    virtual ~ToolPolicy() = default;

    virtual bool Allow(const core::ToolCall& call, std::string& reason) const = 0;

    /// 是否允许执行 side_effects=true 或 access_scope != Read 的工具。
    /// 默认拒绝（fail-closed）；Host 显式 opt-in 后才能通过组合校验。
    virtual bool AllowsSideEffects() const
    {
        return false;
    }
};

/// 默认放行；只适合可信 host。可信 host 同时接受副作用工具。
class AllowAllToolPolicy final : public ToolPolicy
{
  public:
    bool Allow(const core::ToolCall&, std::string&) const override
    {
        return true;
    }

    bool AllowsSideEffects() const override
    {
        return true;
    }
};

/// 显式 allowlist：只放行列出的工具名；副作用工具需要构造时显式开启。
/// Host（CLI / UE）按自己的工具清单创建即可，避免 AllowAll 的过度授权。
class AllowListToolPolicy final : public ToolPolicy
{
  public:
    explicit AllowListToolPolicy(std::vector<std::string> allowed_tools, bool allow_side_effects = false)
        : m_allowed_tools(std::move(allowed_tools))
        , m_allow_side_effects(allow_side_effects)
    {}

    bool Allow(const core::ToolCall& call, std::string& reason) const override
    {
        const bool listed =
            std::find(m_allowed_tools.begin(), m_allowed_tools.end(), call.name) != m_allowed_tools.end();
        if (!listed)
        {
            reason = "tool is not in the allowlist: " + call.name;
            return false;
        }
        return true;
    }

    bool AllowsSideEffects() const override
    {
        return m_allow_side_effects;
    }

  private:
    std::vector<std::string> m_allowed_tools;
    bool m_allow_side_effects;
};

} // namespace agent::runtime
