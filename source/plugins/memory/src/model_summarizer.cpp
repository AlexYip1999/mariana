#include "agent/plugins/memory/model_summarizer.h"

#include <exception>
#include <utility>
#include <vector>

#include "agent/core/utf8.h"

namespace agent::plugins::memory
{

namespace
{

std::string RoleName(core::ChatRole role)
{
    switch (role)
    {
        case core::ChatRole::System:
        {
            return "system";
        }
        case core::ChatRole::User:
        {
            return "user";
        }
        case core::ChatRole::Assistant:
        {
            return "assistant";
        }
        case core::ChatRole::Tool:
        {
            return "tool";
        }
    }
    return "unknown";
}

std::string RenderTurns(const std::vector<core::ChatMessage>& turns)
{
    std::string rendered;
    for (const core::ChatMessage& message : turns)
    {
        if (message.content.empty())
        {
            continue;
        }
        rendered += "- " + RoleName(message.role) + ": " + message.content + "\n";
    }
    return rendered;
}

core::SummaryResult Failure(std::string error)
{
    core::SummaryResult result;
    result.ok = false;
    result.error = std::move(error);
    return result;
}

} // namespace

ModelSummarizer::ModelSummarizer(std::shared_ptr<core::ChatModel> model, Config config,
                                 std::shared_ptr<core::Summarizer> fallback)
    : m_model(std::move(model))
    , m_config(std::move(config))
    , m_fallback(std::move(fallback))
{}

core::SummaryResult ModelSummarizer::Fallback(const core::SummaryRequest& request, std::string reason) const
{
    if (!m_fallback)
    {
        return Failure(std::move(reason));
    }
    return m_fallback->Summarize(request);
}

core::SummaryResult ModelSummarizer::Summarize(const core::SummaryRequest& request)
{
    if (!m_model)
    {
        return Fallback(request, "model summarizer has no ChatModel");
    }

    // 采样参数固定：temperature 0 保证可复现，输出上限由 Config 决定。
    core::ChatRequest chat;
    chat.model = m_config.model;
    chat.temperature = 0.0;
    chat.max_tokens = m_config.max_tokens;
    chat.call = request.call;

    // 系统消息携带“只重述、不新增”的约束，用户消息放待压缩轮次。
    core::ChatMessage system_message;
    system_message.role = core::ChatRole::System;
    system_message.content = m_config.system_prompt;
    chat.messages.push_back(std::move(system_message));

    core::ChatMessage user_message;
    user_message.role = core::ChatRole::User;
    user_message.content = "Compress these old conversation turns:\n\n" + RenderTurns(request.turns);
    chat.messages.push_back(std::move(user_message));

    // 模型异常、失败或空结果都视为不可用，交给 fallback（若配置）。
    core::ChatResponse response;
    try
    {
        response = m_model->Chat(chat);
    }
    catch (const std::exception& error)
    {
        return Fallback(request, std::string("summarizer model threw: ") + error.what());
    }
    catch (...)
    {
        return Fallback(request, "summarizer model threw an unknown exception");
    }
    if (!response.ok)
    {
        const std::string reason =
            response.error.empty() ? "summarizer model failed" : "summarizer model failed: " + response.error;
        return Fallback(request, reason);
    }
    if (response.content.empty())
    {
        return Fallback(request, "summarizer model returned empty content");
    }

    core::SummaryResult result;
    result.ok = true;
    result.summary = std::move(response.content);
    if (request.max_chars > 0 && result.summary.size() > request.max_chars)
    {
        result.summary.resize(core::Utf8PrefixSize(result.summary, request.max_chars));
    }
    return result;
}

} // namespace agent::plugins::memory
