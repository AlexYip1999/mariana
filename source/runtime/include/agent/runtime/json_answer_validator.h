#pragma once

#include <string>
#include <vector>

#include "agent/runtime/validator.h"

namespace agent::runtime
{

/// 内置输出契约校验：最终回答必须是合法 JSON 对象，可要求若干顶层 key。
/// 只校验 Completed 状态；issue code 为 invalid_json / not_object / missing_key。
class JsonAnswerValidator final : public Validator
{
  public:
    struct Config
    {
        // 必须存在的顶层字段名
        std::vector<std::string> required_keys;

        // 允许最终回答是任意合法 JSON（默认必须是对象）
        bool allow_non_object = false;
    };

    explicit JsonAnswerValidator(Config config = {});

    ValidationResult Validate(const RunOutcome& outcome, const ValidationContext& context) override;

  private:
    Config m_config;
};

} // namespace agent::runtime
