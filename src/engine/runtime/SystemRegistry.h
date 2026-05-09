#pragma once
#include <algorithm>
#include <vector>
#include <unordered_map>
#include <memory>
#include <typeindex>
#include "../systems/ISystem.h"
#include "../../core/Assert.h"

namespace engine {

class SystemRegistry {
public:
    // 注册并获取所有权，返回引用供外部持有
    template<typename T, typename... Args>
    T& registerSystem(Args&&... args) {
        auto ptr = std::make_unique<T>(static_cast<Args&&>(args)...);
        T& ref = *ptr;
        lookup_[std::type_index(typeid(T))] = ptr.get();
        order_.push_back(std::move(ptr));
        return ref;
    }

    ISystem& registerSystem(std::unique_ptr<ISystem> system) {
        ASSERT_MSG(system != nullptr, "System must not be null");
        ISystem& ref = *system;
        order_.push_back(std::move(system));
        return ref;
    }

    bool unregisterSystem(ISystem* system, bool callShutdown = true) {
        if (!system) return false;

        for (auto it = lookup_.begin(); it != lookup_.end(); ) {
            if (it->second == system) it = lookup_.erase(it);
            else ++it;
        }

        auto it = std::find_if(order_.begin(), order_.end(),
                               [&](const std::unique_ptr<ISystem>& ptr) {
                                   return ptr.get() == system;
                               });
        if (it == order_.end()) return false;
        if (callShutdown) (*it)->shutdown();
        order_.erase(it);
        return true;
    }

    template<typename T>
    T& get() {
        auto it = lookup_.find(std::type_index(typeid(T)));
        ASSERT_MSG(it != lookup_.end(), "System not registered");
        return *static_cast<T*>(it->second);
    }

    template<typename T>
    bool has() const {
        return lookup_.count(std::type_index(typeid(T))) > 0;
    }

    void initAll() {
        for (auto& s : order_) s->init();
    }

    void shutdownAll() {
        // 逆序 shutdown
        for (int i = static_cast<int>(order_.size()) - 1; i >= 0; --i)
            order_[i]->shutdown();
    }

    // FrameScheduler 按显式 phase 顺序扫描所有系统，不提供 updateAll，
    // 避免系统绕过阶段语义直接“整帧全跑一遍”。
    const std::vector<std::unique_ptr<ISystem>>& systems() const { return order_; }

private:
    std::vector<std::unique_ptr<ISystem>>        order_;
    std::unordered_map<std::type_index, ISystem*> lookup_;
};

} // namespace engine
