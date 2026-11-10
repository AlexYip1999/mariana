#pragma once

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "agent/core/memory.h"

namespace agent::plugins::memory
{

/// 追加式 JSONL 持久化实现：每行一条操作（remember / forget / clear_scope），Open 时逐行重放重建状态。
/// 召回语义与 InMemoryMemoryStore 完全一致；进程内状态与追加写由内部 mutex 串行化。
/// 日志只追加不修改：写操作累计到阈值后自动 Compact 重写，Open 会容忍并修复半写入的撕裂尾行。
class JsonlMemoryStore final : public core::MemoryStore
{
  public:
    /// 打开（或创建）JSONL 记忆文件并重放已有记录；失败返回 nullptr 并写 error。
    /// 最后一行缺换行时：完整 JSON 正常重放并补换行；解析失败视为撕裂行忽略，随后用 Compact 重写文件。
    static std::shared_ptr<JsonlMemoryStore> Open(const std::filesystem::path& path, std::string& error);

    bool Remember(const core::MemoryRecord& record, std::string& error) override;
    std::vector<core::MemoryHit> Recall(const core::MemoryQuery& query) override;
    bool Forget(const std::string& id, std::string& error) override;
    bool ClearScope(const core::MemoryScope& scope, std::string& error) override;

    /// 手动压缩：把当前记录重写为 remember 行（临时文件 + 替换原文件），锁内执行。
    /// 失败返回 false 并写 error，内存状态不变。
    bool Compact(std::string& error);

    // 查询（线程安全；路径在构造后不再变化）
    const std::filesystem::path& Path() const;
    std::size_t Size() const;
    std::vector<core::MemoryRecord> Records() const;

  private:
    explicit JsonlMemoryStore(std::filesystem::path path);

    // 内部写路径：调用方必须持有 m_mutex
    bool ReopenAppendFile(std::string& error);
    void TryReopenAppendFile(std::string& error);
    bool WriteSnapshot(const std::filesystem::path& temp_path, std::string& error);
    bool CompactLocked(std::string& error);
    void RecordAppendedOperation();
    bool ShouldAutoCompact() const;

    // 内部状态：m_mutex 同时保护 m_file、m_records 与 m_appended_ops
    mutable std::mutex m_mutex;
    std::filesystem::path m_path;
    std::ofstream m_file;
    std::vector<core::MemoryRecord> m_records;

    // 自上次压缩以来成功追加的操作数，用于触发自动压缩
    std::size_t m_appended_ops = 0;
};

} // namespace agent::plugins::memory
