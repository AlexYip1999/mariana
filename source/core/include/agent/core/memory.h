#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "agent/core/call_context.h"

namespace agent::core
{

/// 长期记忆的类型；Host 可按业务扩展语义，core 不解释字符串内容。
enum class MemoryKind
{
    Fact,
    Event,
    Episode,
    Preference,
    Relationship,
    Goal,
};

inline const char* ToString(MemoryKind kind)
{
    switch (kind)
    {
        case MemoryKind::Fact:
        {
            return "Fact";
        }
        case MemoryKind::Event:
        {
            return "Event";
        }
        case MemoryKind::Episode:
        {
            return "Episode";
        }
        case MemoryKind::Preference:
        {
            return "Preference";
        }
        case MemoryKind::Relationship:
        {
            return "Relationship";
        }
        case MemoryKind::Goal:
        {
            return "Goal";
        }
    }
    return "Unknown";
}

/// 记忆作用域：会话 / 角色 / 用户 / 世界（Host 自行解释 id 的含义）。
struct MemoryScope
{
    std::string kind = "agent";
    std::string id;
};

inline bool operator==(const MemoryScope& left, const MemoryScope& right)
{
    return left.kind == right.kind && left.id == right.id;
}

inline bool operator!=(const MemoryScope& left, const MemoryScope& right)
{
    return !(left == right);
}

/// 一条长期记忆。text 是给模型看的自然语言；payload_json 是给 Host 做精确查询/更新的可选结构化数据，
/// core 不解析。
struct MemoryRecord
{
    // 标识与分类
    std::string id;
    MemoryKind kind = MemoryKind::Fact;

    // 内容
    std::string text;
    std::string payload_json;
    std::vector<std::string> tags;

    // 排序与可信度
    double importance = 0.5;
    double confidence = 1.0;

    // 作用域与来源
    MemoryScope scope;
    std::string source_session;
    std::string source_run;

    // 时间（由 Host/Clock 注入，便于回放）；0 = 未知/长期有效/不过期
    std::int64_t created_at = 0;
    std::int64_t updated_at = 0;
    std::int64_t valid_to = 0;
    std::int64_t expires_at = 0;
};

/// 召回请求。
struct MemoryQuery
{
    // 查询文本（通常是当前 user 输入）；空 = 只按 tags/kinds/scopes 过滤
    std::string text;

    // 过滤条件；空 = 不限制
    std::vector<MemoryKind> kinds;
    std::vector<std::string> tags;
    std::vector<MemoryScope> scopes;

    // 预算
    std::size_t limit = 8;
    std::size_t max_chars = 0; // 0 = 不限（注入预算由 Host/Runner 控制）

    // 取消 / deadline
    CallContext call;
};

struct MemoryHit
{
    MemoryRecord record;
    double score = 0.0;
};

/// 长期记忆存储抽象：持久化、索引、向量检索等全部由具体实现决定。
class MemoryStore
{
  public:
    virtual ~MemoryStore() = default;

    /// 写入/更新一条记忆；id 重复时按实现策略覆盖。失败时返回 false 并写 error。
    virtual bool Remember(const MemoryRecord& record, std::string& error) = 0;

    /// 按查询召回记忆；返回结果由实现排序（建议相关度 + importance + 新近度）。
    virtual std::vector<MemoryHit> Recall(const MemoryQuery& query) = 0;

    /// 按 id 删除；找不到返回 false 并写 error。
    virtual bool Forget(const std::string& id, std::string& error) = 0;

    /// 清空一个作用域下的全部记忆。
    virtual bool ClearScope(const MemoryScope& scope, std::string& error) = 0;
};

} // namespace agent::core
