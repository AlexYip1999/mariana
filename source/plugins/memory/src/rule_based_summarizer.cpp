#include "agent/plugins/memory/rule_based_summarizer.h"

#include <algorithm>
#include <string>
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

} // namespace

RuleBasedSummarizer::RuleBasedSummarizer(std::size_t max_items)
    : m_max_items(max_items)
{}

core::SummaryResult RuleBasedSummarizer::Summarize(const core::SummaryRequest& request)
{
    core::SummaryResult result;
    std::vector<std::string> lines;
    for (auto it = request.turns.rbegin(); it != request.turns.rend() && lines.size() < m_max_items; ++it)
    {
        if (it->content.empty() || it->role == core::ChatRole::Tool)
        {
            continue;
        }
        lines.push_back("- " + RoleName(it->role) + ": " + it->content);
    }

    std::reverse(lines.begin(), lines.end());

    for (const std::string& line : lines)
    {
        result.summary += line + "\n";
    }
    if (!result.summary.empty())
    {
        result.summary.pop_back();
    }

    // 超出字符上限时按 UTF-8 边界安全截断，避免产生非法 UTF-8。
    if (request.max_chars > 0 && result.summary.size() > request.max_chars)
    {
        result.summary.resize(core::Utf8PrefixSize(result.summary, request.max_chars));
    }

    return result;
}

} // namespace agent::plugins::memory
