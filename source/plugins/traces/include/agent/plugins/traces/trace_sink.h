#pragma once

#include <filesystem>
#include <fstream>
#include <string>

namespace agent::plugins::traces
{

/// trace 事件输出的可替换 sink。
class TraceSink
{
  public:
    virtual ~TraceSink() = default;

    virtual bool IsOpen() const = 0;
    virtual void Write(const std::string& json_line) = 0;
    virtual void Close() = 0;
};

/// JSONL 文件 sink。
class JsonlFileSink final : public TraceSink
{
  public:
    explicit JsonlFileSink(std::filesystem::path file_path);

    bool IsOpen() const override;
    void Write(const std::string& json_line) override;
    void Close() override;

    const std::filesystem::path& Path() const;

  private:
    std::filesystem::path m_path;
    std::ofstream m_file;
};

} // namespace agent::plugins::traces
