#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include "agent/model/openai_chat_model.h"
#include "support/fake_http_transport.h"

namespace
{

agent::model::OpenAiConfig MakeConfig()
{
    agent::model::OpenAiConfig config;
    config.endpoint = "https://api.deepseek.com/chat/completions";
    config.model = "deepseek-flash";
    config.api_key = "test-key";
    config.timeout_ms = 15000;
    return config;
}

agent::core::ChatRequest MakeChatRequest()
{
    agent::core::ChatRequest request;
    request.messages = {
        {
            agent::core::ChatRole::System,
            "be brief",
        },
        {
            agent::core::ChatRole::User,
            "hi",
        },
    };
    request.max_tokens = 64;
    return request;
}

bool HasHeader(const agent::transport::HttpRequest& request, const std::string& name, const std::string& value)
{
    for (const auto& [key, header_value] : request.headers)
    {
        if (key == name)
        {
            return header_value == value;
        }
    }
    return false;
}

} // namespace

TEST_CASE("builds OpenAI-compatible request and parses content")
{
    agent::tests::FakeHttpTransport transport;
    transport.SetHandler(
        [](const agent::transport::HttpRequest&)
        {
            nlohmann::json parsed;
            parsed["choices"] = nlohmann::json::array();
            parsed["choices"].push_back({{"message", {{"content", "hello"}}}});
            parsed["usage"]["prompt_tokens"] = 120;
            parsed["usage"]["completion_tokens"] = 20;
            parsed["usage"]["total_tokens"] = 140;
            parsed["usage"]["prompt_cache_hit_tokens"] = 100;
            parsed["usage"]["prompt_cache_miss_tokens"] = 20;
            agent::transport::HttpResponse response;
            response.status = 200;
            response.body = parsed.dump();
            return response;
        });

    agent::model::OpenAiChatModel adapter(transport, MakeConfig());
    const agent::core::ChatResponse response = adapter.Chat(MakeChatRequest());

    CHECK(response.ok);
    CHECK(response.content == "hello");
    CHECK(response.usage.prompt_tokens == 120);
    CHECK(response.usage.completion_tokens == 20);
    CHECK(response.usage.total_tokens == 140);
    CHECK(response.usage.prompt_cache_hit_tokens == 100);
    CHECK(response.usage.prompt_cache_miss_tokens == 20);
    CHECK(transport.RequestCount() == 1);

    const agent::transport::HttpRequest& request = transport.LastRequest();
    CHECK(request.url == "https://api.deepseek.com/chat/completions");
    CHECK(request.timeout_ms == 15000);
    CHECK(HasHeader(request, "Content-Type", "application/json"));
    CHECK(HasHeader(request, "Authorization", "Bearer test-key"));

    const nlohmann::json body = nlohmann::json::parse(request.body);
    CHECK(body["model"] == "deepseek-flash");
    CHECK(body["stream"] == false);
    CHECK(body["messages"].size() == 2);
    CHECK(body["messages"][0]["role"] == "system");
    CHECK(body["messages"][1]["role"] == "user");
    CHECK(body["messages"][1]["content"] == "hi");
}

TEST_CASE("serializes tools and tool messages")
{
    agent::tests::FakeHttpTransport transport;
    transport.SetHandler(
        [](const agent::transport::HttpRequest&)
        {
            nlohmann::json parsed;
            parsed["choices"] = nlohmann::json::array();
            parsed["choices"].push_back({{"message", {{"content", "done"}}}});
            agent::transport::HttpResponse response;
            response.status = 200;
            response.body = parsed.dump();
            return response;
        });

    agent::core::ChatRequest request = MakeChatRequest();
    agent::core::ChatMessage assistant;
    assistant.role = agent::core::ChatRole::Assistant;
    assistant.content = "";
    agent::core::ToolCall call;
    call.id = "call_1";
    call.name = "read_file";
    call.arguments_json = R"({"path":"README.md"})";
    assistant.tool_calls.push_back(call);
    request.messages.push_back(assistant);

    agent::core::ChatMessage tool_message;
    tool_message.role = agent::core::ChatRole::Tool;
    tool_message.tool_call_id = "call_1";
    tool_message.content = "ok";
    request.messages.push_back(tool_message);

    agent::core::ToolSpec spec;
    spec.name = "read_file";
    spec.description = "Read a file";
    spec.parameters_json = R"({"type":"object","properties":{"path":{"type":"string"}}})";
    request.tools.push_back(spec);

    agent::model::OpenAiChatModel adapter(transport, MakeConfig());
    const agent::core::ChatResponse response = adapter.Chat(request);

    CHECK(response.ok);
    CHECK(response.content == "done");

    const nlohmann::json body = nlohmann::json::parse(transport.LastRequest().body);
    CHECK(body["tools"].size() == 1);
    CHECK(body["tools"][0]["type"] == "function");
    CHECK(body["tools"][0]["function"]["name"] == "read_file");
    CHECK(body["tools"][0]["function"]["parameters"]["type"] == "object");
    CHECK(body["messages"][2]["role"] == "assistant");
    CHECK(body["messages"][2]["tool_calls"][0]["id"] == "call_1");
    CHECK(body["messages"][2]["tool_calls"][0]["function"]["name"] == "read_file");
    CHECK(body["messages"][3]["role"] == "tool");
    CHECK(body["messages"][3]["tool_call_id"] == "call_1");
    CHECK(body["messages"][3]["content"] == "ok");
}

TEST_CASE("rejects invalid tool schema before sending")
{
    agent::tests::FakeHttpTransport transport;
    agent::model::OpenAiChatModel adapter(transport, MakeConfig());

    agent::core::ChatRequest request = MakeChatRequest();
    agent::core::ToolSpec spec;
    spec.name = "read_file";
    spec.parameters_json = "{";
    request.tools.push_back(spec);

    const agent::core::ChatResponse response = adapter.Chat(request);

    CHECK_FALSE(response.ok);
    CHECK(response.error.find("invalid tool parameters") != std::string::npos);
    CHECK(transport.RequestCount() == 0);
}

TEST_CASE("does not crash on invalid UTF-8 message content")
{
    agent::tests::FakeHttpTransport transport;
    transport.SetHandler(
        [](const agent::transport::HttpRequest&)
        {
            nlohmann::json parsed;
            parsed["choices"] = nlohmann::json::array();
            parsed["choices"].push_back({{"message", {{"content", "ok"}}}});
            agent::transport::HttpResponse response;
            response.status = 200;
            response.body = parsed.dump();
            return response;
        });

    agent::core::ChatRequest request = MakeChatRequest();
    std::string invalid_utf8;
    invalid_utf8.push_back(static_cast<char>(0x80));
    request.messages[1].content = invalid_utf8;

    agent::model::OpenAiChatModel adapter(transport, MakeConfig());
    const agent::core::ChatResponse response = adapter.Chat(request);

    CHECK(response.ok);
    CHECK(response.content == "ok");
    CHECK(transport.RequestCount() == 1);
}

TEST_CASE("rejects malformed tool_calls response")
{
    agent::tests::FakeHttpTransport transport;
    transport.SetHandler(
        [](const agent::transport::HttpRequest&)
        {
            nlohmann::json function;
            function["name"] = "read_file";
            nlohmann::json call;
            call["function"] = function;
            nlohmann::json message;
            message["content"] = nullptr;
            message["tool_calls"] = nlohmann::json::array({call});
            nlohmann::json parsed;
            parsed["choices"] = nlohmann::json::array();
            parsed["choices"].push_back({{"message", message}});
            agent::transport::HttpResponse response;
            response.status = 200;
            response.body = parsed.dump();
            return response;
        });

    agent::model::OpenAiChatModel adapter(transport, MakeConfig());
    const agent::core::ChatResponse response = adapter.Chat(MakeChatRequest());

    CHECK_FALSE(response.ok);
    CHECK(response.error.find("invalid tool_calls") != std::string::npos);
}

TEST_CASE("parses tool calls with null content")
{
    agent::tests::FakeHttpTransport transport;
    transport.SetHandler(
        [](const agent::transport::HttpRequest&)
        {
            nlohmann::json arguments;
            arguments["path"] = "README.md";
            nlohmann::json function;
            function["name"] = "read_file";
            function["arguments"] = arguments.dump();
            nlohmann::json call;
            call["id"] = "call_1";
            call["type"] = "function";
            call["function"] = function;
            nlohmann::json message;
            message["content"] = nullptr;
            message["tool_calls"] = nlohmann::json::array({call});
            nlohmann::json parsed;
            parsed["choices"] = nlohmann::json::array();
            parsed["choices"].push_back({{"message", message}});
            agent::transport::HttpResponse response;
            response.status = 200;
            response.body = parsed.dump();
            return response;
        });

    agent::model::OpenAiChatModel adapter(transport, MakeConfig());
    const agent::core::ChatResponse response = adapter.Chat(MakeChatRequest());

    CHECK(response.ok);
    CHECK(response.content.empty());
    CHECK(response.tool_calls.size() == 1);
    CHECK(response.tool_calls[0].id == "call_1");
    CHECK(response.tool_calls[0].name == "read_file");
    CHECK(response.tool_calls[0].arguments_json == R"({"path":"README.md"})");
}

TEST_CASE("parses OpenAI-style cached tokens")
{
    agent::tests::FakeHttpTransport transport;
    transport.SetHandler(
        [](const agent::transport::HttpRequest&)
        {
            nlohmann::json parsed;
            parsed["choices"] = nlohmann::json::array();
            parsed["choices"].push_back({{"message", {{"content", "hello"}}}});
            parsed["usage"]["prompt_tokens"] = 100;
            parsed["usage"]["completion_tokens"] = 10;
            parsed["usage"]["total_tokens"] = 110;
            parsed["usage"]["prompt_tokens_details"]["cached_tokens"] = 80;
            agent::transport::HttpResponse response;
            response.status = 200;
            response.body = parsed.dump();
            return response;
        });

    agent::model::OpenAiChatModel adapter(transport, MakeConfig());
    const agent::core::ChatResponse response = adapter.Chat(MakeChatRequest());

    CHECK(response.ok);
    CHECK(response.usage.prompt_cache_hit_tokens == 80);
    CHECK(response.usage.prompt_cache_miss_tokens == 20);
}

TEST_CASE("returns transport error unchanged")
{
    agent::tests::FakeHttpTransport transport;
    transport.SetHandler(
        [](const agent::transport::HttpRequest&)
        {
            agent::transport::HttpResponse response;
            response.error = "timeout";
            return response;
        });

    agent::model::OpenAiChatModel adapter(transport, MakeConfig());
    const agent::core::ChatResponse response = adapter.Chat(MakeChatRequest());

    CHECK_FALSE(response.ok);
    CHECK(response.error == "timeout");
}

TEST_CASE("reports HTTP status and error message")
{
    agent::tests::FakeHttpTransport transport;
    transport.SetHandler(
        [](const agent::transport::HttpRequest&)
        {
            nlohmann::json parsed;
            parsed["error"]["message"] = "bad key";
            agent::transport::HttpResponse response;
            response.status = 401;
            response.body = parsed.dump();
            return response;
        });

    agent::model::OpenAiChatModel adapter(transport, MakeConfig());
    const agent::core::ChatResponse response = adapter.Chat(MakeChatRequest());

    CHECK_FALSE(response.ok);
    CHECK(response.error.find("401") != std::string::npos);
    CHECK(response.error.find("bad key") != std::string::npos);
}

TEST_CASE("rejects invalid JSON response")
{
    agent::tests::FakeHttpTransport transport;
    transport.SetHandler(
        [](const agent::transport::HttpRequest&)
        {
            agent::transport::HttpResponse response;
            response.status = 200;
            response.body = "not-json";
            return response;
        });

    agent::model::OpenAiChatModel adapter(transport, MakeConfig());
    const agent::core::ChatResponse response = adapter.Chat(MakeChatRequest());

    CHECK_FALSE(response.ok);
    CHECK(response.error.find("invalid JSON") != std::string::npos);
}

TEST_CASE("rejects empty API key before sending")
{
    agent::tests::FakeHttpTransport transport;
    agent::model::OpenAiConfig config = MakeConfig();
    config.api_key.clear();
    agent::model::OpenAiChatModel adapter(transport, config);

    const agent::core::ChatResponse response = adapter.Chat(MakeChatRequest());

    CHECK_FALSE(response.ok);
    CHECK(response.error == "API key is empty");
    CHECK(transport.RequestCount() == 0);
}
