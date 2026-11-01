#pragma once

#include <filesystem>
#include <functional>
#include <iosfwd>
#include <memory>
#include <string>
#include <vector>

#include "agent/transport/http_transport.h"

namespace agent::hosts::cli
{

/// 宿主环境抽象：main() 只负责 OS 适配，RunHost 负责可测试的组合根逻辑。
struct HostEnvironment
{
    std::string executable_path;
    std::function<std::string(const std::string&)> get_env;
    std::function<std::filesystem::path()> current_path;
    std::function<std::unique_ptr<agent::transport::HttpTransport>()> create_transport;
};

/// 解析参数、加载配置、装配 Executor/Session、分发单轮/交互。
int RunHost(const std::vector<std::string>& args, const HostEnvironment& environment, std::istream& in,
            std::ostream& out, std::ostream& err);

} // namespace agent::hosts::cli
