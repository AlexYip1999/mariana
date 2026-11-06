#include "agent/hosts/reference/reference_host.h"

#include <algorithm>
#include <chrono>
#include <utility>

#include "agent/core/version.h"
#include "agent/runtime/agent_runner.h"
#include "agent/runtime/context_policy.h"
#include "agent/runtime/execution_id.h"
#include "agent/runtime/tool_output_policy.h"

namespace agent::hosts::reference
{

struct ReferenceHost::AgentState
{
    std::unique_ptr<runtime::Agent> agent;
    std::string session_id;

    // 每个 Agent 串行：busy 保证同一时刻最多一个 Send。
    std::mutex mutex;
    std::uint64_t generation = 0;
    bool busy = false;
    std::stop_source stop;
};

struct ReferenceHost::Worker
{
    std::thread thread;
    std::shared_ptr<std::atomic<bool>> finished;
};

void MainThreadQueue::Post(std::function<void()> task)
{
    if (!task)
    {
        return;
    }
    std::lock_guard lock(m_mutex);
    if (m_closed)
    {
        return;
    }
    m_tasks.push_back(std::move(task));
}

std::size_t MainThreadQueue::Drain()
{
    std::deque<std::function<void()>> tasks;
    {
        std::lock_guard lock(m_mutex);
        tasks.swap(m_tasks);
    }
    std::size_t executed = 0;
    for (std::function<void()>& task : tasks)
    {
        // 宿主主线程任务异常不能穿出循环；真实引擎里应转成日志/错误上报。
        try
        {
            task();
        }
        catch (...)
        {}
        ++executed;
    }
    return executed;
}

std::size_t MainThreadQueue::Pending() const
{
    std::lock_guard lock(m_mutex);
    return m_tasks.size();
}

void MainThreadQueue::Close()
{
    std::lock_guard lock(m_mutex);
    m_closed = true;
    m_tasks.clear();
}

MainThreadToolScheduler::MainThreadToolScheduler(std::shared_ptr<MainThreadQueue> queue,
                                                 std::chrono::milliseconds poll_interval)
    : m_queue(std::move(queue))
    , m_poll_interval(poll_interval)
{}

core::ToolResult MainThreadToolScheduler::Execute(std::shared_ptr<core::Tool> tool, const std::string& arguments,
                                                  const core::CallContext& context)
{
    struct Pending
    {
        std::mutex mutex;
        std::condition_variable condition;
        bool done = false;
        core::ToolResult result;
    };

    auto pending = std::make_shared<Pending>();

    m_queue->Post(
        [pending, tool = std::move(tool), arguments, context]() mutable
        {
            core::ToolResult result;
            const core::CallError interruption = context.Interruption();
            if (interruption != core::CallError::None)
            {
                result.ok = false;
                result.error_code = interruption;
                result.error = "tool cancelled before main thread dispatch";
            }
            else
            {
                try
                {
                    result = tool->Execute(arguments, context);
                }
                catch (...)
                {
                    result.ok = false;
                    result.error_code = core::CallError::ToolFailure;
                    result.error = "tool threw on main thread";
                }
            }
            {
                std::lock_guard lock(pending->mutex);
                pending->result = std::move(result);
                pending->done = true;
            }
            pending->condition.notify_all();
        });

    std::unique_lock lock(pending->mutex);
    while (!pending->done)
    {
        const int remaining_ms = context.RemainingTimeoutMs(m_default_timeout_ms);
        if (remaining_ms <= 0)
        {
            core::ToolResult timeout;
            timeout.ok = false;
            timeout.error_code = core::CallError::DeadlineExceeded;
            timeout.error = "tool main-thread dispatch timed out";
            return timeout;
        }
        if (context.stop_token.stop_requested())
        {
            core::ToolResult cancelled;
            cancelled.ok = false;
            cancelled.error_code = core::CallError::Cancelled;
            cancelled.error = "tool main-thread dispatch cancelled";
            return cancelled;
        }
        pending->condition.wait_for(lock, m_poll_interval,
                                    [&pending]
                                    {
                                        return pending->done;
                                    });
    }
    return pending->result;
}

ReferenceHost::ReferenceHost(std::shared_ptr<transport::HttpTransport> transport, model::OpenAiConfig model_config)
    : m_main_queue(std::make_shared<MainThreadQueue>())
    , m_scheduler(std::make_shared<MainThreadToolScheduler>(m_main_queue))
    , m_model(std::make_shared<model::OpenAiChatModel>(std::move(transport), std::move(model_config)))
    , m_tools(std::make_shared<runtime::ToolRegistry>())
{}

ReferenceHost::~ReferenceHost()
{
    Shutdown();
}

bool ReferenceHost::RegisterTool(std::shared_ptr<core::Tool> tool, runtime::ToolDescriptor descriptor,
                                 std::string& error)
{
    if (m_stopping.load())
    {
        error = "host is shutting down";
        return false;
    }
    return m_tools->Register(std::move(tool), std::move(descriptor), error);
}

bool ReferenceHost::AddAgent(std::string agent_id, runtime::AgentProfile profile,
                             std::shared_ptr<const runtime::ToolPolicy> tool_policy,
                             std::shared_ptr<core::ContextProvider> context_provider, std::string& error)
{
    if (m_stopping.load())
    {
        error = "host is shutting down";
        return false;
    }
    if (agent_id.empty())
    {
        error = "agent id is required";
        return false;
    }
    {
        std::lock_guard lock(m_agents_mutex);
        if (m_agents.find(agent_id) != m_agents.end())
        {
            error = "agent already exists: " + agent_id;
            return false;
        }
    }

    const std::shared_ptr<const runtime::ToolPolicy> policy =
        tool_policy != nullptr ? std::move(tool_policy)
                               : std::make_shared<runtime::AllowListToolPolicy>(profile.tool_names);

    runtime::RunnerDependencies dependencies;
    dependencies.model = m_model;
    dependencies.tools = m_tools;
    dependencies.tool_policy = policy;
    dependencies.context_policy = std::make_shared<runtime::KeepRecentTurnsPolicy>();
    dependencies.tool_output_policy = std::make_shared<runtime::TruncateToolOutputPolicy>();
    dependencies.tool_scheduler = m_scheduler;
    dependencies.systems = std::make_shared<core::SystemRegistry>();
    dependencies.context_provider = std::move(context_provider);

    runtime::AgentDescriptor descriptor;
    descriptor.id.value = agent_id;
    descriptor.name = agent_id;
    descriptor.version = core::Version();

    runtime::AgentDependencies agent_dependencies;
    agent_dependencies.executor = std::make_shared<runtime::AgentRunner>(std::move(dependencies));

    auto state = std::make_shared<AgentState>();
    state->agent =
        std::make_unique<runtime::Agent>(std::move(descriptor), std::move(profile), std::move(agent_dependencies));
    state->session_id = state->agent->SessionId();
    {
        std::lock_guard lock(m_agents_mutex);
        m_agents.emplace(std::move(agent_id), std::move(state));
    }
    return true;
}

RunHandle ReferenceHost::SendAsync(const std::string& agent_id, std::string input, TurnCallback on_done)
{
    if (m_stopping.load())
    {
        return {};
    }
    std::shared_ptr<AgentState> state;
    {
        std::lock_guard lock(m_agents_mutex);
        const auto found = m_agents.find(agent_id);
        if (found == m_agents.end())
        {
            runtime::AgentRunResult missing;
            missing.status = runtime::RunStatus::Failed;
            missing.error_code = runtime::RunErrorCode::InvalidConfiguration;
            missing.error = "unknown agent: " + agent_id;
            Deliver(std::move(on_done), std::move(missing));
            return {};
        }
        state = found->second;
    }

    RunHandle handle;
    handle.session_id = state->session_id;
    handle.run_id = runtime::ExecutionId::NewRun();
    std::stop_token stop_token;
    {
        std::lock_guard lock(state->mutex);
        if (state->busy)
        {
            runtime::AgentRunResult busy;
            busy.session_id = state->session_id;
            busy.status = runtime::RunStatus::Busy;
            busy.error_code = runtime::RunErrorCode::Busy;
            busy.error = "agent already has an active turn";
            Deliver(std::move(on_done), std::move(busy));
            return {};
        }
        state->busy = true;
        ++state->generation;
        state->stop.request_stop();
        state->stop = std::stop_source{};
        stop_token = state->stop.get_token();
        handle.generation = state->generation;
    }

    runtime::RunOptions options;
    options.stop_token = stop_token;
    options.run_id = handle.run_id;

    ReapWorkers();
    auto worker = std::make_shared<Worker>();
    worker->finished = std::make_shared<std::atomic<bool>>(false);
    {
        std::lock_guard lock(m_workers_mutex);
        m_workers.push_back(worker);
    }
    worker->thread = std::thread(
        [this, state, generation = handle.generation, input = std::move(input), options = std::move(options),
         on_done = std::move(on_done), finished = worker->finished]() mutable
        {
            runtime::AgentRunResult result = state->agent->Send(input, options);
            bool current = false;
            {
                std::lock_guard lock(state->mutex);
                if (state->generation == generation)
                {
                    state->busy = false;
                    current = true;
                }
            }
            if (current)
            {
                Deliver(std::move(on_done), std::move(result));
            }
            finished->store(true);
        });
    return handle;
}

bool ReferenceHost::Cancel(const RunHandle& handle)
{
    std::shared_ptr<AgentState> state;
    {
        std::lock_guard lock(m_agents_mutex);
        for (const auto& entry : m_agents)
        {
            if (entry.second->session_id == handle.session_id)
            {
                state = entry.second;
                break;
            }
        }
    }
    if (!state)
    {
        return false;
    }
    std::lock_guard lock(state->mutex);
    if (!state->busy || state->generation != handle.generation)
    {
        return false;
    }
    state->stop.request_stop();
    return true;
}

std::size_t ReferenceHost::DrainMainThreadTasks()
{
    return m_main_queue->Drain();
}

void ReferenceHost::Shutdown()
{
    m_stopping.store(true);
    RequestStopAll();
    JoinWorkers();
    m_main_queue->Close();
}

bool ReferenceHost::HasAgent(const std::string& agent_id) const
{
    std::lock_guard lock(m_agents_mutex);
    return m_agents.find(agent_id) != m_agents.end();
}

std::size_t ReferenceHost::AgentCount() const
{
    std::lock_guard lock(m_agents_mutex);
    return m_agents.size();
}

void ReferenceHost::Deliver(TurnCallback on_done, runtime::AgentRunResult result)
{
    if (!on_done)
    {
        return;
    }
    m_main_queue->Post(
        [callback = std::move(on_done), result = std::move(result)]() mutable
        {
            callback(std::move(result));
        });
}

void ReferenceHost::ReapWorkers()
{
    std::vector<std::shared_ptr<Worker>> finished;
    {
        std::lock_guard lock(m_workers_mutex);
        auto first_finished = std::remove_if(m_workers.begin(), m_workers.end(),
                                             [&finished](const std::shared_ptr<Worker>& worker)
                                             {
                                                 if (worker->finished->load())
                                                 {
                                                     finished.push_back(worker);
                                                     return true;
                                                 }
                                                 return false;
                                             });
        m_workers.erase(first_finished, m_workers.end());
    }
    for (const std::shared_ptr<Worker>& worker : finished)
    {
        if (worker->thread.joinable())
        {
            worker->thread.join();
        }
    }
}

void ReferenceHost::RequestStopAll()
{
    std::lock_guard agents_lock(m_agents_mutex);
    for (const auto& entry : m_agents)
    {
        std::lock_guard state_lock(entry.second->mutex);
        if (entry.second->busy)
        {
            entry.second->stop.request_stop();
        }
    }
}

void ReferenceHost::JoinWorkers()
{
    std::vector<std::shared_ptr<Worker>> workers;
    {
        std::lock_guard lock(m_workers_mutex);
        workers.swap(m_workers);
    }
    for (const std::shared_ptr<Worker>& worker : workers)
    {
        if (worker->thread.joinable())
        {
            worker->thread.join();
        }
    }
}

} // namespace agent::hosts::reference
