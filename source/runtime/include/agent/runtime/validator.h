#pragma once

#include <string>
#include <vector>

#include "agent/runtime/agent_run.h"

namespace agent::runtime
{

/// 单条输出契约问题：code 供程序分支，message 供人读。
struct ValidationIssue
{
    std::string code;
    std::string message;
};

/// 输出契约校验结果：ok=true 表示通过；否则由 issues 说明原因。
struct ValidationResult
{
    bool ok = true;
    std::vector<ValidationIssue> issues;

    /// 通过时返回 "ok"；失败时拼成 "code: message; code2: message2"。
    std::string Describe() const
    {
        if (ok)
        {
            return "ok";
        }

        std::string text;
        for (const ValidationIssue& issue : issues)
        {
            if (!text.empty())
            {
                text += "; ";
            }
            text += issue.code + ": " + issue.message;
        }
        return text;
    }
};

/// 校验上下文：本轮关联与用户输入，供 Validator 做业务判断。
struct ValidationContext
{
    std::string session_id;
    std::string run_id;
    std::string user_input;
};

/// 输出契约校验接口：Runner 只在最终回答写入对话前调用；实现不得修改 RunOutcome/历史。
class Validator
{
  public:
    virtual ~Validator() = default;

    virtual ValidationResult Validate(const RunOutcome& outcome, const ValidationContext& context) = 0;
};

} // namespace agent::runtime
