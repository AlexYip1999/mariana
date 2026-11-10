#pragma once

#include <memory>
#include <string>

#include "agent/core/chat_model.h"
#include "agent/core/summarizer.h"

namespace agent::plugins::memory
{

/// 模型摘要：复用 ChatModel（可单独配置便宜模型）压缩旧轮次。
/// 失败时可退回 fallback（例如 RuleBasedSummarizer）；没有 fallback 时返回 ok=false。
class ModelSummarizer final : public core::Summarizer
{
  public:
    struct Config
    {
        // 采样：摘要模型名（空 = 由 ChatModel 适配器决定）与输出上限
        std::string model;
        int max_tokens = 256;

        // 提示词：只允许基于给定轮次重述，不新增事实
        std::string system_prompt = "You compress old conversation turns into a short factual summary. "
                                    "Keep facts, commitments, open tasks and relationships. "
                                    "Only restate what the turns contain; never invent new information. "
                                    "Answer in the same language as the conversation.";
    };

    ModelSummarizer(std::shared_ptr<core::ChatModel> model, Config config,
                    std::shared_ptr<core::Summarizer> fallback = nullptr);

    core::SummaryResult Summarize(const core::SummaryRequest& request) override;

  private:
    core::SummaryResult Fallback(const core::SummaryRequest& request, std::string reason) const;

    std::shared_ptr<core::ChatModel> m_model;
    Config m_config;
    std::shared_ptr<core::Summarizer> m_fallback;
};

} // namespace agent::plugins::memory
