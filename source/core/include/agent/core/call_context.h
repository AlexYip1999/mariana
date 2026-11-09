#pragma once

#include <algorithm>
#include <chrono>
#include <stop_token>

namespace agent::core
{

/// 单次模型/工具调用可见的错误；不包含 Run 预算/策略/生命周期语义。
enum class CallError
{
    None,
    Cancelled,
    DeadlineExceeded,
    ModelFailure,
    ToolFailure,
};

inline const char* ToString(CallError value)
{
    switch (value)
    {
        case CallError::None:
        {
            return "None";
        }
        case CallError::Cancelled:
        {
            return "Cancelled";
        }
        case CallError::DeadlineExceeded:
        {
            return "DeadlineExceeded";
        }
        case CallError::ModelFailure:
        {
            return "ModelFailure";
        }
        case CallError::ToolFailure:
        {
            return "ToolFailure";
        }
    }
    return "Unknown";
}

/// 单次模型/工具调用需要的取消与 deadline；不包含 session/run/预算。
struct CallContext
{
    std::stop_token stop_token;
    std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max();

    CallError Interruption() const
    {
        if (stop_token.stop_requested())
        {
            return CallError::Cancelled;
        }
        return std::chrono::steady_clock::now() >= deadline ? CallError::DeadlineExceeded : CallError::None;
    }

    int RemainingTimeoutMs(int request_timeout_ms) const
    {
        if (deadline == std::chrono::steady_clock::time_point::max())
        {
            return request_timeout_ms;
        }
        const auto remaining =
            std::chrono::ceil<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (remaining.count() <= 0)
        {
            return 0;
        }
        return static_cast<int>(
            (std::min)(remaining.count(), static_cast<std::chrono::milliseconds::rep>(request_timeout_ms)));
    }
};

} // namespace agent::core
