#pragma once

#include <cstddef>

#include "agent/core/summarizer.h"

namespace agent::plugins::memory
{

/// 规则式摘要：离线、确定性、零模型调用。压缩最旧的若干轮，保留最近 max_items 条非空消息。
/// 适合作为默认实现和模型摘要失败时的降级方案。
class RuleBasedSummarizer final : public core::Summarizer
{
  public:
    explicit RuleBasedSummarizer(std::size_t max_items = 6);

    core::SummaryResult Summarize(const core::SummaryRequest& request) override;

  private:
    std::size_t m_max_items;
};

} // namespace agent::plugins::memory
