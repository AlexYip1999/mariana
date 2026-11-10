#include "agent/runtime/json_answer_validator.h"

#include <exception>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace agent::runtime
{

namespace
{

ValidationResult Rejected(std::string code, std::string message)
{
    ValidationResult result;
    result.ok = false;
    result.issues.push_back({std::move(code), std::move(message)});
    return result;
}

} // namespace

JsonAnswerValidator::JsonAnswerValidator(Config config)
    : m_config(std::move(config))
{}

ValidationResult JsonAnswerValidator::Validate(const RunOutcome& outcome, const ValidationContext& context)
{
    (void)context;

    // 只校验成功完成的最终回答；失败/取消/预算等结果由 Runner 的既有错误语义处理。
    if (outcome.status != RunStatus::Completed)
    {
        return {};
    }

    nlohmann::json parsed;
    try
    {
        parsed = nlohmann::json::parse(outcome.content);
    }
    catch (const std::exception& exception)
    {
        return Rejected("invalid_json", std::string("final answer is not valid JSON: ") + exception.what());
    }
    catch (...)
    {
        return Rejected("invalid_json", "final answer is not valid JSON");
    }

    if (!parsed.is_object() && !m_config.allow_non_object)
    {
        return Rejected("not_object", "final answer must be a JSON object");
    }

    // 逐个检查必需顶层字段；全部命中才通过。
    ValidationResult result;
    for (const std::string& key : m_config.required_keys)
    {
        if (!parsed.contains(key))
        {
            result.issues.push_back({"missing_key", "missing required key: " + key});
        }
    }
    result.ok = result.issues.empty();
    return result;
}

} // namespace agent::runtime
