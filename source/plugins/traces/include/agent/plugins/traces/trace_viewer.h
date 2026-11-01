#pragma once

#include <filesystem>
#include <iosfwd>
#include <string>

namespace agent::plugins::traces
{

/// 把 JSONL trace 渲染成人可读的时间线。
bool RenderTraceFile(const std::filesystem::path& path, std::ostream& out, std::string& error);

} // namespace agent::plugins::traces
