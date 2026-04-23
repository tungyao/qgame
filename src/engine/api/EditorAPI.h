#pragma once
#ifdef BUILD_EDITOR
#include <entt/entt.hpp>
#include "../../backend/shared/ResourceHandle.h"

namespace engine {

class EngineContext;

// editor 层唯一对外接口 — Month 6 填入实现
class EditorAPI {
public:
    explicit EditorAPI(EngineContext& ctx) : ctx_(ctx) {}

    // 场景预览：渲染到离屏纹理，供 ImGui::Image 显示
    TextureHandle renderSceneToTexture(int w, int h);

    // 编辑器摄像机（不写进存档）
    void setEditorCamera(float x, float y, float zoom);

    // 临时实体（不进存档，重载场景时销毁）
    entt::entity createTransientEntity();
    void         destroyTransientEntities();

    // 调试访问
    entt::registry& world();

private:
    EngineContext& ctx_;
};

} // namespace engine
#endif // BUILD_EDITOR
