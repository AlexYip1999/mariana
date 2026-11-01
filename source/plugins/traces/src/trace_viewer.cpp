#include "agent/plugins/traces/trace_viewer.h"

#include <fstream>
#include <ostream>
#include <string>

#include <nlohmann/json.hpp>

namespace agent::plugins::traces
{

namespace
{

std::string Preview(const std::string& text, std::size_t limit)
{
    if (text.size() <= limit)
    {
        return text;
    }
    return text.substr(0, limit) + "...";
}

} // namespace

bool RenderTraceFile(const std::filesystem::path& path, std::ostream& out, std::string& error)
{
    error.clear();
    std::ifstream file(path);
    if (!file.is_open())
    {
        error = "cannot open trace file: " + path.string();
        return false;
    }

    out << "trace: " << path.string() << "\n";
    std::string line;
    while (std::getline(file, line))
    {
        try
        {
            const nlohmann::json event = nlohmann::json::parse(line);
            const std::string name = event.value("event", std::string());
            const std::string timestamp = event.value("ts", std::string());
            const int turn = event.value("turn", 0);
            const int step = event.value("step", 0);

            if (name == "turn_start")
            {
                out << "\nTurn " << turn << "\n";
                continue;
            }
            if (name == "turn_end")
            {
                out << "  turn_end ok=" << event.value("ok", false) << " steps=" << event.value("steps", 0)
                    << " duration=" << event.value("duration_ms", 0) << "ms\n";
                continue;
            }
            if (name == "model_request")
            {
                out << "  Step " << step << " model_request model=" << event.value("model", std::string())
                    << " messages=" << event.value("message_count", 0) << " tools=" << event.value("tool_count", 0)
                    << "\n";
                continue;
            }
            if (name == "model_response")
            {
                out << "    model_response ok=" << event.value("ok", false)
                    << " finish=" << event.value("finish_reason", std::string())
                    << " duration=" << event.value("duration_ms", 0) << "ms"
                    << " content=\"" << Preview(event.value("content", std::string()), 80) << "\"\n";
                continue;
            }
            if (name == "tool_call")
            {
                out << "    tool_call " << event.value("name", std::string()) << " args=\""
                    << Preview(event.value("arguments", std::string()), 80) << "\"\n";
                continue;
            }
            if (name == "tool_result")
            {
                out << "    tool_result " << event.value("name", std::string()) << " ok=" << event.value("ok", false)
                    << " duration=" << event.value("duration_ms", 0) << "ms"
                    << " content=\"" << Preview(event.value("content", std::string()), 80) << "\"\n";
                continue;
            }
            out << "  [" << timestamp << "] " << name << "\n";
        }
        catch (const nlohmann::json::exception& exception)
        {
            error = std::string("invalid trace JSON: ") + exception.what();
            return false;
        }
    }
    return true;
}

} // namespace agent::plugins::traces
