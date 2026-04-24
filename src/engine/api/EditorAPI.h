#pragma once
#ifdef BUILD_EDITOR

#include <vector>

#include <entt/entt.hpp>

#include "../../backend/shared/ResourceHandle.h"

namespace engine {

class EngineContext;

class EditorAPI {
public:
    explicit EditorAPI(EngineContext& ctx)
        : ctx_(ctx) {}

    TextureHandle renderSceneToTexture(int w, int h);
    void*         getRawTexture(TextureHandle handle) const;
    void setEditorCamera(float x, float y, float zoom);

    entt::entity createTransientEntity();
    void destroyTransientEntities();

    entt::registry& world();
    const std::vector<entt::entity>& transientEntities() const;

private:
    EngineContext& ctx_;
    std::vector<entt::entity> transientEntities_;
};

} // namespace engine

#endif // BUILD_EDITOR
