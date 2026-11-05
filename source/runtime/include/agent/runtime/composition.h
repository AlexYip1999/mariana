#pragma once

#include <string>
#include <vector>

#include "agent/runtime/agent_runner.h"

namespace agent::runtime
{

struct CompositionIssue
{
    // 出问题的字段/依赖名
    std::string field;
    std::string message;
};

struct CompositionValidation
{
    std::vector<CompositionIssue> issues;

    bool Ok() const
    {
        return issues.empty();
    }
};

/// 构建期/运行前组合校验：依赖完整性、tool 元数据、线程亲和与副作用审批。
CompositionValidation ValidateComposition(const RunnerDependencies& dependencies, const AgentProfile& profile);

/// 把 issues 拼成一条可读错误。
std::string DescribeCompositionIssues(const CompositionValidation& validation);

} // namespace agent::runtime
