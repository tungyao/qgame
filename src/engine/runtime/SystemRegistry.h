#pragma once
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

    // FrameScheduler 分步调用，不提供 updateAll — 强制显式排序
    const std::vector<std::unique_ptr<ISystem>>& systems() const { return order_; }

private:
    std::vector<std::unique_ptr<ISystem>>        order_;
    std::unordered_map<std::type_index, ISystem*> lookup_;
};

} // namespace engine
