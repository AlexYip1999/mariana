#include <doctest/doctest.h>

#include "agent/hosts/cli/cli_options.h"

TEST_CASE("parses chat question")
{
    const agent::hosts::cli::CliParseResult result = agent::hosts::cli::ParseCliOptions({"hello", "world"});

    CHECK(result.ok);
    CHECK(result.options.mode == agent::hosts::cli::CliMode::Chat);
    CHECK(result.options.config_path.empty());
    CHECK(result.options.question.size() == 2);
    CHECK(result.options.question[0] == "hello");
    CHECK(result.options.question[1] == "world");
}

TEST_CASE("parses stats option")
{
    const agent::hosts::cli::CliParseResult result = agent::hosts::cli::ParseCliOptions({"--stats", "hello"});

    CHECK(result.ok);
    CHECK(result.options.stats);
    CHECK(result.options.question.size() == 1);
}

TEST_CASE("parses interactive mode and config path")
{
    const agent::hosts::cli::CliParseResult result =
        agent::hosts::cli::ParseCliOptions({"--interactive", "--config", "my.json"});

    CHECK(result.ok);
    CHECK(result.options.mode == agent::hosts::cli::CliMode::Interactive);
    CHECK(result.options.config_path == "my.json");
}

TEST_CASE("parses init config mode")
{
    const agent::hosts::cli::CliParseResult result = agent::hosts::cli::ParseCliOptions({"--init-config"});

    CHECK(result.ok);
    CHECK(result.options.mode == agent::hosts::cli::CliMode::InitConfig);
}

TEST_CASE("requires config path")
{
    const agent::hosts::cli::CliParseResult result = agent::hosts::cli::ParseCliOptions({"--config"});

    CHECK_FALSE(result.ok);
    CHECK(result.error.find("requires a path") != std::string::npos);
}

TEST_CASE("rejects unknown option")
{
    const agent::hosts::cli::CliParseResult result = agent::hosts::cli::ParseCliOptions({"--unknown"});

    CHECK_FALSE(result.ok);
    CHECK(result.error.find("unknown option") != std::string::npos);
}

TEST_CASE("supports end of options marker")
{
    const agent::hosts::cli::CliParseResult result = agent::hosts::cli::ParseCliOptions({"--", "-not-an-option"});

    CHECK(result.ok);
    CHECK(result.options.mode == agent::hosts::cli::CliMode::Chat);
    CHECK(result.options.question.size() == 1);
    CHECK(result.options.question[0] == "-not-an-option");
}

TEST_CASE("rejects conflicting modes")
{
    const agent::hosts::cli::CliParseResult init_and_interactive =
        agent::hosts::cli::ParseCliOptions({"--init-config", "--interactive"});
    CHECK_FALSE(init_and_interactive.ok);

    const agent::hosts::cli::CliParseResult interactive_with_question =
        agent::hosts::cli::ParseCliOptions({"--interactive", "hello"});
    CHECK_FALSE(interactive_with_question.ok);

    const agent::hosts::cli::CliParseResult init_with_question =
        agent::hosts::cli::ParseCliOptions({"--init-config", "hello"});
    CHECK_FALSE(init_with_question.ok);
}

TEST_CASE("help takes precedence")
{
    const agent::hosts::cli::CliParseResult result = agent::hosts::cli::ParseCliOptions({"--help", "--interactive"});

    CHECK(result.ok);
    CHECK(result.options.mode == agent::hosts::cli::CliMode::Help);
}
