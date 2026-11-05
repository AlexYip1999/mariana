#include "agent/runtime/execution_id.h"

#include <atomic>
#include <chrono>

namespace agent::runtime
{

std::string ExecutionId::NewSession()
{
    return New("session");
}

std::string ExecutionId::NewRun()
{
    return New("run");
}

std::string ExecutionId::New(const char* prefix)
{
    static std::atomic<unsigned long long> sequence{0};
    static const auto epoch = std::chrono::system_clock::now().time_since_epoch().count();
    return std::string(prefix) + "-" + std::to_string(epoch) + "-" + std::to_string(++sequence);
}

} // namespace agent::runtime
