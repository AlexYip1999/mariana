#pragma once

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>

#include "agent/core/tracer.h"
#include "agent/plugins/traces/trace_sink.h"

namespace agent::plugins::traces
{

/// 把 AgentSystem 事件格式化成 JSONL，再交给 TraceSink。
class FileTracer : public core::Tracer
{
  public:
    explicit FileTracer(std::filesystem::path trace_dir, std::string session_id = {});
    explicit FileTracer(std::shared_ptr<TraceSink> sink);
    ~FileTracer() override;

    bool IsOpen() const;
    const std::filesystem::path& Path() const;
    const std::string& SessionId() const;
    void Close();

    void OnTurnStart() override;
    void OnTurnStarted(const core::TurnEvent& event) override;
    void OnModelRequest(const core::ChatRequest& request) override;
    void OnModelResponse(const core::ChatResponse& response) override;
    void OnToolCall(const core::ToolCall& call) override;
    void OnToolResult(const core::ToolCall& call, const core::ToolResult& result) override;
    void OnToolReplayed(const core::ToolCall& call, const core::ToolResult& result) override;
    void OnTurnEnd(bool ok, const std::string& error, int steps) override;
    void OnTurnEnded(const core::TurnEvent& event) override;

  private:
    void StartTurn(const core::TurnEvent* event);
    void EndTurn(const core::TurnEvent* event, bool ok, const std::string& error, int steps);
    void WriteEvent(const std::string& json_line);
    std::string Timestamp() const;
    std::string Sanitize(std::string text) const;

    // 输出目标与会话标识
    std::filesystem::path m_path;
    std::string m_session_id;
    std::string m_run_id;
    std::shared_ptr<TraceSink> m_sink;

    // 当前轮/步状态与计时
    int m_turn = 0;
    int m_step = 0;
    std::string m_model;
    std::chrono::steady_clock::time_point m_turn_started;
    std::chrono::steady_clock::time_point m_request_started;
    std::chrono::steady_clock::time_point m_tool_started;
};

} // namespace agent::plugins::traces
