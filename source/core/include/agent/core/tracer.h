#pragma once

#include "agent/core/agent_system.h"

namespace agent::core
{

/// 可观测性 system：只读观察 Agent loop 阶段，不修改执行流。
class Tracer : public AgentSystem
{
  public:
    virtual ~Tracer() = default;
};

/// 默认空实现：不需要观测时直接注入，避免各处判空。
class NullTracer : public Tracer
{
  public:
    virtual ~NullTracer() = default;
};

} // namespace agent::core
