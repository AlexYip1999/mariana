#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>

#include "agent/plugins/metrics/metrics_collector.h"
#include "agent/runtime/agent.h"
#include "agent/runtime/agent_runner.h"

namespace
{

using namespace std::chrono_literals;
using agent::runtime::RunErrorCode;
using agent::runtime::RunStatus;

agent::core::ChatResponse Answer()
{
    agent::core::ChatResponse response;
    response.ok = true;
    response.content = "done";
    return response;
}

agent::core::ChatResponse Call(std::string id = "call", std::string arguments = "{}")
{
    auto response = Answer();
    response.content.clear();
    response.tool_calls.push_back({std::move(id), "echo", std::move(arguments)});
    return response;
}

class FunctionModel final : public agent::core::ChatModel
{
  public:
    agent::core::ChatResponse Chat(const agent::core::ChatRequest& request) override
    {
        return chat(request);
    }

    std::function<agent::core::ChatResponse(const agent::core::ChatRequest&)> chat = [](const agent::core::ChatRequest&)
    {
        return Answer();
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
        return "test";
    }

    std::string ParametersJson() const override
    {
        return "{}";
    }

    agent::core::ToolResult Execute(const std::string&, const agent::core::CallContext& context) override
    {
        ++calls;
        return execute(context);
    }

    int calls = 0;
    std::function<agent::core::ToolResult(const agent::core::CallContext&)> execute =
        [](const agent::core::CallContext&)
    {
        agent::core::ToolResult result;
        result.content = "echo";
        return result;
    };
};

class TurnObserver final : public agent::core::AgentSystem
{
  public:
    void OnTurnEnd(bool ok, const std::string& error, int steps) override
    {
        ++finishes;
        last_ok = ok;
        last_error = error;
        last_steps = steps;
    }

    int finishes = 0;
    bool last_ok = false;
    std::string last_error;
    int last_steps = 0;
};

class EndOrderObserver final : public agent::core::AgentSystem
{
  public:
    const agent::runtime::AgentSession* session = nullptr;
    std::size_t history_size_at_end = 0;
    std::string run_id;

    void OnTurnEnded(const agent::core::TurnEvent& event) override
    {
        run_id = event.run_id;
        if (session != nullptr)
        {
            history_size_at_end = session->History().messages.size();
        }
    }
};

struct Fixture
{
    Fixture()
    {
        std::string error;
        tools->Register(tool, error);
        dependencies.model = model;
        dependencies.tools = tools;
        dependencies.tool_policy = std::make_shared<agent::runtime::AllowAllToolPolicy>();
        dependencies.context_policy = std::make_shared<agent::runtime::KeepRecentTurnsPolicy>();
        dependencies.tool_output_policy = std::make_shared<agent::runtime::TruncateToolOutputPolicy>();
        dependencies.tool_scheduler = std::make_shared<agent::runtime::InlineToolScheduler>();
        dependencies.systems = systems;
        profile.tool_names = {"echo"};
        profile.system_prompt = "system";
    }

    std::shared_ptr<agent::runtime::AgentRunner> Runner() const
    {
        return std::make_shared<agent::runtime::AgentRunner>(dependencies);
    }

    std::shared_ptr<FunctionModel> model = std::make_shared<FunctionModel>();
    std::shared_ptr<FunctionTool> tool = std::make_shared<FunctionTool>();
    std::shared_ptr<agent::runtime::ToolRegistry> tools = std::make_shared<agent::runtime::ToolRegistry>();
    std::shared_ptr<agent::core::SystemRegistry> systems = std::make_shared<agent::core::SystemRegistry>();
    agent::runtime::RunnerDependencies dependencies;
    agent::runtime::AgentProfile profile;
};

class Gate
{
  public:
    void Enter()
    {
        std::unique_lock lock(m_mutex);
        ++m_arrivals;
        m_condition.notify_all();
        m_condition.wait_for(lock, 5s,
                             [&]
                             {
                                 return m_released;
                             });
    }

    bool WaitFor(int count)
    {
        std::unique_lock lock(m_mutex);
        return m_condition.wait_for(lock, 3s,
                                    [&]
                                    {
                                        return m_arrivals >= count;
                                    });
    }

    void Release()
    {
        std::lock_guard lock(m_mutex);
        m_released = true;
        m_condition.notify_all();
    }

  private:
    std::mutex m_mutex;
    std::condition_variable m_condition;
    int m_arrivals = 0;
    bool m_released = false;
};

} // namespace

TEST_CASE("runner dependencies own their model after assembly scope ends")
{
    std::shared_ptr<agent::runtime::AgentRunner> runner;
    std::weak_ptr<FunctionModel> model;
    agent::runtime::AgentProfile profile;
    {
        Fixture fixture;
        model = fixture.model;
        profile = fixture.profile;
        runner = fixture.Runner();
    }
    CHECK_FALSE(model.expired());
    agent::runtime::AgentSession session(profile, runner);
    CHECK(session.Send("hello").Ok());
    const agent::runtime::RunnerDependencies missing;
    CHECK_THROWS_AS(static_cast<void>(std::make_shared<agent::runtime::AgentRunner>(missing)), std::invalid_argument);
}

TEST_CASE("agent profile defaults stay immutable and executor replacement releases old dependencies")
{
    Fixture first;
    Fixture second;
    auto initial = first.Runner();
    std::weak_ptr<agent::runtime::AgentExecutor> previous = initial;
    agent::runtime::AgentDescriptor descriptor;
    agent::runtime::Agent agent(descriptor, first.profile, {nullptr, initial});
    initial.reset();
    auto current = agent.Profile();
    current.system_prompt = "updated";
    auto replacement = second.Runner();
    REQUIRE(agent.Reconfigure(current, replacement));
    CHECK(previous.expired());
    CHECK(agent.Dependencies().executor == replacement);
    CHECK(agent.Profile().system_prompt == "updated");
    current.system_prompt = "uncommitted";
    CHECK(agent.Profile().system_prompt == "updated");
    CHECK(agent.Send("hello").Ok());
}

TEST_CASE("child service scope owns its parent and runner snapshots tool registration")
{
    auto parent = std::make_shared<agent::runtime::ServiceRegistry>();
    parent->Set<int>(std::make_shared<int>(42));
    agent::runtime::ServiceRegistry child(parent);
    parent.reset();
    REQUIRE(child.Get<int>());
    CHECK(*child.Get<int>() == 42);
    Fixture fixture;
    auto runner = fixture.Runner();
    fixture.tools->Clear();
    int steps = 0;
    fixture.model->chat = [&](const agent::core::ChatRequest&)
    {
        return ++steps == 1 ? Call() : Answer();
    };
    agent::runtime::AgentSession session(fixture.profile, runner, {}, fixture.systems);
    CHECK(session.Send("execute").Ok());
    CHECK(fixture.tool->calls == 1);
}

TEST_CASE("scheduler receives context only after authorization and cancellation checks")
{
    class Scheduler final : public agent::runtime::ToolScheduler
    {
      public:
        agent::core::ToolResult Execute(std::shared_ptr<agent::core::Tool> tool, const std::string& arguments,
                                        const agent::core::CallContext& context) override
        {
            ++calls;
            deadline = context.deadline;
            return tool->Execute(arguments, context);
        }

        int calls = 0;
        std::chrono::steady_clock::time_point deadline;
    };

    class Policy final : public agent::runtime::ToolPolicy
    {
      public:
        bool Allow(const agent::core::ToolCall&, std::string& reason) const override
        {
            if (stop)
            {
                stop->request_stop();
            }
            reason = "test denial";
            return allow;
        }

        bool allow = true;
        std::stop_source* stop = nullptr;
    };

    const int modes[] = {0, 1, 2};
    for (int mode : modes)
    {
        Fixture fixture;
        auto scheduler = std::make_shared<Scheduler>();
        auto policy = std::make_shared<Policy>();
        std::stop_source stop;
        policy->allow = mode != 1;
        policy->stop = mode == 2 ? &stop : nullptr;
        fixture.dependencies.tool_policy = policy;
        fixture.dependencies.tool_scheduler = scheduler;
        int steps = 0;
        fixture.model->chat = [&](const agent::core::ChatRequest&)
        {
            return ++steps == 1 ? Call() : Answer();
        };
        agent::runtime::AgentSession session(fixture.profile, fixture.Runner(), {}, fixture.systems);
        const auto result = session.Send("execute", stop.get_token());
        REQUIRE(result.tool_executions.size() == 1);
        CHECK(scheduler->calls == (mode == 0 ? 1 : 0));
        CHECK(result.tool_executions.front().execution_started == (mode == 0));
        if (mode == 0)
        {
            CHECK(scheduler->deadline != std::chrono::steady_clock::time_point::max());
        }
        else if (mode == 1)
        {
            CHECK(result.tool_executions.front().dispatch_error_code == RunErrorCode::ToolDenied);
        }
        else
        {
            CHECK(result.status == RunStatus::Cancelled);
        }
    }
}

TEST_CASE("finalization rolls back internal failure after execution while keeping its ledger")
{
    class ThrowingOutputPolicy final : public agent::runtime::ToolOutputPolicy
    {
      public:
        std::string Limit(const agent::core::ToolCall&, std::string, agent::runtime::ToolOutputBudget&) const override
        {
            throw std::runtime_error("output policy failure");
        }
    };

    Fixture fixture;
    fixture.dependencies.tool_output_policy = std::make_shared<ThrowingOutputPolicy>();
    fixture.model->chat = [](const agent::core::ChatRequest&)
    {
        return Call();
    };
    auto log = std::make_shared<TurnObserver>();
    fixture.systems->Add(log);
    agent::runtime::AgentSession session(fixture.profile, fixture.Runner(), {}, fixture.systems);
    const auto result = session.Send("execute");
    CHECK(result.error_code == RunErrorCode::InternalError);
    CHECK(session.History().messages.size() == 1);
    REQUIRE(result.tool_executions.size() == 1);
    CHECK(result.tool_executions.front().execution_started);
    CHECK(result.tool_executions.front().result.content == "echo");
    CHECK(log->finishes == 1);
    CHECK_FALSE(log->last_ok);
    CHECK(log->last_steps == result.steps);
}

TEST_CASE("turn end is published after the session commits history")
{
    Fixture fixture;
    fixture.model->chat = [](const agent::core::ChatRequest&)
    {
        return Answer();
    };
    auto observer = std::make_shared<EndOrderObserver>();
    agent::runtime::AgentSession session(fixture.profile, fixture.Runner(), {}, fixture.systems);
    observer->session = &session;
    fixture.systems->Add(observer);
    const auto result = session.Send("execute");
    REQUIRE(result.Ok());
    CHECK(result.run_id == observer->run_id);
    CHECK(observer->history_size_at_end == 3);
}

TEST_CASE("session preflight rejects unknown tools without starting a turn")
{
    Fixture fixture;
    fixture.profile.tool_names = {"missing"};
    fixture.model->chat = [](const agent::core::ChatRequest&)
    {
        return Answer();
    };
    auto log = std::make_shared<TurnObserver>();
    fixture.systems->Add(log);
    agent::runtime::AgentSession session(fixture.profile, fixture.Runner(), {}, fixture.systems);
    const auto result = session.Send("execute");
    CHECK(result.error_code == RunErrorCode::InvalidConfiguration);
    CHECK(log->finishes == 0);
    CHECK(session.Records().empty());
    CHECK(session.History().messages.size() == 1);
}

TEST_CASE("same session rejects concurrent mutations while independent sessions run concurrently")
{
    Fixture first;
    Fixture second;
    Gate gate;
    first.model->chat = second.model->chat = [&](const agent::core::ChatRequest&)
    {
        gate.Enter();
        return Answer();
    };
    agent::runtime::AgentSession one(first.profile, first.Runner(), {}, first.systems);
    agent::runtime::AgentSession two(second.profile, second.Runner(), {}, second.systems);
    auto first_run = std::async(std::launch::async,
                                [&]
                                {
                                    return one.Send("one");
                                });
    CHECK(gate.WaitFor(1));
    const auto busy = one.Send("rejected");
    CHECK(busy.status == RunStatus::Busy);
    CHECK(busy.error_code == RunErrorCode::Busy);
    CHECK_FALSE(one.ClearHistory());
    CHECK_FALSE(one.SetProfile(first.profile));
    CHECK_FALSE(one.Reconfigure(first.profile, first.Runner()));
    CHECK(one.History().messages.size() == 1);
    auto second_run = std::async(std::launch::async,
                                 [&]
                                 {
                                     return two.Send("two");
                                 });
    CHECK(gate.WaitFor(2));
    gate.Release();
    CHECK(first_run.get().Ok());
    CHECK(second_run.get().Ok());
    CHECK(one.Records().size() == 1);
    CHECK(two.Records().size() == 1);
    CHECK(one.Id() != two.Id());
}

TEST_CASE("same thread reentry is busy and does not deadlock snapshots")
{
    Fixture fixture;
    agent::runtime::AgentSession session(fixture.profile, fixture.Runner(), {}, fixture.systems);
    fixture.model->chat = [&](const agent::core::ChatRequest&)
    {
        CHECK(session.Send("reentry").status == RunStatus::Busy);
        CHECK_FALSE(session.ClearHistory());
        CHECK(session.History().messages.size() == 1);
        return Answer();
    };
    CHECK(session.Send("outer").Ok());
}

TEST_CASE("session preserves correlation and committed history when a custom executor throws")
{
    class ThrowingExecutor final : public agent::runtime::AgentExecutor
    {
      public:
        agent::runtime::RunOutcome RunTurn(const agent::runtime::AgentProfile&,
                                           agent::runtime::TurnWorkspace& workspace, const std::string&,
                                           const agent::runtime::RunContext& context) override
        {
            run_id = context.run_id;
            workspace.Conversation().Append(agent::core::ChatRole::User, "not committed");
            throw std::runtime_error("executor failure");
        }

        std::string run_id;
    };

    auto executor = std::make_shared<ThrowingExecutor>();
    agent::runtime::AgentSession session({}, executor);
    const auto result = session.Send("hello");
    CHECK(result.error_code == RunErrorCode::InternalError);
    CHECK(result.run_id == executor->run_id);
    CHECK(result.session_id == session.Id());
    CHECK(session.History().messages.empty());
    REQUIRE(session.Records().size() == 1);
    CHECK(session.Records().front().run_id == executor->run_id);
}

TEST_CASE("cancelled tool side effects remain recorded independently of rolled back history")
{
    Fixture fixture;
    std::stop_source stop;
    agent::core::CallContext model_context;
    fixture.model->chat = [&](const agent::core::ChatRequest& request)
    {
        model_context = request.call;
        return Call();
    };
    fixture.tool->execute = [&](const agent::core::CallContext& context)
    {
        CHECK(context.deadline == model_context.deadline);
        CHECK(context.stop_token == stop.get_token());
        stop.request_stop();
        agent::core::ToolResult result;
        result.content = "side effect completed";
        return result;
    };
    agent::runtime::AgentSession session(fixture.profile, fixture.Runner(), {}, fixture.systems);
    const auto result = session.Send("execute", stop.get_token());
    CHECK(result.status == RunStatus::Cancelled);
    CHECK(result.error_code == RunErrorCode::Cancelled);
    CHECK(result.session_id == session.Id());
    CHECK_FALSE(result.run_id.empty());
    CHECK(session.History().messages.size() == 1);
    REQUIRE(result.tool_executions.size() == 1);
    CHECK(result.tool_executions.front().execution_started);
    CHECK(result.tool_executions.front().result.content == "side effect completed");
    REQUIRE(session.ClearHistory());
    REQUIRE(session.Records().size() == 1);
    CHECK(session.Records().front().tool_executions.size() == 1);
}

TEST_CASE("tool ids replay once per turn and conflicting reuse is rejected")
{
    const bool conflicts[] = {false, true};
    for (bool conflict : conflicts)
    {
        Fixture fixture;
        int requests = 0;
        fixture.model->chat = [&](const agent::core::ChatRequest&)
        {
            ++requests;
            return requests == 1 ? Call() : requests == 2 ? Call("call", conflict ? "[]" : "{}") : Answer();
        };
        agent::runtime::AgentSession session(fixture.profile, fixture.Runner(), {}, fixture.systems);
        const auto result = session.Send("execute");
        CHECK(result.Ok() == !conflict);
        CHECK(result.error_code == (conflict ? RunErrorCode::InvalidToolCall : RunErrorCode::None));
        CHECK(fixture.tool->calls == 1);
        CHECK(result.tool_attempts == 2);
        CHECK(result.tool_executions.size() == 1);
        CHECK(session.History().messages.size() == (conflict ? 4 : 7));
    }
}

TEST_CASE("tool budgets account for cached output and remove unfinished batches")
{
    SUBCASE("attempt limit preserves ledger for the completed part of a batch")
    {
        Fixture fixture;
        fixture.profile.max_tool_calls = 1;
        fixture.model->chat = [](const agent::core::ChatRequest&)
        {
            auto response = Call("first");
            response.tool_calls.push_back({"second", "echo", "{}"});
            return response;
        };
        agent::runtime::AgentSession session(fixture.profile, fixture.Runner(), {}, fixture.systems);
        const auto result = session.Send("execute");
        CHECK(result.status == RunStatus::BudgetExceeded);
        CHECK(result.error_code == RunErrorCode::ToolCallLimit);
        CHECK(result.tool_executions.size() == 1);
        CHECK(fixture.tool->calls == 1);
        REQUIRE(session.History().messages.size() == 2);
        CHECK(session.History().messages.back().role == agent::core::ChatRole::User);
    }
    SUBCASE("replayed result consumes output budget")
    {
        Fixture fixture;
        fixture.profile.max_tool_output_bytes = 8;
        fixture.model->chat = [](const agent::core::ChatRequest&)
        {
            return Call();
        };
        agent::runtime::AgentSession session(fixture.profile, fixture.Runner(), {}, fixture.systems);
        const auto result = session.Send("execute");
        CHECK(result.error_code == RunErrorCode::ToolOutputLimit);
        CHECK(result.tool_attempts == 3);
        CHECK(fixture.tool->calls == 1);
        CHECK(session.History().messages.size() == 6);
    }
}

TEST_CASE("run deadline is shared across model steps and is checked after callbacks")
{
    Fixture fixture;
    agent::core::CallContext first;
    int requests = 0;
    fixture.model->chat = [&](const agent::core::ChatRequest& request)
    {
        if (++requests == 1)
        {
            first = request.call;
            return Call();
        }
        CHECK(request.call.deadline == first.deadline);
        std::this_thread::sleep_until(request.call.deadline + 1ms);
        return Answer();
    };
    agent::runtime::RunOptions options;
    options.timeout = 100ms;
    agent::runtime::AgentSession session(fixture.profile, fixture.Runner(), {}, fixture.systems);
    const auto result = session.Send("execute", options);
    CHECK(result.status == RunStatus::BudgetExceeded);
    CHECK(result.error_code == RunErrorCode::DeadlineExceeded);
    CHECK(result.steps == 2);
    CHECK(result.tool_executions.size() == 1);
    CHECK(session.History().messages.size() == 1);
}

TEST_CASE("structured preflight and model budget failures do not depend on error strings")
{
    Fixture fixture;
    agent::runtime::AgentSession session(fixture.profile, fixture.Runner(), {}, fixture.systems);
    CHECK(session.Send("").error_code == RunErrorCode::InvalidInput);
    agent::runtime::RunOptions options;
    options.timeout = 0ms;
    CHECK(session.Send("hello", options).error_code == RunErrorCode::InvalidConfiguration);
    std::stop_source stop;
    stop.request_stop();
    CHECK(session.Send("hello", stop.get_token()).status == RunStatus::Cancelled);
    fixture.profile.max_steps = 1;
    REQUIRE(session.SetProfile(fixture.profile));
    fixture.model->chat = [](const agent::core::ChatRequest&)
    {
        return Call();
    };
    const auto result = session.Send("execute");
    CHECK(result.status == RunStatus::BudgetExceeded);
    CHECK(result.error_code == RunErrorCode::ModelCallLimit);
    CHECK(result.steps == 1);
}

TEST_CASE("independent sessions keep metrics scoped")
{
    Fixture first;
    Fixture second;
    auto one_metrics = std::make_shared<agent::plugins::metrics::MetricsCollector>();
    auto two_metrics = std::make_shared<agent::plugins::metrics::MetricsCollector>();
    first.systems->Add(one_metrics);
    second.systems->Add(two_metrics);
    agent::runtime::AgentSession one(first.profile, first.Runner(), {}, first.systems);
    agent::runtime::AgentSession two(second.profile, second.Runner(), {}, second.systems);
    CHECK(one.Send("one").Ok());
    CHECK(two.Send("two").Ok());
    CHECK(one.Send("again").Ok());
    CHECK(one_metrics->Snapshot().turns == 2);
    CHECK(two_metrics->Snapshot().turns == 1);
}
