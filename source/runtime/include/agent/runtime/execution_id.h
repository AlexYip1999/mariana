#pragma once

#include <string>

namespace agent::runtime
{

/// 运行 ID 生成器；Session 与 Run 使用不同前缀，避免调用方传错字符串。
class ExecutionId
{
  public:
    static std::string NewSession();
    static std::string NewRun();

  private:
    static std::string New(const char* prefix);
};

} // namespace agent::runtime
