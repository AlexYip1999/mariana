#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "agent/core/system_registry.h"
#include "agent/hosts/cli/cli_command.h"
#include "agent/hosts/cli/cli_config.h"
#include "agent/hosts/cli/run_loop.h"
#include "agent/model/openai_chat_model.h"
#include "agent/plugins/metrics/metrics_collector.h"
#include "agent/plugins/tools/read_file_tool.h"
#include "agent/runtime/agent_runner.h"
#include "agent/runtime/tool_registry.h"
#include "support/fake_http_transport.h"
#include "support/runtime_fixture.h"

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
        : m_executor(MakeDependencies(model, services))
    {}

    agent::runtime::AgentExecutor& Get()
    {
        return m_executor;
    }

  private:
    static agent::runtime::RunnerDependencies MakeDependencies(agent::core::ChatModel& model,
                                                               agent::runtime::ServiceRegistry& services)
    {
        if (!services.Get<agent::runtime::ToolRegistry>())
        {
            auto tools = std::make_shared<agent::runtime::ToolRegistry>();
            std::string error;
            tools->Register(std::make_shared<agent::plugins::tools::ReadFileTool>(std::filesystem::current_path()),
                            error);
            services.Set<agent::runtime::ToolRegistry>(tools);
        }
        return agent::tests::MakeRunnerDependencies(model, services);
    }

    agent::runtime::AgentRunner m_executor;
};

void AddInteractiveCommands(agent::runtime::ServiceRegistry& services, const agent::hosts::cli::CliConfig& config)
{
    auto context = std::make_shared<agent::hosts::cli::CliContext>();
    context->config = config;
    context->stats = services.Get<agent::core::StatsView>();
    services.Set<agent::hosts::cli::CliContext>(context);

    auto commands = std::make_shared<agent::hosts::cli::CommandRegistry>();
    agent::hosts::cli::RegisterDefaultCommands(*commands);
    services.Set<agent::hosts::cli::CommandRegistry>(commands);
}

agent::runtime::Agent MakeAgent(const agent::hosts::cli::CliConfig& config, agent::runtime::AgentExecutor& executor,
                                agent::runtime::ServiceRegistry& services)
{
    agent::runtime::AgentProfile profile;
    profile.model = config.model;
    profile.system_prompt = config.system_prompt;
    profile.temperature = config.temperature;
    profile.max_tokens = config.max_tokens;
    profile.max_steps = config.max_steps;
    profile.max_context_chars = config.max_context_chars;
    profile.tool_names = config.tool_names;

    agent::runtime::AgentDescriptor descriptor;
    descriptor.id.value = "cli";
    descriptor.name = "agent-cli";
    descriptor.version = "test";

    agent::runtime::AgentDependencies agent_services;
    agent_services.services = std::shared_ptr<agent::runtime::ServiceRegistry>(&services,
                                                                               [](agent::runtime::ServiceRegistry*)
                                                                               {
                                                                               });
    agent_services.executor = std::shared_ptr<agent::runtime::AgentExecutor>(&executor,
                                                                             [](agent::runtime::AgentExecutor*)
                                                                             {
                                                                             });
    return agent::runtime::Agent(std::move(descriptor), std::move(profile), agent_services);
}

} // namespace

TEST_CASE("cli validates and loads total turn budgets")
{
    const auto parsed =
        agent::hosts::cli::ParseCliConfig(R"({"turn_timeout_ms":750,"max_tool_calls":2,"max_tool_output_bytes":99})");
    REQUIRE(parsed.ok);
    CHECK(parsed.config.turn_timeout_ms == 750);
    CHECK(parsed.config.max_tool_calls == 2);
    CHECK(parsed.config.max_tool_output_bytes == 99);

    CHECK_FALSE(agent::hosts::cli::ParseCliConfig(R"({"turn_timeout_ms":0})").ok);
    CHECK_FALSE(agent::hosts::cli::ParseCliConfig(R"({"max_tool_calls":-1})").ok);
    CHECK_FALSE(agent::hosts::cli::ParseCliConfig(R"({"max_tool_output_bytes":1.5})").ok);

    CHECK(agent::hosts::cli::ParseCliConfig(R"({"max_tool_calls":0,"max_tool_output_bytes":0})").ok);
}

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

TEST_CASE("HTTP endpoint exceptions require an exact loopback host")
{
    const std::vector<std::string> allowed = {"http://localhost/chat", "http://127.0.0.1:8080/chat",
                                              "http://[::1]:8080/chat", "HTTP://LOCALHOST:80/chat",
                                              "https://example.test/chat"};
    const std::vector<std::string> denied = {"http://localhost.attacker.example/chat",
                                             "http://127.0.0.1@attacker.example/chat",
                                             "http://localhost@attacker.example/chat",
                                             "http://[::1].attacker.example/chat",
                                             "http://localhost:80@attacker.example/chat",
                                             "http://localhost:abc/chat",
                                             "http://localhost:65536/chat",
                                             "http://localhost:/chat",
                                             "http://localhost%2eattacker.example/chat",
                                             "http://localhost\\@attacker.example/chat",
                                             "https:///chat",
                                             "http://localhost\n/chat"};

    for (const auto& endpoint : allowed)
    {
        CAPTURE(endpoint);
        CHECK(agent::hosts::cli::ParseCliConfig(nlohmann::json{{"endpoint", endpoint}}.dump()).ok);
    }

    for (const auto& endpoint : denied)
    {
        CAPTURE(endpoint);
        CHECK_FALSE(agent::hosts::cli::ParseCliConfig(nlohmann::json{{"endpoint", endpoint}}.dump()).ok);
    }
}

TEST_CASE("rejects negative fractional and overflowing integer settings")
{
    const char* fields[] = {"timeout_ms", "max_tokens", "max_steps", "max_context_chars"};
    const double invalid_values[] = {-1.0, 1.5};
    for (const char* field : fields)
    {
        for (const double value : invalid_values)
        {
            CAPTURE(field);
            CHECK_FALSE(agent::hosts::cli::ParseCliConfig(nlohmann::json{{field, value}}.dump()).ok);
        }
    }

    CHECK_FALSE(agent::hosts::cli::ParseCliConfig(R"({"timeout_ms":0})").ok);
    CHECK_FALSE(agent::hosts::cli::ParseCliConfig(R"({"timeout_ms":4294967297})").ok);
    CHECK_FALSE(agent::hosts::cli::ParseCliConfig(R"({"max_context_chars":-1})").ok);
    CHECK(agent::hosts::cli::ParseCliConfig(R"({"max_context_chars":0})").ok);
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
    auto transport = std::make_shared<agent::tests::FakeHttpTransport>();
    transport->SetHandler(
        [](const agent::transport::HttpRequest&)
        {
            return MakeChatResponse("hello");
        });

    agent::model::OpenAiChatModel adapter(transport, MakeOpenAiConfig());

    std::istringstream input("hello\n/exit\n");
    std::ostringstream out;
    std::ostringstream err;
    agent::runtime::ServiceRegistry services;
    AddInteractiveCommands(services, MakeCliConfig());
    TestExecutor executor(adapter, services);
    auto agent = MakeAgent(MakeCliConfig(), executor.Get(), services);

    const int exit_code = agent::hosts::cli::RunCliLoop(agent, services, input, out, err);

    CHECK(exit_code == 0);
    CHECK(out.str().find("hello") != std::string::npos);
    CHECK(err.str().empty());
    CHECK(transport->RequestCount() == 1);

    const nlohmann::json body = nlohmann::json::parse(transport->LastRequest().body);
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
    auto transport = std::make_shared<agent::tests::FakeHttpTransport>();
    transport->SetHandler(
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

    std::istringstream input("read README\n/exit\n");
    std::ostringstream out;
    std::ostringstream err;
    agent::runtime::ServiceRegistry services;
    const auto tools = std::make_shared<agent::runtime::ToolRegistry>();
    std::string tool_error;
    REQUIRE(tools->Register(std::make_shared<agent::plugins::tools::ReadFileTool>(workspace), tool_error));
    services.Set<agent::runtime::ToolRegistry>(tools);

    AddInteractiveCommands(services, MakeCliConfig());
    TestExecutor executor(adapter, services);
    auto agent = MakeAgent(MakeCliConfig(), executor.Get(), services);

    const int exit_code = agent::hosts::cli::RunCliLoop(agent, services, input, out, err);

    CHECK(exit_code == 0);
    CHECK(out.str().find("done") != std::string::npos);
    CHECK(err.str().empty());
    CHECK(transport->RequestCount() == 2);

    const nlohmann::json first_request = nlohmann::json::parse(first_body);
    REQUIRE(first_request["tools"].size() == 1);
    CHECK(first_request["tools"][0]["function"]["name"] == "read_file");

    const nlohmann::json second_request = nlohmann::json::parse(transport->LastRequest().body);
    const nlohmann::json& tool_message = second_request["messages"].back();
    CHECK(tool_message["role"] == "tool");
    CHECK(tool_message["tool_call_id"] == "call_1");
    CHECK(tool_message["content"] == "hello from file");

    std::filesystem::remove_all(workspace);
}

TEST_CASE("reports model errors")
{
    auto transport = std::make_shared<agent::tests::FakeHttpTransport>();
    transport->SetHandler(
        [](const agent::transport::HttpRequest&)
        {
            agent::transport::HttpResponse response;
            response.error = "timeout";
            return response;
        });

    agent::model::OpenAiChatModel adapter(transport, MakeOpenAiConfig());

    std::istringstream input("hello\n/exit\n");
    std::ostringstream out;
    std::ostringstream err;
    agent::runtime::ServiceRegistry services;
    AddInteractiveCommands(services, MakeCliConfig());
    TestExecutor executor(adapter, services);
    auto agent = MakeAgent(MakeCliConfig(), executor.Get(), services);

    const int exit_code = agent::hosts::cli::RunCliLoop(agent, services, input, out, err);

    CHECK(exit_code == 0);
    CHECK(err.str().find("timeout") != std::string::npos);
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
    AddInteractiveCommands(services, config);
    TestExecutor executor(adapter, services);
    auto agent = MakeAgent(config, executor.Get(), services);
    const int exit_code = agent::hosts::cli::RunCliLoop(agent, services, input, output, error);

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

TEST_CASE("interactive trims whitespace before commands")
{
    FakeChatModel adapter;
    agent::hosts::cli::CliConfig config;
    config.system_prompt = "be brief";

    std::istringstream input("  /stats\n/exit\n");
    std::ostringstream output;
    std::ostringstream error;
    const auto metrics = std::make_shared<agent::plugins::metrics::MetricsCollector>();
    agent::runtime::ServiceRegistry services;
    const auto systems = std::make_shared<agent::core::SystemRegistry>();
    systems->Add(metrics);
    services.Set<agent::core::SystemRegistry>(systems);
    services.Set<agent::core::StatsView>(metrics);

    AddInteractiveCommands(services, config);
    TestExecutor executor(adapter, services);
    auto agent = MakeAgent(config, executor.Get(), services);

    CHECK(agent::hosts::cli::RunCliLoop(agent, services, input, output, error) == 0);
    CHECK(output.str().find("[stats]") != std::string::npos);
    CHECK(adapter.requests.empty());
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

    AddInteractiveCommands(services, config);
    TestExecutor executor(adapter, services);
    auto agent = MakeAgent(config, executor.Get(), services);

    const int exit_code = agent::hosts::cli::RunCliLoop(agent, services, input, output, error, true);

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

    AddInteractiveCommands(services, config);
    TestExecutor executor(adapter, services);
    auto agent = MakeAgent(config, executor.Get(), services);

    const int exit_code = agent::hosts::cli::RunCliLoop(agent, services, input, output, error, false);

    CHECK(exit_code == 0);
    CHECK(metrics->Snapshot().turns == 1);
    CHECK(output.str().find("[stats]") != std::string::npos);
}
