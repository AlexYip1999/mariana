#include <iostream>
#include <string_view>

#include "agent/core/version.h"

namespace
{

void PrintUsage()
{
    std::cout << "agent-cli " << agent::core::Version() << "\n"
              << "\n"
              << "用法:\n"
              << "  agent-cli --version   打印版本\n"
              << "  agent-cli --help      打印帮助\n"
              << "\n"
              << "A0 骨架阶段：交互式 REPL 与模型接入将在 A1 加入。\n";
}

} // namespace

int main(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i)
    {
        const std::string_view arg = argv[i];
        if (arg == "--version" || arg == "-v")
        {
            std::cout << agent::core::Version() << "\n";
            return 0;
        }
        if (arg == "--help" || arg == "-h")
        {
            PrintUsage();
            return 0;
        }
    }

    PrintUsage();
    return 0;
}
