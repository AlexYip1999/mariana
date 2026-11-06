#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "agent/core/context.h"
#include "agent/core/tool.h"
#include "agent/model/openai_chat_model.h"
#include "agent/runtime/agent.h"
#include "agent/runtime/agent_profile.h"
#include "agent/runtime/tool_descriptor.h"
#include "agent/runtime/tool_dispatcher.h"
#include "agent/runtime/tool_policy.h"
#include "agent/runtime/tool_registry.h"
#include "agent/transport/http_transport.h"

namespace agent::hosts::reference
{

using TurnCallback = std::function<void(runtime::AgentRunResult)>;

/// Host 侧 run 句柄：session_id / run_id / generation。
/// generation 每次 SendAsync +1；迟到回调按 generation 丢弃。
struct RunHandle
{
    std::string session_id;
    std::string run_id;
    std::uint64_t generation = 0;

    bool Empty() const
    {
        return session_id.empty() && run_id.empty() && generation == 0;
    }
};

/// 模拟 UE GameThread 的首选线程任务队列：任意线程 Post，主线程 Drain。
class MainThreadQueue
{
  public:
    void Post(std::function<void()> task);
    std::size_t Drain();
    std::size_t Pending() const;
    void Close();

  private:
    mutable std::mutex m_mutex;
    std::deque<std::function<void()>> m_tasks;
    bool m_closed = false;
};

/// 把工具执行编组到主线程的 ToolScheduler 参考实现。
/// worker 线程 Post 任务后等待；主线程必须持续 Drain（等价 UE GameThread tick）。
class MainThreadToolScheduler final : public runtime::ToolScheduler
{
  public:
    explicit MainThreadToolScheduler(std::shared_ptr<MainThreadQueue> queue,
                                     std::chrono::milliseconds poll_interval = std::chrono::milliseconds(2));

    core::ToolResult Execute(std::shared_ptr<core::Tool> tool, const std::string& arguments,
                             const core::CallContext& context) override;

  private:
    std::shared_ptr<MainThreadQueue> m_queue;
    std::chrono::milliseconds m_poll_interval;
    int m_default_timeout_ms = 30000;
};

/// 纯 C++ 参考 Host：演示 UE 宿主需要的 SendAsync / 取消 / generation / 主线程工具编组 / 销毁顺序。
/// 不依赖任何平台；UE 侧按同样结构换 FHttpModule 与 GameThread 即可。
class ReferenceHost
{
  public:
    ReferenceHost(std::shared_ptr<transport::HttpTransport> transport, model::OpenAiConfig model_config);
    ~ReferenceHost();

    ReferenceHost(const ReferenceHost&) = delete;
    ReferenceHost& operator=(const ReferenceHost&) = delete;

    /// 在 AddAgent 之前注册；AgentRunner 会快照注册表。
    bool RegisterTool(std::shared_ptr<core::Tool> tool, runtime::ToolDescriptor descriptor, std::string& error);

    /// tool_policy 为空时使用 AllowListToolPolicy(profile.tool_names)（不允许副作用）。
    bool AddAgent(std::string agent_id, runtime::AgentProfile profile,
                  std::shared_ptr<const runtime::ToolPolicy> tool_policy,
                  std::shared_ptr<core::ContextProvider> context_provider, std::string& error);

    /// 忙则立即投递 Busy 结果并返回空句柄；on_done 一律投递到主线程队列。
    RunHandle SendAsync(const std::string& agent_id, std::string input, TurnCallback on_done);
    bool Cancel(const RunHandle& handle);

    /// 主线程调用；执行队列中的工具任务与回调。
    std::size_t DrainMainThreadTasks();
    void Shutdown();

    bool HasAgent(const std::string& agent_id) const;
    std::size_t AgentCount() const;

  private:
    struct AgentState;
    struct Worker;

    void Deliver(TurnCallback on_done, runtime::AgentRunResult result);
    void ReapWorkers();
    void RequestStopAll();
    void JoinWorkers();

    std::shared_ptr<MainThreadQueue> m_main_queue;
    std::shared_ptr<MainThreadToolScheduler> m_scheduler;
    std::shared_ptr<core::ChatModel> m_model;
    std::shared_ptr<runtime::ToolRegistry> m_tools;

    mutable std::mutex m_agents_mutex;
    std::unordered_map<std::string, std::shared_ptr<AgentState>> m_agents;

    mutable std::mutex m_workers_mutex;
    std::vector<std::shared_ptr<Worker>> m_workers;
    std::atomic<bool> m_stopping{false};
};

} // namespace agent::hosts::reference
