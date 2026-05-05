#pragma once

#include <entt/entt.hpp>
#include <nlohmann/json.hpp>

namespace engine {

class AssetManager;

namespace scene_json {

// Scene 与 Prefab 共用同一套组件 JSON 格式。
//
// 这个小工具只负责“已知组件”的序列化和反序列化；它不注册组件类型，也不做
// 反射。这样 S2 可以先把 Prefab 数据化跑通，后续 ComponentRegistry 成熟后
// 再把未知组件交给反射系统处理。
using Json = nlohmann::json;

// 把一个组件集合写到 JSON object 中。调用方决定这个 object 是 scene entity
// 顶层字段，还是 prefab 的 components 字段。
void writeKnownComponents(entt::registry& reg,
                          entt::entity entity,
                          AssetManager& assetMgr,
                          Json& out);

// 从 JSON object 读取已知组件，并 emplace_or_replace 到目标实体。
// emplace_or_replace 是 Prefab override 的关键：场景实例可以只覆盖局部字段，
// 但一旦写了某个组件，整个组件以该 JSON 为准。
void applyKnownComponents(entt::registry& reg,
                          entt::entity entity,
                          AssetManager& assetMgr,
                          const Json& components);

// 兼容旧场景格式：组件直接挂在 entity object 顶层。
// 新格式推荐放到 "components" 下，但 loader 会同时支持两者，方便 demo/编辑器
// 逐步迁移。
Json collectComponentObject(const Json& entityJson);

} // namespace scene_json

} // namespace engine
