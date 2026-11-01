#include "agent/plugins/traces/trace_sink.h"

#include <system_error>
#include <utility>

namespace agent::plugins::traces
{

JsonlFileSink::JsonlFileSink(std::filesystem::path file_path)
    : m_path(std::move(file_path))
{
    std::error_code error;
    std::filesystem::create_directories(m_path.parent_path(), error);
    if (!error)
    {
        m_file.open(m_path, std::ios::out | std::ios::app | std::ios::binary);
    }
}

bool JsonlFileSink::IsOpen() const
{
    return m_file.is_open();
}

void JsonlFileSink::Write(const std::string& json_line)
{
    if (!m_file.is_open())
    {
        return;
    }
    m_file << json_line << "\n";
    m_file.flush();
}

void JsonlFileSink::Close()
{
    if (m_file.is_open())
    {
        m_file.flush();
        m_file.close();
    }
}

const std::filesystem::path& JsonlFileSink::Path() const
{
    return m_path;
}

} // namespace agent::plugins::traces
