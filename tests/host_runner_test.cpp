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
    agent::transport::HttpResponse PostJson(const agent::transport::HttpRequest&) override
    {
        nlohmann::json parsed;
        parsed["choices"] = nlohmann::json::array();
        parsed["choices"].push_back({{"message", {{"content", "ok"}}}, {"finish_reason", "stop"}});
        agent::transport::HttpResponse response;
        response.status = 200;
        response.body = parsed.dump();
        return response;
    }
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

} // namespace

TEST_CASE("host runner prints help and version")
{
    const auto environment = MakeEnvironment({});
    std::istringstream input;
    std::ostringstream out;
    std::ostringstream err;

    CHECK(agent::hosts::cli::RunHost({"--help"}, environment, input, out, err) == 0);
    CHECK(out.str().find("agent-cli") != std::string::npos);

    out.str("");
    err.str("");
    CHECK(agent::hosts::cli::RunHost({"--version"}, environment, input, out, err) == 0);
    CHECK_FALSE(out.str().empty());
    CHECK(err.str().empty());
}

TEST_CASE("host runner init config is idempotent")
{
    const std::filesystem::path workspace = MakeTempWorkspace();
    {
        ScopedWorkingDirectory scoped(workspace);
        const auto environment = MakeEnvironment({});
        std::istringstream input;
        std::ostringstream out;
        std::ostringstream err;

        CHECK(agent::hosts::cli::RunHost({"--init-config"}, environment, input, out, err) == 0);
        CHECK(std::filesystem::exists(workspace / "config.local.json"));

        out.str("");
        err.str("");
        CHECK(agent::hosts::cli::RunHost({"--init-config"}, environment, input, out, err) == 0);
        CHECK(out.str().find("already exists") != std::string::npos);
    }
    std::filesystem::remove_all(workspace);
}

TEST_CASE("host runner reports missing api key")
{
    const std::filesystem::path workspace = MakeTempWorkspace();
    {
        ScopedWorkingDirectory scoped(workspace);
        const auto environment = MakeEnvironment({});
        std::istringstream input;
        std::ostringstream out;
        std::ostringstream err;

        CHECK(agent::hosts::cli::RunHost({"hi"}, environment, input, out, err) == 2);
        CHECK(err.str().find("missing API key") != std::string::npos);
    }
    std::filesystem::remove_all(workspace);
}

TEST_CASE("host runner rejects invalid workspace")
{
    const std::filesystem::path workspace = MakeTempWorkspace();
    {
        ScopedWorkingDirectory scoped(workspace);
        auto environment = MakeEnvironment({{"DEEPSEEK_API_KEY", "test-key"}});
        std::istringstream input;
        std::ostringstream out;
        std::ostringstream err;

        const std::vector<std::string> args = {"--workspace", "definitely-missing", "hi"};
        CHECK(agent::hosts::cli::RunHost(args, environment, input, out, err) == 2);
        CHECK(err.str().find("invalid workspace") != std::string::npos);
    }
    std::filesystem::remove_all(workspace);
}

TEST_CASE("host runner chats without tools")
{
    const std::filesystem::path workspace = MakeTempWorkspace();
    {
        ScopedWorkingDirectory scoped(workspace);
        const auto environment = MakeEnvironment({{"DEEPSEEK_API_KEY", "test-key"}});
        std::istringstream input;
        std::ostringstream out;
        std::ostringstream err;

        CHECK(agent::hosts::cli::RunHost({"--no-tools", "hi"}, environment, input, out, err) == 0);
        CHECK(out.str().find("ok") != std::string::npos);
        CHECK(err.str().empty());
    }
    std::filesystem::remove_all(workspace);
}

TEST_CASE("host runner writes trace events")
{
    const std::filesystem::path workspace = MakeTempWorkspace();
    {
        ScopedWorkingDirectory scoped(workspace);
        const auto environment = MakeEnvironment({{"DEEPSEEK_API_KEY", "test-key"}});
        std::istringstream input;
        std::ostringstream out;
        std::ostringstream err;
        const std::filesystem::path trace_dir = workspace / "traces";
        const std::vector<std::string> args = {"--trace-dir", trace_dir.string(), "--no-tools", "hi"};

        CHECK(agent::hosts::cli::RunHost(args, environment, input, out, err) == 0);

        std::filesystem::path trace_file;
        for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(trace_dir))
        {
            if (entry.path().extension() == ".jsonl")
            {
                trace_file = entry.path();
            }
        }
        REQUIRE_FALSE(trace_file.empty());
        CHECK(std::filesystem::file_size(trace_file) > 0);
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

        auto environment = MakeEnvironment({{"DEEPSEEK_API_KEY", "test-key"}});
        std::istringstream input;
        std::ostringstream out;
        std::ostringstream err;

        CHECK(agent::hosts::cli::RunHost({"--config", config_path.string(), "hi"}, environment, input, out, err) == 2);
        CHECK(err.str().find("unknown tool") != std::string::npos);
    }
    std::filesystem::remove_all(workspace);
}
