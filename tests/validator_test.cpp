#include <doctest/doctest.h>

#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "agent/core/chat_model.h"
#include "agent/core/conversation.h"
#include "agent/runtime/agent_runner.h"
#include "agent/runtime/agent_session.h"
#include "agent/runtime/json_answer_validator.h"
#include "agent/runtime/validator.h"
#include "support/runtime_fixture.h"

namespace
{

class FakeChatModel : public agent::core::ChatModel
{
  public:
    agent::core::ChatResponse Chat(const agent::core::ChatRequest& request) override
    {
        requests.push_back(request);
        return response;
    }

    // 请求记录与预设响应
    std::vector<agent::core::ChatRequest> requests;
    agent::core::ChatResponse response;
};

class RecordingValidator final : public agent::runtime::Validator
{
  public:
    agent::runtime::ValidationResult Validate(const agent::runtime::RunOutcome& outcome,
                                              const agent::runtime::ValidationContext& context) override
    {
        outcomes.push_back(outcome);
        contexts.push_back(context);
        return result;
    }

    // 调用记录
    std::vector<agent::runtime::RunOutcome> outcomes;
    std::vector<agent::runtime::ValidationContext> contexts;

    // 预设结果
    agent::runtime::ValidationResult result;
};

class ThrowingValidator final : public agent::runtime::Validator
{
  public:
    agent::runtime::ValidationResult Validate(const agent::runtime::RunOutcome&,
                                              const agent::runtime::ValidationContext&) override
    {
        throw std::runtime_error("validator exploded");
    }
};

agent::runtime::AgentSessionConfig MakeConfig()
{
    agent::runtime::AgentSessionConfig config;
    config.model = "deepseek-flash";
    config.system_prompt = "be brief";
    config.max_tokens = 64;
    return config;
}

agent::runtime::ServiceRegistry MakeServices()
{
    agent::runtime::ServiceRegistry services;
    services.Set<agent::runtime::ToolPolicy>(std::make_shared<agent::runtime::AllowAllToolPolicy>());
    services.Set<agent::runtime::ContextPolicy>(std::make_shared<agent::runtime::KeepRecentTurnsPolicy>());
    services.Set<agent::runtime::ToolOutputPolicy>(std::make_shared<agent::runtime::TruncateToolOutputPolicy>());
    return services;
}

agent::runtime::RunOutcome CompletedAnswer(std::string content)
{
    agent::runtime::RunOutcome outcome;
    outcome.status = agent::runtime::RunStatus::Completed;
    outcome.content = std::move(content);
    return outcome;
}

agent::runtime::ValidationContext MakeValidationContext()
{
    agent::runtime::ValidationContext context;
    context.session_id = "session-1";
    context.run_id = "run-1";
    context.user_input = "hi";
    return context;
}

} // namespace

TEST_CASE("json answer validator accepts valid json objects")
{
    agent::runtime::JsonAnswerValidator::Config config;
    config.required_keys = {"answer", "confidence"};
    agent::runtime::JsonAnswerValidator validator(config);
    const agent::runtime::ValidationContext context = MakeValidationContext();

    const agent::runtime::ValidationResult accepted =
        validator.Validate(CompletedAnswer(R"({"answer":"42","confidence":0.9})"), context);

    CHECK(accepted.ok);
    CHECK(accepted.issues.empty());
    CHECK(accepted.Describe() == "ok");
}

TEST_CASE("json answer validator reports invalid json and non objects")
{
    agent::runtime::JsonAnswerValidator validator;
    const agent::runtime::ValidationContext context = MakeValidationContext();

    const agent::runtime::ValidationResult invalid = validator.Validate(CompletedAnswer("{not json"), context);
    CHECK_FALSE(invalid.ok);
    REQUIRE(invalid.issues.size() == 1);
    CHECK(invalid.issues.front().code == "invalid_json");
    CHECK_FALSE(invalid.issues.front().message.empty());

    const agent::runtime::ValidationResult array_answer = validator.Validate(CompletedAnswer("[1, 2, 3]"), context);
    CHECK_FALSE(array_answer.ok);
    REQUIRE(array_answer.issues.size() == 1);
    CHECK(array_answer.issues.front().code == "not_object");

    const agent::runtime::ValidationResult number_answer = validator.Validate(CompletedAnswer("42"), context);
    CHECK_FALSE(number_answer.ok);
    REQUIRE(number_answer.issues.size() == 1);
    CHECK(number_answer.issues.front().code == "not_object");
}

TEST_CASE("json answer validator allows non object answers when configured")
{
    agent::runtime::JsonAnswerValidator::Config config;
    config.allow_non_object = true;
    agent::runtime::JsonAnswerValidator validator(config);
    const agent::runtime::ValidationContext context = MakeValidationContext();

    CHECK(validator.Validate(CompletedAnswer("[1, 2, 3]"), context).ok);
    CHECK(validator.Validate(CompletedAnswer(R"("plain text")"), context).ok);

    const agent::runtime::ValidationResult invalid = validator.Validate(CompletedAnswer("not json"), context);
    CHECK_FALSE(invalid.ok);
    REQUIRE(invalid.issues.size() == 1);
    CHECK(invalid.issues.front().code == "invalid_json");
}

TEST_CASE("json answer validator reports every missing required key")
{
    agent::runtime::JsonAnswerValidator::Config config;
    config.required_keys = {"answer", "confidence", "sources"};
    agent::runtime::JsonAnswerValidator validator(config);
    const agent::runtime::ValidationContext context = MakeValidationContext();

    const agent::runtime::ValidationResult result = validator.Validate(CompletedAnswer(R"({"answer":"42"})"), context);

    CHECK_FALSE(result.ok);
    REQUIRE(result.issues.size() == 2);
    CHECK(result.issues[0].code == "missing_key");
    CHECK(result.issues[0].message.find("confidence") != std::string::npos);
    CHECK(result.issues[1].code == "missing_key");
    CHECK(result.issues[1].message.find("sources") != std::string::npos);
    CHECK(result.Describe().find("missing_key: missing required key: confidence") != std::string::npos);
}

TEST_CASE("json answer validator skips non completed outcomes")
{
    agent::runtime::JsonAnswerValidator::Config config;
    config.required_keys = {"answer"};
    agent::runtime::JsonAnswerValidator validator(config);
    const agent::runtime::ValidationContext context = MakeValidationContext();

    agent::runtime::RunOutcome failed;
    failed.status = agent::runtime::RunStatus::Failed;
    failed.error = "model down";
    failed.content = "not json";
    CHECK(validator.Validate(failed, context).ok);

    agent::runtime::RunOutcome cancelled;
    cancelled.status = agent::runtime::RunStatus::Cancelled;
    CHECK(validator.Validate(cancelled, context).ok);

    agent::runtime::RunOutcome budgeted;
    budgeted.status = agent::runtime::RunStatus::BudgetExceeded;
    CHECK(validator.Validate(budgeted, context).ok);
}

TEST_CASE("validation result describe joins issues")
{
    agent::runtime::ValidationResult result;
    CHECK(result.Describe() == "ok");

    result.ok = false;
    result.issues = {
        {
            "missing_key",
            "missing required key: answer",
        },
        {
            "invalid_json",
            "bad payload",
        },
    };

    CHECK(result.Describe() == "missing_key: missing required key: answer; invalid_json: bad payload");
}

TEST_CASE("runner validates the final answer before committing it")
{
    FakeChatModel model;
    model.response.ok = true;
    model.response.content = R"({"answer":"42"})";

    auto services = MakeServices();
    auto dependencies = agent::tests::MakeRunnerDependencies(model, services);
    auto validator = std::make_shared<RecordingValidator>();
    dependencies.validator = validator;
    agent::runtime::AgentRunner runner(dependencies);
    agent::runtime::AgentSession session(MakeConfig(), agent::tests::BorrowForTest(runner));

    const agent::runtime::AgentRunResult result = session.Send("hi");

    // 结果与历史：合法回答正常提交。
    CHECK(result.Ok());
    CHECK(result.content == R"({"answer":"42"})");
    CHECK(result.steps == 1);

    const agent::core::Conversation history = session.History();
    REQUIRE(history.messages.size() == 3);
    CHECK(history.messages.back().role == agent::core::ChatRole::Assistant);
    CHECK(history.messages.back().content == R"({"answer":"42"})");

    // 校验输入：候选 RunOutcome 与上下文。
    REQUIRE(validator->outcomes.size() == 1);
    const agent::runtime::RunOutcome& candidate = validator->outcomes.front();
    CHECK(candidate.status == agent::runtime::RunStatus::Completed);
    CHECK(candidate.error_code == agent::runtime::RunErrorCode::None);
    CHECK(candidate.content == model.response.content);
    CHECK(candidate.steps == result.steps);

    REQUIRE(validator->contexts.size() == 1);
    CHECK(validator->contexts.front().session_id == session.Id());
    CHECK(validator->contexts.front().run_id == result.run_id);
    CHECK(validator->contexts.front().user_input == "hi");
}

TEST_CASE("runner rejects invalid final answer without committing assistant message")
{
    FakeChatModel model;
    model.response.ok = true;
    model.response.content = "not a json answer";

    auto services = MakeServices();
    auto dependencies = agent::tests::MakeRunnerDependencies(model, services);
    dependencies.validator = std::make_shared<agent::runtime::JsonAnswerValidator>();
    agent::runtime::AgentRunner runner(dependencies);
    agent::runtime::AgentSession session(MakeConfig(), agent::tests::BorrowForTest(runner));

    const agent::runtime::AgentRunResult result = session.Send("hi");

    CHECK_FALSE(result.Ok());
    CHECK(result.status == agent::runtime::RunStatus::Failed);
    CHECK(result.error_code == agent::runtime::RunErrorCode::ValidationFailed);
    CHECK(result.error.find("output validation failed") != std::string::npos);
    CHECK(result.error.find("invalid_json") != std::string::npos);
    CHECK(result.content.empty());
    CHECK(result.steps == 1);

    // 非法 assistant 不提交；用户输入保留在历史中。
    const agent::core::Conversation history = session.History();
    REQUIRE(history.messages.size() == 2);
    CHECK(history.messages[0].role == agent::core::ChatRole::System);
    CHECK(history.messages[1].role == agent::core::ChatRole::User);
    CHECK(history.messages[1].content == "hi");

    REQUIRE(session.Records().size() == 1);
    CHECK(session.Records().front().error_code == agent::runtime::RunErrorCode::ValidationFailed);
}

TEST_CASE("runner reports missing required keys from final answer validation")
{
    FakeChatModel model;
    model.response.ok = true;
    model.response.content = R"({"foo":1})";

    agent::runtime::JsonAnswerValidator::Config config;
    config.required_keys = {"answer"};
    auto services = MakeServices();
    auto dependencies = agent::tests::MakeRunnerDependencies(model, services);
    dependencies.validator = std::make_shared<agent::runtime::JsonAnswerValidator>(config);
    agent::runtime::AgentRunner runner(dependencies);
    agent::runtime::AgentSession session(MakeConfig(), agent::tests::BorrowForTest(runner));

    const agent::runtime::AgentRunResult result = session.Send("hi");

    CHECK(result.error_code == agent::runtime::RunErrorCode::ValidationFailed);
    CHECK(result.error.find("missing_key") != std::string::npos);
    CHECK(result.error.find("answer") != std::string::npos);
    CHECK(session.History().messages.size() == 2);
}

TEST_CASE("validator exceptions become internal errors")
{
    FakeChatModel model;
    model.response.ok = true;
    model.response.content = "{}";

    auto services = MakeServices();
    auto dependencies = agent::tests::MakeRunnerDependencies(model, services);
    dependencies.validator = std::make_shared<ThrowingValidator>();
    agent::runtime::AgentRunner runner(dependencies);
    agent::runtime::AgentSession session(MakeConfig(), agent::tests::BorrowForTest(runner));

    const agent::runtime::AgentRunResult result = session.Send("hi");

    CHECK_FALSE(result.Ok());
    CHECK(result.error_code == agent::runtime::RunErrorCode::InternalError);
    CHECK(result.error.find("validator threw") != std::string::npos);
    CHECK(result.error.find("validator exploded") != std::string::npos);

    // InternalError 回滚本轮：历史只剩 system prompt。
    const agent::core::Conversation history = session.History();
    REQUIRE(history.messages.size() == 1);
    CHECK(history.messages.front().role == agent::core::ChatRole::System);
}

TEST_CASE("validator is not called for failed model responses")
{
    FakeChatModel model;
    model.response.ok = false;
    model.response.error = "model down";

    auto services = MakeServices();
    auto dependencies = agent::tests::MakeRunnerDependencies(model, services);
    auto validator = std::make_shared<RecordingValidator>();
    dependencies.validator = validator;
    agent::runtime::AgentRunner runner(dependencies);
    agent::runtime::AgentSession session(MakeConfig(), agent::tests::BorrowForTest(runner));

    const agent::runtime::AgentRunResult result = session.Send("hi");

    CHECK(result.error_code == agent::runtime::RunErrorCode::ModelFailure);
    CHECK(validator->outcomes.empty());
}
