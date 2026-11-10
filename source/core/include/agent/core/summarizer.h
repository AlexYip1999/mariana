#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "agent/core/call_context.h"
#include "agent/core/memory.h"
#include "agent/core/message.h"

namespace agent::core
{

/// 会话压缩写入的 System 消息前缀；Runtime 用它识别并替换旧摘要，Host 可据此过滤。
inline constexpr const char* kSummaryMessagePrefix = "[conversation-summary]\n";

/// 中期压缩请求：把一段旧轮次压成可继续使用的摘要。
struct SummaryRequest
{
    // 输入：待压缩的旧轮次与所属记忆作用域
    std::vector<ChatMessage> turns;
    MemoryScope scope;

    // 预算与调用：>0 = 摘要输出字符上限，0 = 由实现决定
    std::size_t max_chars = 0;
    CallContext call;
};

struct SummaryResult
{
    // 结果
    bool ok = true;
    std::string summary;

    // 失败原因（ok=false 时由实现填写）
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
