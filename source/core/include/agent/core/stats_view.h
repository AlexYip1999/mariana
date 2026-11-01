#pragma once

#include <string>

namespace agent::core
{

/// 统计展示接口：采集由 Tracer 插件完成，host 只通过该接口取展示文本。
class StatsView
{
  public:
    virtual ~StatsView() = default;

    virtual std::string CurrentSessionLine() const = 0;
};

} // namespace agent::core
