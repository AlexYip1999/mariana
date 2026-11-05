#pragma once

#include <filesystem>
#include <functional>
#include <iosfwd>
#include <memory>
#include <string>
#include <vector>

#include "agent/hosts/cli/cli_config.h"
#include "agent/transport/http_transport.h"

namespace agent::hosts::cli
{

/// 平台适配回调；宿主只注入这些函数，RunHost 保持可测试。
using CurrentPathProvider = std::function<std::filesystem::path()>;
using HttpTransportFactory = std::function<std::unique_ptr<agent::transport::HttpTransport>()>;

/// 宿主环境抽象：main() 只负责 OS 适配，RunHost 负责可测试的组合根逻辑。
struct HostEnvironment
{
    // 进程信息
    std::string executable_path;

    // 平台适配
    EnvironmentLookup get_env;
    CurrentPathProvider current_path;
    HttpTransportFactory create_transport;
};

/// 解析参数、加载配置、装配 Executor/Session、分发单轮/交互。
int RunHost(const std::vector<std::string>& args, const HostEnvironment& environment, std::istream& in,
            std::ostream& out, std::ostream& err);

} // namespace agent::hosts::cli
