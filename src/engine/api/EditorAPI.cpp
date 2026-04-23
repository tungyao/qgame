#include "EditorAPI.h"

#ifdef BUILD_EDITOR

#include "../components/RenderComponents.h"
#include "../runtime/EngineContext.h"
#include "../systems/RenderSystem.h"
#include "../../backend/renderer/CommandBuffer.h"
#include "../../backend/renderer/IRenderDevice.h"

namespace engine {

TextureHandle EditorAPI::renderSceneToTexture(int w, int h) {
    if (w <= 0 || h <= 0) {
        return {};
    }

    ctx_.systems.get<RenderSystem>().update(ctx_.deltaTime);
    return ctx_.renderDevice().renderToTexture(ctx_.renderCommandBuffer(), w, h);
}

void EditorAPI::setEditorCamera(float x, float y, float zoom) {
    auto view = ctx_.world.view<Transform, Camera>();
    for (auto entity : view) {
        auto& transform = view.get<Transform>(entity);
        auto& camera = view.get<Camera>(entity);
        if (!camera.primary) {
            continue;
        }

        transform.x = x;
        transform.y = y;
        camera.zoom = zoom;
        return;
    }

    const entt::entity entity = ctx_.world.create();
    ctx_.world.emplace<Transform>(entity, Transform{ x, y, 0.0f, 1.0f, 1.0f });
    ctx_.world.emplace<Camera>(entity, Camera{ zoom, true });
}

entt::entity EditorAPI::createTransientEntity() {
    const entt::entity entity = ctx_.world.create();
    transientEntities_.push_back(entity);
    return entity;
}

void EditorAPI::destroyTransientEntities() {
    for (const entt::entity entity : transientEntities_) {
        if (ctx_.world.valid(entity)) {
            ctx_.world.destroy(entity);
        }
    }
    transientEntities_.clear();
}

entt::registry& EditorAPI::world() {
    return ctx_.world;
}

const std::vector<entt::entity>& EditorAPI::transientEntities() const {
    return transientEntities_;
}

} // namespace engine

#endif // BUILD_EDITOR
