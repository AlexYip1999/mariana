#pragma once

#include <string>

namespace agent::runtime
{

enum class ToolAccessScope
{
    Read,
    Write,
    Network,
    Host,
};

enum class ToolApprovalMode
{
    NotRequired,
    Required,
    Async,
};

/// 工具治理元数据；与 core::ToolSpec（发给模型的线上格式）分离。
struct ToolDescriptor
{
    // 标识
    std::string name;
    std::string description;

    // 治理属性
    bool side_effects = false;
    ToolAccessScope access_scope = ToolAccessScope::Read;
    ToolApprovalMode approval = ToolApprovalMode::NotRequired;

    // 宿主调度提示：inline / worker / host
    std::string thread_affinity = "worker";
};

} // namespace agent::runtime
