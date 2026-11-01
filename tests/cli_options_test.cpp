#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "agent/hosts/cli/cli_options.h"

TEST_CASE("parses minimal bootstrap options")
{
    const agent::hosts::cli::CliParseResult result =
        agent::hosts::cli::ParseCliOptions({"--help", "--version", "--config", "my.json"});

    CHECK(result.ok);
    CHECK(result.options.help);
    CHECK(result.options.version);
    CHECK(result.options.config_path == "my.json");
}

TEST_CASE("requires config value")
{
    const agent::hosts::cli::CliParseResult config = agent::hosts::cli::ParseCliOptions({"--config"});
    CHECK_FALSE(config.ok);
    CHECK(config.error.find("requires a value") != std::string::npos);

    const agent::hosts::cli::CliParseResult empty = agent::hosts::cli::ParseCliOptions({"--config", ""});
    CHECK_FALSE(empty.ok);
    CHECK(empty.error.find("non-empty") != std::string::npos);
}

TEST_CASE("rejects old startup options")
{
    const std::vector<std::string> options = {"--interactive", "--workspace",   "--no-tools", "--stats",
                                              "--trace",       "--init-config", "--session"};
    for (const std::string& option : options)
    {
        const agent::hosts::cli::CliParseResult result = agent::hosts::cli::ParseCliOptions({option});
        CHECK_FALSE(result.ok);
        CHECK(result.error.find("unknown argument") != std::string::npos);
    }
}

TEST_CASE("rejects positional question")
{
    const agent::hosts::cli::CliParseResult result = agent::hosts::cli::ParseCliOptions({"hello"});

    CHECK_FALSE(result.ok);
    CHECK(result.error.find("交互模式") != std::string::npos);
}
