#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/hosts/reference/reference_host.h"
#include "agent/runtime/tool_policy.h"

namespace
{

using namespace std::chrono_literals;

agent::transport::HttpResponse TextResponse(std::string content)
{
    nlohmann::json body;
    body["choices"] = nlohmann::json::array();
    body["choices"].push_back({{"message", {{"content", std::move(content)}}}, {"finish_reason", "stop"}});
    agent::transport::HttpResponse response;
    response.status = 200;
    response.body = body.dump();
    return response;
}

agent::transport::HttpResponse ToolCallResponse(std::string name, std::string arguments)
{
    nlohmann::json function;
    function["name"] = std::move(name);
    function["arguments"] = std::move(arguments);
    nlohmann::json call;
    call["id"] = "call_1";
    call["type"] = "function";
    call["function"] = std::move(function);

    nlohmann::json message;
    message["content"] = "";
    message["tool_calls"] = nlohmann::json::array({call});
    nlohmann::json body;
    body["choices"] = nlohmann::json::array();
    body["choices"].push_back({{"message", std::move(message)}, {"finish_reason", "tool_calls"}});
    agent::transport::HttpResponse response;
    response.status = 200;
    response.body = body.dump();
    return response;
}

class ScriptedTransport final : public agent::transport::HttpTransport
{
  public:
    void Push(agent::transport::HttpResponse response)
    {
        std::lock_guard lock(m_mutex);
        m_responses.push_back(std::move(response));
    }

    void SetDelayMilliseconds(int delay_ms)
    {
        m_delay_ms = delay_ms;
    }

    agent::transport::HttpResponse PostJson(const agent::transport::HttpRequest&) override
    {
        const int delay_ms = m_delay_ms;
        if (delay_ms > 0)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
        }
        std::lock_guard lock(m_mutex);
        if (m_responses.empty())
        {
            agent::transport::HttpResponse response;
            response.status = 500;
            response.body = "{}";
            return response;
        }
        agent::transport::HttpResponse next = m_responses.front();
        m_responses.erase(m_responses.begin());
        return next;
    }

  private:
    std::mutex m_mutex;
    std::vector<agent::transport::HttpResponse> m_responses;
    std::atomic<int> m_delay_ms{0};
};

class RecordingTool final : public agent::core::Tool
{
  public:
    std::string Name() const override
    {
        return "echo";
    }

    std::string Description() const override
    {
        return "echo arguments";
    }

    std::string ParametersJson() const override
    {
        return R"({"type":"object","properties":{"text":{"type":"string"}}})";
    }

    agent::core::ToolResult Execute(const std::string& arguments, const agent::core::CallContext&) override
    {
        std::lock_guard lock(m_mutex);
        ++m_calls;
        m_execute_thread = std::this_thread::get_id();
        agent::core::ToolResult result;
        result.content = "echo:" + arguments;
        return result;
    }

    int Calls() const
    {
        std::lock_guard lock(m_mutex);
        return m_calls;
    }

    std::thread::id ExecuteThread() const
    {
        std::lock_guard lock(m_mutex);
        return m_execute_thread;
    }

  private:
    mutable std::mutex m_mutex;
    int m_calls = 0;
    std::thread::id m_execute_thread;
};

agent::model::OpenAiConfig MakeModelConfig()
{
    agent::model::OpenAiConfig config;
    config.api_key = "test-key";
    return config;
}

agent::runtime::AgentProfile MakeProfile()
{
    agent::runtime::AgentProfile profile;
    profile.model = "deepseek-flash";
    profile.system_prompt = "be brief";
    return profile;
}

agent::runtime::AgentProfile MakeProfileWithTools(std::vector<std::string> tool_names)
{
    agent::runtime::AgentProfile profile = MakeProfile();
    profile.tool_names = std::move(tool_names);
    return profile;
}

void PumpMainThread(agent::hosts::reference::ReferenceHost& host, const std::atomic<bool>& done)
{
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!done.load() && std::chrono::steady_clock::now() < deadline)
    {
        host.DrainMainThreadTasks();
        std::this_thread::sleep_for(1ms);
    }
}

} // namespace

TEST_CASE("reference host delivers async result on the main thread queue")
{
    auto transport = std::make_shared<ScriptedTransport>();
    transport->Push(TextResponse("hello"));

    agent::hosts::reference::ReferenceHost host(transport, MakeModelConfig());
    std::string error;
    REQUIRE(host.AddAgent("npc-1", MakeProfile(), nullptr, nullptr, error));

    const std::thread::id main_thread = std::this_thread::get_id();
    std::atomic<bool> done{false};
    std::thread::id callback_thread;
    std::string callback_run_id;
    std::string callback_content;
    agent::runtime::AgentRunResult callback_result;

    agent::hosts::reference::RunHandle handle;
    handle = host.SendAsync("npc-1", "hi",
                            [&](agent::runtime::AgentRunResult result)
                            {
                                callback_thread = std::this_thread::get_id();
                                callback_run_id = result.run_id;
                                callback_content = result.content;
                                callback_result = std::move(result);
                                done.store(true);
                            });
    REQUIRE_FALSE(handle.Empty());
    PumpMainThread(host, done);
    REQUIRE(done.load());
    INFO("status=" << agent::runtime::ToString(callback_result.status) << " error=" << callback_result.error);
    CHECK(callback_thread == main_thread);
    CHECK(callback_run_id == handle.run_id);
    CHECK(callback_content == "hello");
}

TEST_CASE("reference host executes tools on the main thread")
{
    auto transport = std::make_shared<ScriptedTransport>();
    transport->Push(ToolCallResponse("echo", R"({"text":"hi"})"));
    transport->Push(TextResponse("done"));

    agent::hosts::reference::ReferenceHost host(transport, MakeModelConfig());
    auto tool = std::make_shared<RecordingTool>();
    agent::runtime::ToolDescriptor descriptor;
    descriptor.name = "echo";
    descriptor.thread_affinity = "host";
    std::string error;
    REQUIRE(host.RegisterTool(tool, descriptor, error));

    auto policy = std::make_shared<agent::runtime::AllowListToolPolicy>(std::vector<std::string>{"echo"});
    REQUIRE(host.AddAgent("npc-1", MakeProfileWithTools({"echo"}), policy, nullptr, error));

    const std::thread::id main_thread = std::this_thread::get_id();
    std::atomic<bool> done{false};
    agent::runtime::AgentRunResult callback_result;
    host.SendAsync("npc-1", "use the tool",
                   [&](agent::runtime::AgentRunResult result)
                   {
                       callback_result = std::move(result);
                       done.store(true);
                   });
    PumpMainThread(host, done);
    REQUIRE(done.load());
    CHECK(callback_result.Ok());
    CHECK(tool->Calls() == 1);
    CHECK(tool->ExecuteThread() == main_thread);
}

TEST_CASE("reference host cancel reports cancelled with the host assigned run id")
{
    auto transport = std::make_shared<ScriptedTransport>();
    transport->SetDelayMilliseconds(200);
    transport->Push(TextResponse("late"));

    agent::hosts::reference::ReferenceHost host(transport, MakeModelConfig());
    std::string error;
    REQUIRE(host.AddAgent("npc-1", MakeProfile(), nullptr, nullptr, error));

    std::atomic<bool> done{false};
    agent::runtime::AgentRunResult callback_result;
    agent::hosts::reference::RunHandle handle;
    handle = host.SendAsync("npc-1", "hi",
                            [&](agent::runtime::AgentRunResult result)
                            {
                                callback_result = std::move(result);
                                done.store(true);
                            });
    REQUIRE_FALSE(handle.Empty());
    CHECK(host.Cancel(handle));
    PumpMainThread(host, done);
    REQUIRE(done.load());
    CHECK(callback_result.status == agent::runtime::RunStatus::Cancelled);
    CHECK(callback_result.run_id == handle.run_id);
}

TEST_CASE("reference host rejects a concurrent turn on the same agent")
{
    auto transport = std::make_shared<ScriptedTransport>();
    transport->SetDelayMilliseconds(200);
    transport->Push(TextResponse("first"));
    transport->Push(TextResponse("second"));

    agent::hosts::reference::ReferenceHost host(transport, MakeModelConfig());
    std::string error;
    REQUIRE(host.AddAgent("npc-1", MakeProfile(), nullptr, nullptr, error));

    std::atomic<bool> first_done{false};
    std::atomic<bool> second_done{false};
    agent::runtime::AgentRunResult second_result;
    host.SendAsync("npc-1", "first",
                   [&](agent::runtime::AgentRunResult)
                   {
                       first_done.store(true);
                   });
    const agent::hosts::reference::RunHandle rejected = host.SendAsync("npc-1", "second",
                                                                       [&](agent::runtime::AgentRunResult result)
                                                                       {
                                                                           second_result = std::move(result);
                                                                           second_done.store(true);
                                                                       });
    CHECK(rejected.Empty());
    PumpMainThread(host, second_done);
    REQUIRE(second_done.load());
    CHECK(second_result.status == agent::runtime::RunStatus::Busy);
    PumpMainThread(host, first_done);
    CHECK(first_done.load());
}
