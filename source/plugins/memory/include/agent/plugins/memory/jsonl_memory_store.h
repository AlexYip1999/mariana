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
class JsonlMemoryStore final : public core::MemoryStore
{
  public:
    /// 打开（或创建）JSONL 记忆文件并重放已有记录；失败返回 nullptr 并写 error。
    static std::shared_ptr<JsonlMemoryStore> Open(const std::filesystem::path& path, std::string& error);

    bool Remember(const core::MemoryRecord& record, std::string& error) override;
    std::vector<core::MemoryHit> Recall(const core::MemoryQuery& query) override;
    bool Forget(const std::string& id, std::string& error) override;
    bool ClearScope(const core::MemoryScope& scope, std::string& error) override;

    // 查询（线程安全；路径在构造后不再变化）
    const std::filesystem::path& Path() const;
    std::size_t Size() const;
    std::vector<core::MemoryRecord> Records() const;

  private:
    explicit JsonlMemoryStore(std::filesystem::path path);

    // 内部状态：m_mutex 同时保护 m_file 与 m_records
    mutable std::mutex m_mutex;
    std::filesystem::path m_path;
    std::ofstream m_file;
    std::vector<core::MemoryRecord> m_records;
};

} // namespace agent::plugins::memory
