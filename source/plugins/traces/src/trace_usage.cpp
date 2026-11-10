#include "agent/plugins/traces/trace_usage.h"

#include <chrono>
#include <ctime>
#include <fstream>
#include <sstream>
#include <string>

#include <nlohmann/json.hpp>

namespace agent::plugins::traces
{

namespace
{

std::string TodayUtc()
{
    const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &now);
#else
    gmtime_r(&now, &utc);
#endif
    char buffer[16] = {};
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d", &utc);
    return buffer;
}

bool MatchesRange(const std::string& timestamp, const std::string& range)
{
    if (range == "all")
    {
        return true;
    }
    return timestamp.size() >= 10 && timestamp.substr(0, 10) == TodayUtc();
}

} // namespace

UsageSummary ScanTraceUsage(const std::filesystem::path& trace_directory, const std::string& range)
{
    UsageSummary summary;

    std::error_code error;
    if (!std::filesystem::is_directory(trace_directory, error))
    {
        return summary;
    }

    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(trace_directory))
    {
        if (!entry.is_regular_file() || entry.path().extension() != ".jsonl")
        {
            continue;
        }

        bool session_counted = false;
        std::ifstream file(entry.path());
        std::string line;
        while (std::getline(file, line))
        {
            try
            {
                const nlohmann::json event = nlohmann::json::parse(line);
                const std::string timestamp = event.value("ts", std::string());
                if (!MatchesRange(timestamp, range))
                {
                    continue;
                }

                const std::string event_name = event.value("event", std::string());
                if (event_name == "turn_start")
                {
                    ++summary.turns;
                }
                else if (event_name == "model_response")
                {
                    ++summary.model_calls;
                    if (event.contains("usage") && event["usage"].is_object())
                    {
                        const nlohmann::json& usage = event["usage"];
                        summary.prompt_tokens += usage.value("prompt_tokens", std::uint64_t{0});
                        summary.completion_tokens += usage.value("completion_tokens", std::uint64_t{0});
                        summary.total_tokens += usage.value("total_tokens", std::uint64_t{0});
                        summary.cache_hit_tokens += usage.value("prompt_cache_hit_tokens", std::uint64_t{0});
                        summary.cache_miss_tokens += usage.value("prompt_cache_miss_tokens", std::uint64_t{0});
                    }
                }

                // 每个 trace 文件只计一次 session。
                if (!session_counted)
                {
                    ++summary.sessions;
                    session_counted = true;
                }
            }
            catch (const nlohmann::json::exception&)
            {
                continue;
            }
        }
    }

    return summary;
}

std::string FormatUsageSummary(const UsageSummary& summary, const std::string& range)
{
    std::ostringstream stream;
    stream << "usage (" << range << "): sessions=" << summary.sessions << " turns=" << summary.turns
           << " model_calls=" << summary.model_calls << " prompt_tokens=" << summary.prompt_tokens
           << " completion_tokens=" << summary.completion_tokens << " total_tokens=" << summary.total_tokens
           << " cache_hit=" << summary.cache_hit_tokens << " cache_miss=" << summary.cache_miss_tokens;
    return stream.str();
}

} // namespace agent::plugins::traces
