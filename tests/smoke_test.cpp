#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <string>

#include "agent/core/version.h"

TEST_CASE("version 不为空")
{
    CHECK_FALSE(agent::core::Version().empty());
}

TEST_CASE("banner 包含项目名与版本")
{
    const std::string banner = agent::core::BuildBanner();
    CHECK(banner.find("agent-core") != std::string::npos);
    CHECK(banner.find(agent::core::Version()) != std::string::npos);
}
