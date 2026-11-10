#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/plugins/traces/file_tracer.h"
#include "agent/plugins/traces/trace_usage.h"
#include "agent/plugins/traces/trace_viewer.h"

namespace
{

class MemoryTraceSink final : public agent::plugins::traces::TraceSink
{
  public:
    bool IsOpen() const override
    {
        return true;
    }

    void Write(const std::string& line) override
    {
        events.push_back(nlohmann::json::parse(line));
    }

    void Close() override {}

    std::vector<nlohmann::json> events;
};

} // namespace

TEST_CASE("trace truncation preserves Chinese and emoji at every byte boundary")
{
    const std::string characters[] = {"中", "😀"};
    for (const std::string& character : characters)
    {
        for (std::size_t prefix = 0; prefix < character.size(); ++prefix)
        {
            auto sink = std::make_shared<MemoryTraceSink>();
            agent::plugins::traces::FileTracer tracer(sink);
            std::string content(prefix, 'a');
            for (int index = 0; index < 700; ++index)
            {
                content += character;
            }

            agent::core::ChatRequest request;
            request.messages.push_back({agent::core::ChatRole::User, content});
            agent::core::ChatResponse response;
            response.content = content;
            agent::core::ToolResult tool_result;
            tool_result.content = content;

            tracer.OnTurnStart();
            CHECK_NOTHROW(tracer.OnModelRequest(request));
            CHECK_NOTHROW(tracer.OnModelResponse(response));
            CHECK_NOTHROW(tracer.OnToolResult({}, tool_result));
            CHECK_NOTHROW(tracer.OnTurnEnd(false, content, 1));
            REQUIRE(sink->events.size() == 5);

            const std::size_t kept = prefix + (2000 - prefix) / character.size() * character.size();
            const std::string expected = content.substr(0, kept) + "... [truncated]";
            CHECK(sink->events[1]["last_content"] == expected);
            CHECK(sink->events[2]["content"] == expected);
            CHECK(sink->events[3]["content"] == expected);
            CHECK(sink->events[4]["error"] == expected);
        }
    }
}

TEST_CASE("file tracer correlates structured turn events with session and run ids")
{
    auto sink = std::make_shared<MemoryTraceSink>();
    agent::plugins::traces::FileTracer tracer(sink);

    agent::core::TurnEvent start;
    start.session_id = "session-1";
    start.run_id = "run-1";
    tracer.OnTurnStarted(start);

    agent::core::TurnEvent end;
    end.session_id = "session-1";
    end.run_id = "run-1";
    end.status = "Failed";
    end.error_code = "ModelFailure";
    end.error = "boom";
    end.steps = 2;
    end.tool_attempts = 1;
    end.replayed = true;
    tracer.OnTurnEnded(end);

    REQUIRE(sink->events.size() == 2);

    CHECK(sink->events[0]["event"] == "turn_start");
    CHECK(sink->events[0]["session_id"] == "session-1");
    CHECK(sink->events[0]["run_id"] == "run-1");

    CHECK(sink->events[1]["event"] == "turn_end");
    CHECK(sink->events[1]["session_id"] == "session-1");
    CHECK(sink->events[1]["run_id"] == "run-1");
    CHECK(sink->events[1]["status"] == "Failed");
    CHECK(sink->events[1]["error_code"] == "ModelFailure");
    CHECK(sink->events[1]["tool_attempts"] == 1);
    CHECK(sink->events[1]["replayed"] == true);
}

TEST_CASE("file tracer marks replayed tool results")
{
    auto sink = std::make_shared<MemoryTraceSink>();
    agent::plugins::traces::FileTracer tracer(sink);

    agent::core::ToolCall call;
    call.id = "call_1";
    call.name = "echo";
    call.arguments_json = "{}";
    agent::core::ToolResult result;
    result.ok = true;
    result.content = "cached";

    tracer.OnTurnStart();
    tracer.OnToolReplayed(call, result);

    REQUIRE(sink->events.size() == 2);

    CHECK(sink->events[1]["event"] == "tool_result");
    CHECK(sink->events[1]["replayed"] == true);
    CHECK(sink->events[1]["content"] == "cached");
}

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

    const agent::plugins::traces::UsageSummary summary = agent::plugins::traces::ScanTraceUsage(directory, "all");
    CHECK(summary.sessions == 1);
    CHECK(summary.turns == 1);
    CHECK(summary.model_calls == 1);
    CHECK(summary.total_tokens == 12);
    CHECK(agent::plugins::traces::FormatUsageSummary(summary, "all").find("usage (all)") != std::string::npos);

    std::ostringstream rendered;
    std::string render_error;
    const bool rendered_ok = agent::plugins::traces::RenderTraceFile(tracer.Path(), rendered, render_error);
    INFO("render error: " << render_error);
    CHECK(rendered_ok);
    CHECK(render_error.empty());
    CHECK(rendered.str().find("Turn 1") != std::string::npos);
    CHECK(rendered.str().find("model_response") != std::string::npos);

    file.close();
    tracer.Close();
    std::filesystem::remove_all(directory);
}
