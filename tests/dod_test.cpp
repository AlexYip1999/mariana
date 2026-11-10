#include <doctest/doctest.h>

#include <chrono>
#include <functional>
#include <memory>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "agent/core/call_context.h"
#include "agent/core/chat_model.h"
#include "agent/core/conversation.h"
#include "agent/core/message.h"
#include "agent/core/system_registry.h"
#include "agent/core/tool.h"
#include "agent/runtime/agent_profile.h"
#include "agent/runtime/agent_run.h"
#include "agent/runtime/agent_runner.h"
#include "agent/runtime/run_context.h"
#include "agent/runtime/service_registry.h"
#include "agent/runtime/tool_policy.h"
#include "agent/runtime/tool_registry.h"
#include "agent/runtime/turn_executor.h"
#include "support/runtime_fixture.h"

namespace
{

using namespace std::chrono_literals;
using agent::runtime::RunErrorCode;
using agent::runtime::RunStatus;
using agent::runtime::TurnExecution;

using ChatHandler = std::function<agent::core::ChatResponse(const agent::core::ChatRequest&)>;
using ToolHandler = std::function<agent::core::ToolResult(const agent::core::CallContext&)>;

agent::core::ChatResponse OkResponse(std::string content)
{
    agent::core::ChatResponse response;
    response.ok = true;
    response.content = std::move(content);
    return response;
}

agent::core::ChatResponse SingleToolCall(std::string id)
{
    agent::core::ChatResponse response;
    response.ok = true;
    response.tool_calls.push_back({std::move(id), "echo", "{}"});
    return response;
}

class FunctionModel final : public agent::core::ChatModel
{
  public:
    agent::core::ChatResponse Chat(const agent::core::ChatRequest& request) override
    {
        ++requests;
        return chat(request);
    }

    int requests = 0;
    ChatHandler chat = [](const agent::core::ChatRequest&)
    {
        return OkResponse("done");
    };
};

class FunctionTool final : public agent::core::Tool
{
  public:
    std::string Name() const override
    {
        return "echo";
    }

    std::string Description() const override
    {
        return "test echo tool";
    }

    std::string ParametersJson() const override
    {
        return R"({"type":"object"})";
    }

    agent::core::ToolResult Execute(const std::string&, const agent::core::CallContext& context) override
    {
        ++calls;
        return execute(context);
    }

    int calls = 0;
    ToolHandler execute = [](const agent::core::CallContext&)
    {
        agent::core::ToolResult result;
        result.content = "echo";
        return result;
    };
};

/// 每个用例独立构造 runner 依赖，并通过 ExecuteTurn 走与 AgentSession 相同的提交/回滚路径。
struct TurnFixture
{
    TurnFixture()
    {
        std::string error;
        tools->Register(tool, error);
        services.Set<agent::runtime::ToolRegistry>(tools);
        profile.tool_names = {"echo"};
        profile.max_steps = 4;
    }

    TurnExecution Run(const std::string& input)
    {
        agent::runtime::AgentRunner runner(agent::tests::MakeRunnerDependencies(*model, services));
        const agent::runtime::RunContext context = agent::runtime::RunContext::Create(profile, "dod-session", options);
        agent::core::SystemRegistry systems;

        TurnExecution execution = agent::runtime::ExecuteTurn(profile, staged, runner, input, context, systems);
        if (execution.history)
        {
            // 复制而不是移动：调用方仍要检查 execution.history 的内容。
            conversation = *execution.history;
        }
        else
        {
            // 回滚后 ExecuteTurn 把回滚结果留在 staged 里。
            conversation = std::move(staged);
        }
        return execution;
    }

    // 替身与注册表
    std::shared_ptr<FunctionModel> model = std::make_shared<FunctionModel>();
    std::shared_ptr<FunctionTool> tool = std::make_shared<FunctionTool>();
    std::shared_ptr<agent::runtime::ToolRegistry> tools = std::make_shared<agent::runtime::ToolRegistry>();
    agent::runtime::ServiceRegistry services;

    // 本次运行的 profile 与选项
    agent::runtime::AgentProfile profile;
    agent::runtime::RunOptions options;

    // 提交 / 回滚前后可见的对话历史
    agent::core::Conversation staged;
    agent::core::Conversation conversation;
};

} // namespace

TEST_CASE("cancelled model response rolls back staged history")
{
    TurnFixture fixture;
    std::stop_source stop;
    fixture.model->chat = [&stop](const agent::core::ChatRequest& request)
    {
        CHECK(request.call.stop_token.stop_possible());
        stop.request_stop();
        return OkResponse("must not be committed");
    };
    fixture.options.stop_token = stop.get_token();

    const TurnExecution execution = fixture.Run("hello");

    CHECK(execution.result.status == RunStatus::Cancelled);
    CHECK(execution.result.error_code == RunErrorCode::Cancelled);
    CHECK(execution.result.steps == 1);
    CHECK(execution.result.content.empty());

    CHECK(fixture.model->requests == 1);

    CHECK_FALSE(execution.history.has_value());
    CHECK(fixture.conversation.messages.empty());
}

TEST_CASE("cancel during tool execution keeps ledger and rolls back history")
{
    TurnFixture fixture;
    std::stop_source stop;
    fixture.model->chat = [](const agent::core::ChatRequest&)
    {
        return SingleToolCall("call-1");
    };
    fixture.tool->execute = [&stop](const agent::core::CallContext& context)
    {
        CHECK(context.stop_token.stop_possible());
        stop.request_stop();
        agent::core::ToolResult result;
        result.content = "side effect completed";
        return result;
    };
    fixture.options.stop_token = stop.get_token();

    const TurnExecution execution = fixture.Run("use echo");

    CHECK(execution.result.status == RunStatus::Cancelled);
    CHECK(execution.result.error_code == RunErrorCode::Cancelled);
    CHECK(execution.result.steps == 1);

    CHECK(execution.result.tool_attempts == 1);
    REQUIRE(execution.result.tool_executions.size() == 1);
    CHECK(execution.result.tool_executions.front().execution_started);
    CHECK(execution.result.tool_executions.front().result.content == "side effect completed");
    CHECK(fixture.tool->calls == 1);

    CHECK_FALSE(execution.history.has_value());
    CHECK(fixture.conversation.messages.empty());
}

TEST_CASE("deadline exceeded during model call returns budget error without hanging")
{
    TurnFixture fixture;
    fixture.profile.turn_timeout_ms = 1;
    fixture.model->chat = [](const agent::core::ChatRequest&)
    {
        std::this_thread::sleep_for(20ms);
        return OkResponse("too late");
    };

    const auto started = std::chrono::steady_clock::now();
    const TurnExecution execution = fixture.Run("hello");
    const auto elapsed = std::chrono::steady_clock::now() - started;

    CHECK(execution.result.status == RunStatus::BudgetExceeded);
    CHECK(execution.result.error_code == RunErrorCode::DeadlineExceeded);
    CHECK(execution.result.steps <= 1);
    CHECK(fixture.model->requests <= 1);
    CHECK(execution.result.error.find("deadline") != std::string::npos);

    CHECK_FALSE(execution.history.has_value());
    CHECK(fixture.conversation.messages.empty());

    CHECK(elapsed < 2s);
}

TEST_CASE("model call budget stops at max steps and commits user input")
{
    TurnFixture fixture;
    fixture.profile.max_steps = 2;
    fixture.model->chat = [&fixture](const agent::core::ChatRequest&)
    {
        return SingleToolCall("call-" + std::to_string(fixture.model->requests));
    };

    const TurnExecution execution = fixture.Run("keep looping");

    CHECK(execution.result.status == RunStatus::BudgetExceeded);
    CHECK(execution.result.error_code == RunErrorCode::ModelCallLimit);
    CHECK(execution.result.steps == 2);
    CHECK(execution.result.tool_attempts == 2);
    CHECK(execution.result.tool_executions.size() == 2);

    CHECK(fixture.model->requests == 2);
    CHECK(fixture.tool->calls == 2);

    REQUIRE(execution.history.has_value());
    REQUIRE(execution.history->messages.size() == 5);
    CHECK(execution.history->messages.front().role == agent::core::ChatRole::User);
    CHECK(execution.history->messages.front().content == "keep looping");
    CHECK(execution.history->messages.back().role == agent::core::ChatRole::Tool);
    CHECK(execution.history->messages.back().tool_call_id == "call-2");
    CHECK(fixture.conversation.messages.size() == 5);
}

TEST_CASE("tool call budget rejects extra calls and removes incomplete batch")
{
    TurnFixture fixture;
    fixture.profile.max_tool_calls = 1;
    fixture.model->chat = [](const agent::core::ChatRequest&)
    {
        agent::core::ChatResponse response;
        response.ok = true;
        response.tool_calls.push_back({"first", "echo", "{}"});
        response.tool_calls.push_back({"second", "echo", "{}"});
        return response;
    };

    const TurnExecution execution = fixture.Run("run two tools");

    CHECK(execution.result.status == RunStatus::BudgetExceeded);
    CHECK(execution.result.error_code == RunErrorCode::ToolCallLimit);
    CHECK(execution.result.steps == 1);

    CHECK(execution.result.tool_attempts == 1);
    REQUIRE(execution.result.tool_executions.size() == 1);
    CHECK(execution.result.tool_executions.front().call.id == "first");
    CHECK(execution.result.tool_executions.front().execution_started);
    CHECK(fixture.tool->calls == 1);

    REQUIRE(execution.history.has_value());
    REQUIRE(execution.history->messages.size() == 1);
    CHECK(execution.history->messages.front().role == agent::core::ChatRole::User);
}

TEST_CASE("tool output budget truncates large tool result")
{
    TurnFixture fixture;
    fixture.profile.max_tool_output_bytes = 8;
    fixture.tool->execute = [](const agent::core::CallContext&)
    {
        agent::core::ToolResult result;
        result.content = std::string(100, 'x');
        return result;
    };
    fixture.model->chat = [&fixture](const agent::core::ChatRequest&)
    {
        return fixture.model->requests == 1 ? SingleToolCall("call-1") : OkResponse("done");
    };

    const TurnExecution execution = fixture.Run("read a lot");

    CHECK(execution.result.status == RunStatus::Completed);
    CHECK(execution.result.error_code == RunErrorCode::None);
    CHECK(execution.result.content == "done");
    CHECK(execution.result.steps == 2);

    REQUIRE(execution.result.tool_executions.size() == 1);
    const std::string& truncated = execution.result.tool_executions.front().result.content;
    CHECK(truncated.size() < 100);
    CHECK(truncated.find("[truncated]") != std::string::npos);

    REQUIRE(execution.history.has_value());
    REQUIRE(execution.history->messages.size() == 4);
    CHECK(execution.history->messages[2].role == agent::core::ChatRole::Tool);
    CHECK(execution.history->messages[2].content == truncated);
}

TEST_CASE("pre-cancelled run never calls the model and leaves no history")
{
    TurnFixture fixture;
    std::stop_source stop;
    stop.request_stop();
    fixture.options.stop_token = stop.get_token();

    const TurnExecution execution = fixture.Run("hello");

    CHECK(execution.result.status == RunStatus::Cancelled);
    CHECK(execution.result.error_code == RunErrorCode::Cancelled);
    CHECK(execution.result.steps == 0);
    CHECK(execution.result.tool_attempts == 0);
    CHECK(execution.result.tool_executions.empty());

    CHECK(fixture.model->requests == 0);
    CHECK(fixture.tool->calls == 0);

    CHECK_FALSE(execution.history.has_value());
    CHECK(fixture.conversation.messages.empty());
}

TEST_CASE("tool policy cancellation records a not-started execution")
{
    class CancellingPolicy final : public agent::runtime::ToolPolicy
    {
      public:
        bool Allow(const agent::core::ToolCall&, std::string&) const override
        {
            stop->request_stop();
            return true;
        }

        std::stop_source* stop = nullptr;
    };

    TurnFixture fixture;
    std::stop_source stop;
    auto policy = std::make_shared<CancellingPolicy>();
    policy->stop = &stop;
    fixture.services.Set<agent::runtime::ToolPolicy>(policy);
    fixture.model->chat = [](const agent::core::ChatRequest&)
    {
        return SingleToolCall("call-1");
    };
    fixture.options.stop_token = stop.get_token();

    const TurnExecution execution = fixture.Run("use echo");

    CHECK(execution.result.status == RunStatus::Cancelled);
    CHECK(execution.result.error_code == RunErrorCode::Cancelled);
    CHECK(execution.result.steps == 1);

    CHECK(execution.result.tool_attempts == 1);
    REQUIRE(execution.result.tool_executions.size() == 1);
    CHECK_FALSE(execution.result.tool_executions.front().execution_started);
    CHECK(execution.result.tool_executions.front().dispatch_error_code == RunErrorCode::Cancelled);
    CHECK(fixture.tool->calls == 0);

    CHECK_FALSE(execution.history.has_value());
    CHECK(fixture.conversation.messages.empty());
}
