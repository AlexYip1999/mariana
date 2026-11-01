#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "agent/core/system_registry.h"
#include "agent/hosts/cli/cli_app.h"
#include "agent/hosts/cli/cli_config.h"
#include "agent/hosts/cli/interactive_app.h"
#include "agent/model/openai_chat_model.h"
#include "agent/plugins/metrics/metrics_collector.h"
#include "agent/plugins/tools/read_file_tool.h"
#include "agent/runtime/agent_runner.h"
#include "agent/runtime/tool_registry.h"
#include "support/fake_http_transport.h"

namespace
{

class ScopedWorkingDirectory
{
  public:
    explicit ScopedWorkingDirectory(const std::filesystem::path& path)
        : m_previous(std::filesystem::current_path())
    {
        std::filesystem::current_path(path);
    }

    ~ScopedWorkingDirectory()
    {
        std::error_code error;
        std::filesystem::current_path(m_previous, error);
    }

    ScopedWorkingDirectory(const ScopedWorkingDirectory&) = delete;
    ScopedWorkingDirectory& operator=(const ScopedWorkingDirectory&) = delete;

  private:
    std::filesystem::path m_previous;
};

agent::hosts::cli::CliConfig MakeCliConfig()
{
    agent::hosts::cli::CliConfig config;
    config.model = "deepseek-flash";
    config.system_prompt = "be brief";
    config.max_tokens = 64;
    return config;
}

agent::model::OpenAiConfig MakeOpenAiConfig()
{
    agent::model::OpenAiConfig config;
    config.endpoint = "https://api.deepseek.com/chat/completions";
    config.model = "deepseek-flash";
    config.api_key = "test-key";
    return config;
}

agent::transport::HttpResponse MakeChatResponse(const std::string& content)
{
    nlohmann::json parsed;
    parsed["choices"] = nlohmann::json::array();
    parsed["choices"].push_back({{"message", {{"content", content}}}});
    agent::transport::HttpResponse response;
    response.status = 200;
    response.body = parsed.dump();
    return response;
}

class FakeChatModel : public agent::core::ChatModel
{
  public:
    std::vector<agent::core::ChatRequest> requests;
    std::vector<std::string> replies;

    agent::core::ChatResponse Chat(const agent::core::ChatRequest& request) override
    {
        requests.push_back(request);
        agent::core::ChatResponse response;
        response.ok = true;
        if (!replies.empty())
        {
            response.content = replies.front();
            replies.erase(replies.begin());
        }
        return response;
    }
};

class TestExecutor
{
  public:
    TestExecutor(agent::core::ChatModel& model, agent::runtime::ServiceRegistry& services)
        : m_tools(services.Get<agent::runtime::ToolRegistry>())
        , m_systems(services.Get<agent::core::SystemRegistry>())
        , m_executor(model, m_tools.get(), m_systems.get())
    {}

    agent::runtime::AgentExecutor& Get()
    {
        return m_executor;
    }

  private:
    std::shared_ptr<agent::runtime::ToolRegistry> m_tools;
    std::shared_ptr<agent::core::SystemRegistry> m_systems;
    agent::runtime::AgentRunner m_executor;
};

} // namespace

TEST_CASE("parses cli config overrides")
{
    const agent::hosts::cli::ConfigLoadResult result = agent::hosts::cli::ParseCliConfig(
        R"({"endpoint":"https://example.test/chat","model":"deepseek-reasoner","timeout_ms":5000,"temperature":0.3})");

    CHECK(result.ok);
    CHECK(result.config.endpoint == "https://example.test/chat");
    CHECK(result.config.model == "deepseek-reasoner");
    CHECK(result.config.timeout_ms == 5000);
    CHECK(result.config.temperature == doctest::Approx(0.3));
    CHECK(result.config.api_key_env == "DEEPSEEK_API_KEY");
    CHECK(result.config.provider == "deepseek");
    CHECK(result.config.cost_currency == "CNY");
    CHECK(result.config.max_context_chars == 120000);
    CHECK(result.config.tool_names == std::vector<std::string>{"read_file"});
}

TEST_CASE("parses tool names")
{
    const agent::hosts::cli::ConfigLoadResult result =
        agent::hosts::cli::ParseCliConfig(R"({"tool_names":["read_file","search_web"]})");

    CHECK(result.ok);
    REQUIRE(result.config.tool_names.size() == 2);
    CHECK(result.config.tool_names[0] == "read_file");
    CHECK(result.config.tool_names[1] == "search_web");
}

TEST_CASE("rejects insecure endpoint")
{
    const agent::hosts::cli::ConfigLoadResult result =
        agent::hosts::cli::ParseCliConfig(R"({"endpoint":"http://api.example.test/chat"})");

    CHECK_FALSE(result.ok);
    CHECK(result.error.find("https") != std::string::npos);
}

TEST_CASE("rejects invalid tool names")
{
    const agent::hosts::cli::ConfigLoadResult result = agent::hosts::cli::ParseCliConfig(R"({"tool_names":[1]})");

    CHECK_FALSE(result.ok);
    CHECK(result.error.find("tool_names") != std::string::npos);
}

TEST_CASE("rejects empty provider")
{
    const agent::hosts::cli::ConfigLoadResult result = agent::hosts::cli::ParseCliConfig(R"({"provider":""})");

    CHECK_FALSE(result.ok);
    CHECK(result.error.find("provider") != std::string::npos);
}

TEST_CASE("rejects unsupported cost currency")
{
    const agent::hosts::cli::ConfigLoadResult result = agent::hosts::cli::ParseCliConfig(R"({"cost_currency":"EUR"})");

    CHECK_FALSE(result.ok);
    CHECK(result.error.find("cost_currency") != std::string::npos);
}

TEST_CASE("merges config over a base")
{
    agent::hosts::cli::CliConfig base;
    base.model = "shared-model";
    base.max_tokens = 111;

    const agent::hosts::cli::ConfigLoadResult result = agent::hosts::cli::ParseCliConfig(R"({"max_tokens":222})", base);

    CHECK(result.ok);
    CHECK(result.config.model == "shared-model");
    CHECK(result.config.max_tokens == 222);
    CHECK(result.config.endpoint == base.endpoint);
}

TEST_CASE("rejects invalid cli config")
{
    const agent::hosts::cli::ConfigLoadResult result = agent::hosts::cli::ParseCliConfig("not-json");

    CHECK_FALSE(result.ok);
    CHECK_FALSE(result.error.empty());
}

TEST_CASE("rejects api keys stored in config")
{
    const agent::hosts::cli::ConfigLoadResult result = agent::hosts::cli::ParseCliConfig(R"({"api_key":"sk-test"})");

    CHECK_FALSE(result.ok);
    CHECK(result.error.find("api_key") != std::string::npos);
}

TEST_CASE("loads config.local.json from working directory")
{
    const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const std::filesystem::path workspace =
        std::filesystem::temp_directory_path() / ("agent-core-cli-cwd-" + std::to_string(stamp));
    std::filesystem::create_directories(workspace);

    {
        ScopedWorkingDirectory scoped(workspace);
        {
            std::ofstream file("config.local.json");
            file << R"({"model":"unit-test-model"})";
        }

        const agent::hosts::cli::DefaultConfigResult result = agent::hosts::cli::LoadDefaultCliConfig();

        CHECK(result.ok);
        CHECK(result.found);
        CHECK(result.path == "config.local.json");
        CHECK(result.config.model == "unit-test-model");
    }

    std::filesystem::remove_all(workspace);
}

TEST_CASE("loads shared config from exe directory")
{
    const std::filesystem::path exe_dir = std::filesystem::temp_directory_path() / "agent-core-shared-config-test";
    std::filesystem::remove_all(exe_dir);
    std::filesystem::create_directories(exe_dir);
    {
        std::ofstream file(exe_dir / "config.json");
        file << R"({"model":"shared-model","max_tokens":111})";
    }

    const agent::hosts::cli::DefaultConfigResult result =
        agent::hosts::cli::LoadDefaultCliConfig((exe_dir / "agent-cli.exe").string());
    std::filesystem::remove_all(exe_dir);

    CHECK(result.ok);
    CHECK(result.found);
    CHECK(result.config.model == "shared-model");
    CHECK(result.config.max_tokens == 111);
    CHECK(result.config.endpoint == agent::hosts::cli::CliConfig{}.endpoint);
}

TEST_CASE("local config overrides shared fields")
{
    const std::filesystem::path exe_dir = std::filesystem::temp_directory_path() / "agent-core-merge-config-test";
    std::filesystem::remove_all(exe_dir);
    std::filesystem::create_directories(exe_dir);
    {
        std::ofstream file(exe_dir / "config.json");
        file << R"({"model":"shared-model","max_tokens":111})";
    }
    {
        std::ofstream file(exe_dir / "config.local.json");
        file << R"({"max_tokens":222})";
    }

    const agent::hosts::cli::DefaultConfigResult result =
        agent::hosts::cli::LoadDefaultCliConfig((exe_dir / "agent-cli.exe").string());
    std::filesystem::remove_all(exe_dir);

    CHECK(result.ok);
    CHECK(result.found);
    CHECK(result.config.model == "shared-model");
    CHECK(result.config.max_tokens == 222);
    CHECK(result.local_path.find("config.local.json") != std::string::npos);
}

TEST_CASE("uses built-in defaults when no config exists")
{
    const std::filesystem::path exe_dir = std::filesystem::temp_directory_path() / "agent-core-empty-config-test";
    std::filesystem::remove_all(exe_dir);
    std::filesystem::create_directories(exe_dir);

    const agent::hosts::cli::DefaultConfigResult result =
        agent::hosts::cli::LoadDefaultCliConfig((exe_dir / "agent-cli.exe").string());
    std::filesystem::remove_all(exe_dir);

    CHECK(result.ok);
    CHECK_FALSE(result.found);
    CHECK(result.config.model == agent::hosts::cli::CliConfig{}.model);
    CHECK(result.config.endpoint == agent::hosts::cli::CliConfig{}.endpoint);
}

TEST_CASE("writes default config and refuses overwrite")
{
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "agent-core-init-config-test.json";
    std::filesystem::remove(path);

    const agent::hosts::cli::ConfigInitResult init = agent::hosts::cli::WriteDefaultCliConfigFile(path.string());
    CHECK(init.ok);

    const agent::hosts::cli::ConfigLoadResult loaded = agent::hosts::cli::LoadCliConfigFile(path.string());
    CHECK(loaded.ok);
    CHECK(loaded.config.model == agent::hosts::cli::CliConfig{}.model);

    const agent::hosts::cli::ConfigInitResult again = agent::hosts::cli::WriteDefaultCliConfigFile(path.string());
    CHECK_FALSE(again.ok);
    CHECK(again.error.find("already exists") != std::string::npos);

    std::filesystem::remove(path);
}

TEST_CASE("reports missing config file")
{
    const agent::hosts::cli::ConfigLoadResult result =
        agent::hosts::cli::LoadCliConfigFile("definitely-missing-config.json");

    CHECK_FALSE(result.ok);
    CHECK(result.error.find("cannot open") != std::string::npos);
}

TEST_CASE("resolves api key from environment lookup")
{
    agent::hosts::cli::CliConfig config;
    config.api_key_env = "TEST_KEY";

    const std::string key =
        agent::hosts::cli::ResolveApiKey(config,
                                         [](const std::string& name)
                                         {
                                             return name == "TEST_KEY" ? std::string("secret") : std::string();
                                         });

    CHECK(key == "secret");
}

TEST_CASE("runs a single question and prints content")
{
    agent::tests::FakeHttpTransport transport;
    transport.SetHandler(
        [](const agent::transport::HttpRequest&)
        {
            return MakeChatResponse("hello");
        });

    agent::model::OpenAiChatModel adapter(transport, MakeOpenAiConfig());
    std::ostringstream out;
    std::ostringstream err;
    agent::runtime::ServiceRegistry services;
    TestExecutor executor(adapter, services);
    const int exit_code = agent::hosts::cli::RunCli({"hello"}, executor.Get(), MakeCliConfig(), out, err, services);

    CHECK(exit_code == 0);
    CHECK(out.str().find("hello") != std::string::npos);
    CHECK(err.str().empty());
    CHECK(transport.RequestCount() == 1);

    const nlohmann::json body = nlohmann::json::parse(transport.LastRequest().body);
    CHECK(body["messages"].size() == 2);
    CHECK(body["messages"][0]["role"] == "system");
    CHECK(body["messages"][1]["role"] == "user");
    CHECK(body["messages"][1]["content"] == "hello");
}

TEST_CASE("runs a tool call end to end through cli")
{
    const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const std::filesystem::path workspace =
        std::filesystem::temp_directory_path() / ("agent-core-cli-tool-" + std::to_string(stamp));
    std::filesystem::create_directories(workspace);
    {
        std::ofstream file(workspace / "README.md");
        file << "hello from file";
    }

    int request_count = 0;
    std::string first_body;
    agent::tests::FakeHttpTransport transport;
    transport.SetHandler(
        [&](const agent::transport::HttpRequest& request)
        {
            ++request_count;
            if (request_count == 1)
            {
                first_body = request.body;
                nlohmann::json function;
                function["name"] = "read_file";
                function["arguments"] = R"({"path":"README.md"})";
                nlohmann::json tool_call;
                tool_call["id"] = "call_1";
                tool_call["type"] = "function";
                tool_call["function"] = function;
                nlohmann::json message;
                message["content"] = nullptr;
                message["tool_calls"] = nlohmann::json::array({tool_call});
                nlohmann::json choice;
                choice["message"] = message;
                choice["finish_reason"] = "tool_calls";
                nlohmann::json parsed;
                parsed["choices"] = nlohmann::json::array({choice});
                agent::transport::HttpResponse response;
                response.status = 200;
                response.body = parsed.dump();
                return response;
            }
            return MakeChatResponse("done");
        });

    agent::model::OpenAiChatModel adapter(transport, MakeOpenAiConfig());
    std::ostringstream out;
    std::ostringstream err;
    agent::runtime::ServiceRegistry services;
    const auto tools = std::make_shared<agent::runtime::ToolRegistry>();
    std::string tool_error;
    REQUIRE(tools->Register(std::make_shared<agent::plugins::tools::ReadFileTool>(workspace), tool_error));
    services.Set<agent::runtime::ToolRegistry>(tools);

    TestExecutor executor(adapter, services);
    const int exit_code =
        agent::hosts::cli::RunCli({"read README"}, executor.Get(), MakeCliConfig(), out, err, services);

    CHECK(exit_code == 0);
    CHECK(out.str().find("done") != std::string::npos);
    CHECK(err.str().empty());
    CHECK(transport.RequestCount() == 2);

    const nlohmann::json first_request = nlohmann::json::parse(first_body);
    REQUIRE(first_request["tools"].size() == 1);
    CHECK(first_request["tools"][0]["function"]["name"] == "read_file");

    const nlohmann::json second_request = nlohmann::json::parse(transport.LastRequest().body);
    const nlohmann::json& tool_message = second_request["messages"].back();
    CHECK(tool_message["role"] == "tool");
    CHECK(tool_message["tool_call_id"] == "call_1");
    CHECK(tool_message["content"] == "hello from file");

    std::filesystem::remove_all(workspace);
}

TEST_CASE("reports model errors")
{
    agent::tests::FakeHttpTransport transport;
    transport.SetHandler(
        [](const agent::transport::HttpRequest&)
        {
            agent::transport::HttpResponse response;
            response.error = "timeout";
            return response;
        });

    agent::model::OpenAiChatModel adapter(transport, MakeOpenAiConfig());
    std::ostringstream out;
    std::ostringstream err;
    agent::runtime::ServiceRegistry services;
    TestExecutor executor(adapter, services);
    const int exit_code = agent::hosts::cli::RunCli({"hello"}, executor.Get(), MakeCliConfig(), out, err, services);

    CHECK(exit_code == 1);
    CHECK(err.str().find("timeout") != std::string::npos);
}

TEST_CASE("rejects empty question")
{
    agent::tests::FakeHttpTransport transport;
    agent::model::OpenAiChatModel adapter(transport, MakeOpenAiConfig());
    std::ostringstream out;
    std::ostringstream err;
    agent::runtime::ServiceRegistry services;
    TestExecutor executor(adapter, services);
    const int exit_code = agent::hosts::cli::RunCli({}, executor.Get(), MakeCliConfig(), out, err, services);

    CHECK(exit_code == 2);
    CHECK(err.str().find("usage") != std::string::npos);
    CHECK(transport.RequestCount() == 0);
}

TEST_CASE("interactive chat keeps history and handles commands")
{
    FakeChatModel adapter;
    adapter.replies = {"first reply", "second reply"};

    agent::hosts::cli::CliConfig config;
    config.system_prompt = "be brief";

    std::istringstream input("hello\nworld\n/clear\nafter clear\n/exit\n");
    std::ostringstream output;
    std::ostringstream error;
    agent::runtime::ServiceRegistry services;
    TestExecutor executor(adapter, services);
    const int exit_code = agent::hosts::cli::RunInteractive(executor.Get(), config, input, output, error, services);

    CHECK(exit_code == 0);
    CHECK(adapter.requests.size() == 3);

    CHECK(adapter.requests[0].messages.size() == 2);
    CHECK(adapter.requests[0].messages[0].role == agent::core::ChatRole::System);
    CHECK(adapter.requests[0].messages[1].role == agent::core::ChatRole::User);

    CHECK(adapter.requests[1].messages.size() == 4);
    CHECK(adapter.requests[1].messages[2].role == agent::core::ChatRole::Assistant);
    CHECK(adapter.requests[1].messages[2].content == "first reply");

    CHECK(adapter.requests[2].messages.size() == 2);
    CHECK(adapter.requests[2].messages[1].content == "after clear");

    CHECK(output.str().find("first reply") != std::string::npos);
    CHECK(output.str().find("second reply") != std::string::npos);
    CHECK(output.str().find("history cleared") != std::string::npos);
}

TEST_CASE("one-shot reports stats when enabled")
{
    FakeChatModel adapter;
    adapter.replies = {"hello"};

    std::ostringstream output;
    std::ostringstream error;
    const auto metrics = std::make_shared<agent::plugins::metrics::MetricsCollector>();
    agent::runtime::ServiceRegistry services;
    const auto systems = std::make_shared<agent::core::SystemRegistry>();
    systems->Add(metrics);
    services.Set<agent::core::SystemRegistry>(systems);
    services.Set<agent::core::StatsView>(metrics);

    TestExecutor executor(adapter, services);
    const int exit_code = agent::hosts::cli::RunCli({"hi"}, executor.Get(), MakeCliConfig(), output, error, services);

    CHECK(exit_code == 0);
    CHECK(output.str().find("hello") != std::string::npos);
    CHECK(error.str().find("[stats]") != std::string::npos);
    CHECK(metrics->Snapshot().turns == 1);
    CHECK(metrics->Snapshot().steps == 1);
}

TEST_CASE("interactive reports metrics when enabled")
{
    FakeChatModel adapter;
    adapter.replies = {"hello"};

    agent::hosts::cli::CliConfig config;
    config.system_prompt = "be brief";

    std::istringstream input("hi\n/exit\n");
    std::ostringstream output;
    std::ostringstream error;
    const auto metrics = std::make_shared<agent::plugins::metrics::MetricsCollector>();
    agent::runtime::ServiceRegistry services;
    const auto systems = std::make_shared<agent::core::SystemRegistry>();
    systems->Add(metrics);
    services.Set<agent::core::SystemRegistry>(systems);
    services.Set<agent::core::StatsView>(metrics);
    TestExecutor executor(adapter, services);
    const int exit_code =
        agent::hosts::cli::RunInteractive(executor.Get(), config, input, output, error, services, true);

    CHECK(exit_code == 0);
    CHECK(metrics->Snapshot().turns == 1);
    CHECK(metrics->Snapshot().steps == 1);
    CHECK(error.str().find("[stats]") != std::string::npos);
}

TEST_CASE("interactive stats command reports current session")
{
    FakeChatModel adapter;
    adapter.replies = {"hello"};

    agent::hosts::cli::CliConfig config;
    config.system_prompt = "be brief";

    std::istringstream input("hi\n/stats\n/exit\n");
    std::ostringstream output;
    std::ostringstream error;
    const auto metrics = std::make_shared<agent::plugins::metrics::MetricsCollector>();
    agent::runtime::ServiceRegistry services;
    const auto systems = std::make_shared<agent::core::SystemRegistry>();
    systems->Add(metrics);
    services.Set<agent::core::SystemRegistry>(systems);
    services.Set<agent::core::StatsView>(metrics);
    TestExecutor executor(adapter, services);
    const int exit_code =
        agent::hosts::cli::RunInteractive(executor.Get(), config, input, output, error, services, false);

    CHECK(exit_code == 0);
    CHECK(metrics->Snapshot().turns == 1);
    CHECK(output.str().find("[stats]") != std::string::npos);
}
