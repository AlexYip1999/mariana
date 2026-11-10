#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/hosts/cli/host_runner.h"
#include "agent/transport/http_transport.h"

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

class FixedTransport : public agent::transport::HttpTransport
{
  public:
    explicit FixedTransport(std::shared_ptr<std::vector<nlohmann::json>> requests = nullptr)
        : m_requests(std::move(requests))
    {}

    agent::transport::HttpResponse PostJson(const agent::transport::HttpRequest& request) override
    {
        if (m_requests)
        {
            m_requests->push_back(nlohmann::json::parse(request.body));
        }
        nlohmann::json parsed;
        parsed["choices"] = nlohmann::json::array();
        parsed["choices"].push_back({{"message", {{"content", "ok"}}}, {"finish_reason", "stop"}});
        agent::transport::HttpResponse response;
        response.status = 200;
        response.body = parsed.dump();
        return response;
    }

  private:
    std::shared_ptr<std::vector<nlohmann::json>> m_requests;
};

std::filesystem::path MakeTempWorkspace()
{
    const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const std::filesystem::path workspace =
        std::filesystem::temp_directory_path() / ("agent-core-host-" + std::to_string(stamp));
    std::filesystem::create_directories(workspace);
    return workspace;
}

agent::hosts::cli::HostEnvironment MakeEnvironment(std::map<std::string, std::string> values)
{
    agent::hosts::cli::HostEnvironment environment;
    environment.get_env = [values](const std::string& name)
    {
        const auto found = values.find(name);
        return found == values.end() ? std::string() : found->second;
    };
    environment.current_path = []
    {
        return std::filesystem::current_path();
    };
    environment.create_transport = []
    {
        return std::make_unique<FixedTransport>();
    };
    return environment;
}

int RunWithInput(const std::vector<std::string>& args, const agent::hosts::cli::HostEnvironment& environment,
                 const std::string& input_text, std::ostringstream& out, std::ostringstream& err)
{
    std::istringstream input(input_text);
    return agent::hosts::cli::RunHost(args, environment, input, out, err);
}

} // namespace

TEST_CASE("host runner prints help and version")
{
    const auto environment = MakeEnvironment({});
    std::ostringstream out;
    std::ostringstream err;

    CHECK(RunWithInput({"--help"}, environment, "", out, err) == 0);
    CHECK(out.str().find("agent-cli") != std::string::npos);

    out.str("");
    err.str("");
    CHECK(RunWithInput({"--version"}, environment, "", out, err) == 0);
    CHECK_FALSE(out.str().empty());
    CHECK(err.str().empty());
}

TEST_CASE("workspace changes preserve tool configuration until explicitly enabled")
{
    const auto workspace = MakeTempWorkspace();
    {
        ScopedWorkingDirectory scoped(workspace);
        std::ofstream("no-tools.json") << R"({"tool_names":[]})";
        auto requests = std::make_shared<std::vector<nlohmann::json>>();
        auto environment = MakeEnvironment({{"DEEPSEEK_API_KEY", "test-only"}});
        environment.create_transport = [requests]()
        {
            return std::make_unique<FixedTransport>(requests);
        };

        std::ostringstream out;
        std::ostringstream err;
        const std::string input =
            "first\n/workspace .\nsecond\n/tools on\nthird\n/tools off\n/workspace .\nfourth\n/exit\n";

        CHECK(RunWithInput({"--config", "no-tools.json"}, environment, input, out, err) == 0);
        CHECK(err.str().empty());

        REQUIRE(requests->size() == 4);
        CHECK_FALSE((*requests)[0].contains("tools"));
        CHECK_FALSE((*requests)[1].contains("tools"));
        REQUIRE((*requests)[2]["tools"].size() == 1);
        CHECK((*requests)[2]["tools"][0]["function"]["name"] == "read_file");
        CHECK_FALSE((*requests)[3].contains("tools"));
    }
    std::filesystem::remove_all(workspace);
}

TEST_CASE("host runner config init command is idempotent")
{
    const std::filesystem::path workspace = MakeTempWorkspace();
    {
        ScopedWorkingDirectory scoped(workspace);
        const auto environment = MakeEnvironment({{"DEEPSEEK_API_KEY", "test-key"}});
        std::ostringstream out;
        std::ostringstream err;

        CHECK(RunWithInput({}, environment, "/config init\n/exit\n", out, err) == 0);
        CHECK(std::filesystem::exists(workspace / "config.local.json"));

        out.str("");
        err.str("");
        CHECK(RunWithInput({}, environment, "/config init\n/exit\n", out, err) == 0);
        CHECK(err.str().find("already exists") != std::string::npos);
    }
    std::filesystem::remove_all(workspace);
}

TEST_CASE("host runner reports missing api key")
{
    const std::filesystem::path workspace = MakeTempWorkspace();
    {
        ScopedWorkingDirectory scoped(workspace);
        const auto environment = MakeEnvironment({});
        std::ostringstream out;
        std::ostringstream err;

        CHECK(RunWithInput({}, environment, "", out, err) == 2);
        CHECK(err.str().find("missing API key") != std::string::npos);
    }
    std::filesystem::remove_all(workspace);
}

TEST_CASE("host runner reports invalid workspace command")
{
    const std::filesystem::path workspace = MakeTempWorkspace();
    {
        ScopedWorkingDirectory scoped(workspace);
        const auto environment = MakeEnvironment({{"DEEPSEEK_API_KEY", "test-key"}});
        std::ostringstream out;
        std::ostringstream err;

        CHECK(RunWithInput({}, environment, "/workspace definitely-missing\n/exit\n", out, err) == 0);
        CHECK(err.str().find("invalid workspace") != std::string::npos);
    }
    std::filesystem::remove_all(workspace);
}

TEST_CASE("host runner can disable tools from command")
{
    const std::filesystem::path workspace = MakeTempWorkspace();
    {
        ScopedWorkingDirectory scoped(workspace);
        const auto environment = MakeEnvironment({{"DEEPSEEK_API_KEY", "test-key"}});
        std::ostringstream out;
        std::ostringstream err;

        CHECK(RunWithInput({}, environment, "/tools off\nhi\n/exit\n", out, err) == 0);
        CHECK(out.str().find("tools: off") != std::string::npos);
        CHECK(out.str().find("ok") != std::string::npos);
    }
    std::filesystem::remove_all(workspace);
}

TEST_CASE("host runner trace defaults on and shows current trace")
{
    const std::filesystem::path workspace = MakeTempWorkspace();
    {
        ScopedWorkingDirectory scoped(workspace);
        const auto environment = MakeEnvironment({{"DEEPSEEK_API_KEY", "test-key"}});
        std::ostringstream out;
        std::ostringstream err;

        CHECK(RunWithInput({}, environment, "hi\n/trace\n/exit\n", out, err) == 0);
        CHECK(out.str().find("Turn 1") != std::string::npos);
        CHECK(out.str().find("model_response") != std::string::npos);
    }
    std::filesystem::remove_all(workspace);
}

TEST_CASE("host runner shows trace and aggregates usage from commands")
{
    const std::filesystem::path workspace = MakeTempWorkspace();
    {
        ScopedWorkingDirectory scoped(workspace);
        std::filesystem::create_directories(workspace / "traces");
        const std::filesystem::path trace_file = workspace / "traces" / "demo.jsonl";
        {
            std::ofstream file(trace_file);
            file << R"({"ts":"2026-11-01T10:00:00Z","event":"turn_start","turn":1})" << "\n";
            file
                << R"({"ts":"2026-11-01T10:00:01Z","event":"model_response","turn":1,"step":1,"ok":true,"usage":{"prompt_tokens":3,"completion_tokens":7,"total_tokens":10}})"
                << "\n";
            file << R"({"ts":"2026-11-01T10:00:02Z","event":"turn_end","turn":1,"ok":true,"steps":1,"duration_ms":10})"
                 << "\n";
        }

        const auto environment = MakeEnvironment({{"DEEPSEEK_API_KEY", "test-key"}});
        std::ostringstream out;
        std::ostringstream err;
        const std::string input_text = "/trace show " + trace_file.string() + "\n/stats all\n/exit\n";

        CHECK(RunWithInput({}, environment, input_text, out, err) == 0);
        CHECK(out.str().find("Turn 1") != std::string::npos);
        CHECK(out.str().find("model_response") != std::string::npos);
        CHECK(out.str().find("total_tokens=10") != std::string::npos);
    }
    std::filesystem::remove_all(workspace);
}

TEST_CASE("host runner rejects unknown tool names at startup")
{
    const std::filesystem::path workspace = MakeTempWorkspace();
    {
        ScopedWorkingDirectory scoped(workspace);
        const std::filesystem::path config_path = workspace / "tool-config.json";
        {
            std::ofstream file(config_path);
            file << R"({"tool_names":["missing_tool"]})";
        }

        const auto environment = MakeEnvironment({{"DEEPSEEK_API_KEY", "test-key"}});
        std::ostringstream out;
        std::ostringstream err;
        const std::vector<std::string> args = {"--config", config_path.string()};

        CHECK(RunWithInput(args, environment, "", out, err) == 2);
        CHECK(err.str().find("unknown tool") != std::string::npos);
    }
    std::filesystem::remove_all(workspace);
}

TEST_CASE("host runner persists jsonl memory and recalls it in a later session")
{
    const std::filesystem::path workspace = MakeTempWorkspace();
    {
        ScopedWorkingDirectory scoped(workspace);
        const std::filesystem::path memory_path = workspace / "memory.jsonl";
        auto requests = std::make_shared<std::vector<nlohmann::json>>();
        auto environment = MakeEnvironment({{"DEEPSEEK_API_KEY", "test-key"}});
        environment.create_transport = [requests]()
        {
            return std::make_unique<FixedTransport>(requests);
        };

        // 第一个会话：写入记忆，并验证同会话内召回注入。
        std::ostringstream out;
        std::ostringstream err;
        const std::string first_input = "/memory add 用户的电话是 13800001111\n/memory list\n电话\n/exit\n";
        CHECK(RunWithInput({"--memory", memory_path.string()}, environment, first_input, out, err) == 0);
        CHECK(out.str().find("remembered cli-") != std::string::npos);
        CHECK(out.str().find("13800001111") != std::string::npos);
        CHECK(std::filesystem::exists(memory_path));
        REQUIRE(requests->size() == 1);
        CHECK((*requests)[0].dump().find("13800001111") != std::string::npos);

        // 第二个会话：重新加载同一 JSONL，验证跨会话持久化与召回，随后清空。
        requests->clear();
        out.str("");
        err.str("");
        const std::string second_input = "电话\n/memory clear\n/memory\n/exit\n";
        CHECK(RunWithInput({"--memory", memory_path.string()}, environment, second_input, out, err) == 0);
        REQUIRE(requests->size() == 1);
        CHECK((*requests)[0].dump().find("13800001111") != std::string::npos);
        CHECK(out.str().find("memory cleared") != std::string::npos);
        CHECK(out.str().find("(0 records)") != std::string::npos);
    }
    std::filesystem::remove_all(workspace);
}
