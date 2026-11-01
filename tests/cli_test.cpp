#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "agent/hosts/cli/cli_app.h"
#include "agent/hosts/cli/cli_config.h"
#include "agent/hosts/cli/interactive_app.h"
#include "agent/model/openai_chat_model.h"
#include "agent/plugins/metrics/metrics_collector.h"
#include "support/fake_http_transport.h"

namespace
{

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
    const std::string path = "config.local.json";
    {
        std::ofstream file(path);
        file << R"({"model":"unit-test-model"})";
    }

    const agent::hosts::cli::DefaultConfigResult result = agent::hosts::cli::LoadDefaultCliConfig();
    std::filesystem::remove(path);

    CHECK(result.ok);
    CHECK(result.found);
    CHECK(result.path == path);
    CHECK(result.config.model == "unit-test-model");
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
    const int exit_code = agent::hosts::cli::RunCli({"hello"}, adapter, MakeCliConfig(), out, err, services);

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
    const int exit_code = agent::hosts::cli::RunCli({"hello"}, adapter, MakeCliConfig(), out, err, services);

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
    const int exit_code = agent::hosts::cli::RunCli({}, adapter, MakeCliConfig(), out, err, services);

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
    const int exit_code = agent::hosts::cli::RunInteractive(adapter, config, input, output, error, services);

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
    services.Set<agent::core::Tracer>(metrics);
    services.Set<agent::core::StatsView>(metrics);

    const int exit_code = agent::hosts::cli::RunCli({"hi"}, adapter, MakeCliConfig(), output, error, services);

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
    services.Set<agent::core::Tracer>(metrics);
    services.Set<agent::core::StatsView>(metrics);
    const int exit_code = agent::hosts::cli::RunInteractive(adapter, config, input, output, error, services, true);

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
    services.Set<agent::core::Tracer>(metrics);
    services.Set<agent::core::StatsView>(metrics);
    const int exit_code = agent::hosts::cli::RunInteractive(adapter, config, input, output, error, services, false);

    CHECK(exit_code == 0);
    CHECK(metrics->Snapshot().turns == 1);
    CHECK(output.str().find("[stats]") != std::string::npos);
}
