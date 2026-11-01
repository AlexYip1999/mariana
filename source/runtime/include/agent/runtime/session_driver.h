#pragma once

#include <optional>
#include <string>

#include "agent/runtime/agent_session.h"

namespace agent::runtime
{

/// 外层循环的输入源：单轮、stdin、IM 事件、UE 事件都实现它。
class InputSource
{
  public:
    virtual ~InputSource() = default;

    virtual std::optional<std::string> Next() = 0;
};

/// 外层循环的输出目标。
class OutputSink
{
  public:
    virtual ~OutputSink() = default;

    virtual void OnResult(const AgentRunResult& result) = 0;
};

/// 外层循环策略：单轮、交互、事件驱动、低代码 Flow 都实现它。
class SessionDriver
{
  public:
    virtual ~SessionDriver() = default;

    virtual int Run(AgentSession& session, InputSource& input, OutputSink& output) = 0;
};

} // namespace agent::runtime
