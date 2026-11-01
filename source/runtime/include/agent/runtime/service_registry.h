#pragma once

#include <functional>
#include <memory>
#include <typeindex>
#include <unordered_map>
#include <utility>

namespace agent::runtime
{

/// 按接口类型存放的服务/插件实例注册表。
/// 用于可选服务与动态插件；必需依赖仍建议显式注入。
class ServiceRegistry
{
  public:
    template <typename T> void Set(std::shared_ptr<T> service)
    {
        m_services[std::type_index(typeid(T))] = std::move(service);
    }

    template <typename T> std::shared_ptr<T> Get() const
    {
        const auto it = m_services.find(std::type_index(typeid(T)));
        if (it == m_services.end())
        {
            return nullptr;
        }
        return std::static_pointer_cast<T>(it->second);
    }

    /// 存在则返回，不存在则调用 factory 创建、注册并返回。
    template <typename T> std::shared_ptr<T> CreateOrGet(const std::function<std::shared_ptr<T>()>& factory)
    {
        std::shared_ptr<T> existing = Get<T>();
        if (existing != nullptr)
        {
            return existing;
        }

        std::shared_ptr<T> created = factory();
        if (created != nullptr)
        {
            Set(created);
        }
        return created;
    }

  private:
    std::unordered_map<std::type_index, std::shared_ptr<void>> m_services;
};

} // namespace agent::runtime
