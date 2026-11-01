#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/plugins/traces/file_tracer.h"

TEST_CASE("file tracer writes redacted jsonl events")
{
    const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() / ("agent-core-trace-" + std::to_string(stamp));
    std::filesystem::remove_all(directory);

    agent::plugins::traces::FileTracer tracer(directory, "test-session");
    REQUIRE(tracer.IsOpen());

    agent::core::ChatRequest request;
    request.model = "deepseek-flash";
    request.messages.push_back({agent::core::ChatRole::User, "hello"});

    agent::core::ChatResponse response;
    response.ok = true;
    response.content = "token sk-1234567890abcdef";
    response.finish_reason = "stop";
    response.usage.total_tokens = 12;

    agent::core::ToolCall call;
    call.id = "call_1";
    call.name = "read_file";
    call.arguments_json = R"({"path":"README.md"})";

    agent::core::ToolResult tool_result;
    tool_result.content = "ok sk-abcdef1234567890";

    tracer.OnTurnStart();
    tracer.OnModelRequest(request);
    tracer.OnModelResponse(response);
    tracer.OnToolCall(call);
    tracer.OnToolResult(call, tool_result);
    tracer.OnTurnEnd(true, {}, 1);

    std::ifstream file(tracer.Path());
    REQUIRE(file.is_open());

    std::vector<nlohmann::json> events;
    std::string line;
    while (std::getline(file, line))
    {
        events.push_back(nlohmann::json::parse(line));
    }

    REQUIRE(events.size() == 6);
    CHECK(events[0]["event"] == "turn_start");
    CHECK(events[1]["event"] == "model_request");
    CHECK(events[2]["event"] == "model_response");
    CHECK(events[3]["event"] == "tool_call");
    CHECK(events[4]["event"] == "tool_result");
    CHECK(events[5]["event"] == "turn_end");
    CHECK(events[2]["content"].get<std::string>().find("[REDACTED]") != std::string::npos);
    CHECK(events[4]["content"].get<std::string>().find("[REDACTED]") != std::string::npos);
    CHECK(events[2]["usage"]["total_tokens"] == 12);

    file.close();
    tracer.Close();
    std::filesystem::remove_all(directory);
}
