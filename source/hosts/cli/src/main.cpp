#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

// clang-format off
#if defined(_WIN32)
#include <windows.h>
#include <shellapi.h>
#endif
// clang-format on

#include "agent/hosts/cli/host_runner.h"
#include "agent/hosts/cli/platform_transport.h"

namespace
{

std::vector<std::string> ReadUtf8Args(int argc, char** argv)
{
#if defined(_WIN32)
    int wide_count = 0;
    LPWSTR* wide_args = CommandLineToArgvW(GetCommandLineW(), &wide_count);
    if (wide_args != nullptr)
    {
        std::vector<std::string> args;
        args.reserve(static_cast<std::size_t>(wide_count));
        for (int i = 0; i < wide_count; ++i)
        {
            const int size = WideCharToMultiByte(CP_UTF8, 0, wide_args[i], -1, nullptr, 0, nullptr, nullptr);
            if (size <= 0)
            {
                continue;
            }

            std::string value(static_cast<std::size_t>(size), '\0');
            WideCharToMultiByte(CP_UTF8, 0, wide_args[i], -1, value.data(), size, nullptr, nullptr);
            value.resize(static_cast<std::size_t>(size - 1));
            args.push_back(std::move(value));
        }
        LocalFree(wide_args);
        return args;
    }
#endif

    // 非 Windows 平台：argv 直接按 UTF-8 处理
    std::vector<std::string> args;
    args.reserve(static_cast<std::size_t>(argc));
    for (int i = 0; i < argc; ++i)
    {
        args.emplace_back(argv[i]);
    }
    return args;
}

} // namespace

int main(int argc, char** argv)
{
#if defined(_WIN32)
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);
#endif

    try
    {
        const std::vector<std::string> all_args = ReadUtf8Args(argc, argv);
        const std::string executable_path = all_args.empty() ? std::string() : all_args.front();

        std::vector<std::string> raw_args;
        if (all_args.size() > 1)
        {
            raw_args.assign(all_args.begin() + 1, all_args.end());
        }

        agent::hosts::cli::HostEnvironment environment;
        environment.executable_path = executable_path;
        environment.get_env = [](const std::string& name)
        {
            const char* value = std::getenv(name.c_str());
            return value == nullptr ? std::string() : std::string(value);
        };
        environment.current_path = []
        {
            return std::filesystem::current_path();
        };
        environment.create_transport = []
        {
            return agent::hosts::cli::CreatePlatformTransport();
        };

        return agent::hosts::cli::RunHost(raw_args, environment, std::cin, std::cout, std::cerr);
    }
    catch (const std::exception& error)
    {
        std::cerr << "fatal: " << error.what() << "\n";
        return 2;
    }
    catch (...)
    {
        std::cerr << "fatal: unknown error\n";
        return 2;
    }
}
