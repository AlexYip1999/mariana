#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "agent/core/call_context.h"
#include "agent/core/memory.h"
#include "agent/core/message.h"

namespace agent::core
{

/// 中期压缩请求：把一段旧轮次压成可继续使用的摘要。
struct SummaryRequest
{
    std::vector<ChatMessage> turns;
    MemoryScope scope;
    // >0 = 摘要输出字符上限；0 = 由实现决定
    std::size_t max_chars = 0;
    CallContext call;
};

struct SummaryResult
{
    bool ok = true;
    std::string summary;
    std::string error;
};

/// 摘要抽象：规则实现离线确定性；模型实现由 Host 注入 ChatModel（可为便宜模型）。
class Summarizer
{
  public:
    virtual ~Summarizer() = default;

    virtual SummaryResult Summarize(const SummaryRequest& request) = 0;
};

} // namespace agent::core
