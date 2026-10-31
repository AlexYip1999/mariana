#include <doctest/doctest.h>

#include <string>

#include "agent/core/chat_model.h"
#include "agent/core/context.h"
#include "agent/core/conversation.h"
#include "agent/core/message.h"
#include "agent/core/tool.h"
#include "agent/core/tracer.h"

namespace
{

class FakeTool : public agent::core::Tool
{
  public:
    std::string Name() const override
    {
        return "fake_tool";
    }

    std::string Description() const override
    {
        return "fake tool";
    }

    std::string ParametersJson() const override
    {
        return R"({"type":"object","properties":{}})";
    }

    agent::core::ToolResult Execute(const std::string& arguments_json) override
    {
        ++m_call_count;
        m_last_arguments = arguments_json;
        agent::core::ToolResult result;
        result.content = "tool-result";
        return result;
    }

    int m_call_count = 0;
    std::string m_last_arguments;
};

class FakeContextProvider : public agent::core::ContextProvider
{
  public:
    agent::core::AgentContextResult BuildContext(const agent::core::AgentContextRequest& request) override
    {
        agent::core::AgentContextResult result;
        result.context_json = "{\"application\":\"" + request.application + "\"}";
        return result;
    }
};

class FakeTracer : public agent::core::Tracer
{
  public:
    void OnModelRequest(const agent::core::ChatRequest& request) override
    {
        ++m_model_requests;
        m_last_model = request.model;
    }

    void OnModelResponse(const agent::core::ChatResponse& response) override
    {
        ++m_model_responses;
        m_last_ok = response.ok;
    }

    void OnToolCall(const agent::core::ToolCall& call) override
    {
        ++m_tool_calls;
        m_last_tool = call.name;
    }

    void OnToolResult(const agent::core::ToolCall&, const agent::core::ToolResult& result) override
    {
        ++m_tool_results;
        m_last_tool_ok = result.ok;
    }

    int m_model_requests = 0;
    int m_model_responses = 0;
    int m_tool_calls = 0;
    int m_tool_results = 0;
    std::string m_last_model;
    std::string m_last_tool;
    bool m_last_ok = false;
    bool m_last_tool_ok = false;
};

} // namespace

TEST_CASE("chat message carries tool calls and tool call id")
{
    agent::core::ChatMessage message;
    message.role = agent::core::ChatRole::Assistant;
    message.content = "calling tool";

    agent::core::ToolCall call;
    call.id = "call_1";
    call.name = "read_file";
    call.arguments_json = R"({"path":"README.md"})";
    message.tool_calls.push_back(call);

    CHECK(message.role == agent::core::ChatRole::Assistant);
    CHECK(message.tool_calls.size() == 1);
    CHECK(message.tool_calls[0].id == "call_1");
    CHECK(message.tool_calls[0].name == "read_file");

    agent::core::ChatMessage tool_message;
    tool_message.role = agent::core::ChatRole::Tool;
    tool_message.tool_call_id = "call_1";
    tool_message.content = "file content";
    CHECK(tool_message.role == agent::core::ChatRole::Tool);
    CHECK(tool_message.tool_call_id == "call_1");
}

TEST_CASE("chat request carries tool specs")
{
    agent::core::ToolSpec spec;
    spec.name = "read_file";
    spec.description = "read a file";
    spec.parameters_json = R"({"type":"object"})";

    agent::core::ChatRequest request;
    request.model = "deepseek-flash";
    request.tools.push_back(spec);

    CHECK(request.tools.size() == 1);
    CHECK(request.tools[0].name == "read_file");
}

TEST_CASE("fake tool implements the tool interface")
{
    FakeTool tool;
    CHECK(tool.Name() == "fake_tool");
    CHECK_FALSE(tool.Description().empty());
    CHECK_FALSE(tool.ParametersJson().empty());

    const agent::core::ToolResult result = tool.Execute(R"({"path":"x"})");

    CHECK(result.ok);
    CHECK(result.content == "tool-result");
    CHECK(tool.m_call_count == 1);
    CHECK(tool.m_last_arguments == R"({"path":"x"})");
}

TEST_CASE("context provider returns application context")
{
    FakeContextProvider provider;
    agent::core::AgentContextRequest request;
    request.application = "ue_editor";
    request.user_input = "hello";

    const agent::core::AgentContextResult result = provider.BuildContext(request);

    CHECK(result.ok);
    CHECK(result.context_json.find("ue_editor") != std::string::npos);
}

TEST_CASE("null tracer is safe to call")
{
    agent::core::NullTracer tracer;
    agent::core::ChatRequest request;
    agent::core::ChatResponse response;
    agent::core::ToolCall call;
    agent::core::ToolResult result;

    tracer.OnModelRequest(request);
    tracer.OnModelResponse(response);
    tracer.OnToolCall(call);
    tracer.OnToolResult(call, result);
}

TEST_CASE("fake tracer records events")
{
    FakeTracer tracer;
    agent::core::ChatRequest request;
    request.model = "deepseek-flash";
    agent::core::ChatResponse response;
    response.ok = true;
    agent::core::ToolCall call;
    call.name = "read_file";
    agent::core::ToolResult result;

    tracer.OnModelRequest(request);
    tracer.OnModelResponse(response);
    tracer.OnToolCall(call);
    tracer.OnToolResult(call, result);

    CHECK(tracer.m_model_requests == 1);
    CHECK(tracer.m_model_responses == 1);
    CHECK(tracer.m_tool_calls == 1);
    CHECK(tracer.m_tool_results == 1);
    CHECK(tracer.m_last_model == "deepseek-flash");
    CHECK(tracer.m_last_tool == "read_file");
}

TEST_CASE("conversation appends and clears messages")
{
    agent::core::Conversation conversation;
    conversation.session_id = "session-1";

    agent::core::ChatMessage first;
    first.role = agent::core::ChatRole::System;
    first.content = "be brief";
    conversation.Append(first);

    agent::core::ChatMessage second;
    second.role = agent::core::ChatRole::User;
    second.content = "hi";
    conversation.Append(second);

    CHECK(conversation.messages.size() == 2);
    CHECK(conversation.messages[0].role == agent::core::ChatRole::System);
    CHECK(conversation.messages[1].content == "hi");

    conversation.Clear();
    CHECK(conversation.messages.empty());
}
