#pragma once

#include <string>

#include "agent/plugins/metrics/metrics_snapshot.h"

namespace agent::plugins::metrics
{

std::string FormatMetricsLine(const MetricsSnapshot& snapshot);

} // namespace agent::plugins::metrics
