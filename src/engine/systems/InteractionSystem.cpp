#include "InteractionSystem.h"
#include "../runtime/EngineContext.h"
#include "../input/InputState.h"
#include "../systems/RenderSystem.h"
#include "../components/RenderComponents.h"
#include "../components/PhysicsComponents.h"
#include <cmath>
#include <limits>
#include <algorithm>

namespace engine {

InteractionSystem::InteractionSystem(EngineContext& ctx)
    : ctx_(ctx) {
}

void InteractionSystem::init() {
}

void InteractionSystem::shutdown() {
    hovered_ = entt::null;
    pressed_ = entt::null;
    prevPointerDown_ = false;
}

static bool pointTestShape(float px, float py,
                           float cx, float cy,
                           const Collider& col,
                           float& outDistSq) {
    switch (col.shapeType) {
    case ShapeType::Box: {
        const float hw = col.width * 0.5f;
        const float hh = col.height * 0.5f;
        const float minX = cx - hw, maxX = cx + hw;
        const float minY = cy - hh, maxY = cy + hh;
        if (px >= minX && px <= maxX && py >= minY && py <= maxY) {
            // Distance to center for tiebreaking
            const float dx = px - cx, dy = py - cy;
            outDistSq = dx * dx + dy * dy;
            return true;
        }
        return false;
    }
    case ShapeType::Circle: {
        const float dx = px - cx, dy = py - cy;
        const float dSq = dx * dx + dy * dy;
        const float r = col.radius;
        if (dSq < r * r) {
            outDistSq = dSq;
            return true;
        }
        return false;
    }
    case ShapeType::Capsule: {
        // Vertical capsule: line segment + hemispherical caps
        const float halfLen = std::max(0.0f, col.height * 0.5f - col.radius);
        const float segY0 = cy - halfLen;
        const float segY1 = cy + halfLen;
        const float closestY = std::clamp(py, segY0, segY1);
        const float dx = px - cx, dy = py - closestY;
        const float dSq = dx * dx + dy * dy;
        const float r = col.radius;
        if (dSq < r * r) {
            outDistSq = dSq;
            return true;
        }
        return false;
    }
    }
    return false;
}

void InteractionSystem::update(float dt) {
    (void)dt;

    auto& input = ctx_.inputState;
    const bool  down         = input.pointerDown(0);
    const bool  justPressed  =  down && !prevPointerDown_;
    const bool  justReleased = !down &&  prevPointerDown_;
    prevPointerDown_ = down;

    const float sx = input.pointerX(0) * ctx_.windowWidth;
    const float sy = input.pointerY(0) * ctx_.windowHeight;

    auto resolved = RenderSystem::resolveActiveWorldCamera(ctx_);
    if (!resolved.valid) return;

    const float wx = (sx - resolved.viewportW * 0.5f) / resolved.zoom + resolved.x;
    const float wy = (sy - resolved.viewportH * 0.5f) / resolved.zoom + resolved.y;

    // Hit test: find closest Interactable entity.
    //   - Entities with Collider → shape-accurate point test
    //   - Entities without Collider → simple radius test
    entt::entity bestHit = entt::null;
    float bestDistSq = std::numeric_limits<float>::max();

    auto view = ctx_.world.view<Transform, Interactable>();
    for (auto entity : view) {
        auto& tf = view.get<Transform>(entity);
        auto& interact = view.get<Interactable>(entity);

        float hitDistSq = std::numeric_limits<float>::max();
        bool hit = false;

        if (auto* col = ctx_.world.try_get<Collider>(entity)) {
            const float cx = tf.x + col->offsetX;
            const float cy = tf.y + col->offsetY;
            hit = pointTestShape(wx, wy, cx, cy, *col, hitDistSq);
        } else {
            const float dx = wx - tf.x;
            const float dy = wy - tf.y;
            hitDistSq = dx * dx + dy * dy;
            if (hitDistSq < interact.radius * interact.radius)
                hit = true;
        }

        if (hit && hitDistSq < bestDistSq) {
            bestDistSq = hitDistSq;
            bestHit = entity;
        }
    }

    // Hover state change
    if (bestHit != hovered_) {
        if (hovered_ != entt::null) {
            ctx_.dispatcher.trigger(InteractEvent{hovered_, InteractType::Hover,
                                                  wx, wy, sx, sy});
        }
        hovered_ = bestHit;
        if (hovered_ != entt::null) {
            ctx_.dispatcher.trigger(InteractEvent{hovered_, InteractType::Hover,
                                                  wx, wy, sx, sy});
        }
    }

    // Click
    if (justPressed && bestHit != entt::null) {
        pressed_ = bestHit;
        ctx_.dispatcher.trigger(InteractEvent{bestHit, InteractType::Click,
                                              wx, wy, sx, sy});
    }

    // Release
    if (justReleased && pressed_ != entt::null) {
        ctx_.dispatcher.trigger(InteractEvent{pressed_, InteractType::Release,
                                              wx, wy, sx, sy});
        pressed_ = entt::null;
    }
}

} // namespace engine
