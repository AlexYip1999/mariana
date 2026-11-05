#include "agent/plugins/tools/workspace_path_policy.h"

#include <string>
#include <system_error>
#include <utility>

namespace agent::plugins::tools
{

namespace
{

std::string LowerAscii(std::string value)
{
    for (char& character : value)
    {
        if (character >= 'A' && character <= 'Z')
        {
            character = static_cast<char>(character + ('a' - 'A'));
        }
    }
    return value;
}

bool IsSensitiveComponent(const std::filesystem::path& component)
{
    const std::u8string utf8_name = component.u8string();
    const std::string name = LowerAscii(std::string(utf8_name.begin(), utf8_name.end()));
    if (name == ".env" || name.rfind(".env.", 0) == 0 || name == "config.local.json" || name == ".git" ||
        name == ".docker" || name == ".kube" || name == "id_rsa" || name == "id_ed25519" || name == "id_ecdsa" ||
        name == ".git-credentials" || name == ".netrc" || name == ".npmrc" || name == ".pypirc" || name == ".htpasswd")
    {
        return true;
    }
    return name.ends_with(".key") || name.ends_with(".pem") || name.ends_with(".pfx") || name.ends_with(".p12") ||
           name.ends_with(".jks") || name.ends_with(".ppk") || name.ends_with(".kdbx");
}

} // namespace

WorkspacePathPolicy::WorkspacePathPolicy(std::filesystem::path root)
{
    std::error_code error;
    m_root = std::filesystem::weakly_canonical(std::move(root), error);
    if (error)
    {
        m_root.clear();
    }
}

bool WorkspacePathPolicy::IsValid() const
{
    return !m_root.empty();
}

bool WorkspacePathPolicy::IsWithinWorkspace(const std::filesystem::path& candidate) const
{
    if (m_root.empty())
    {
        return false;
    }

    const std::filesystem::path relative = candidate.lexically_relative(m_root);
    if (relative.empty())
    {
        return false;
    }

    const std::filesystem::path first = *relative.begin();
    return first != "..";
}

bool WorkspacePathPolicy::IsSensitive(const std::filesystem::path& relative_path) const
{
    for (const std::filesystem::path& component : relative_path)
    {
        if (IsSensitiveComponent(component))
        {
            return true;
        }
    }
    return false;
}

const std::filesystem::path& WorkspacePathPolicy::Root() const
{
    return m_root;
}

} // namespace agent::plugins::tools
