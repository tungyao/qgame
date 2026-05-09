#include "UISystem.h"
#include "RenderSystem.h"
#include "PhysicsSystem.h"

#include "../runtime/EngineContext.h"
#include "../runtime/TransformInterpolation.h"
#include "../input/InputState.h"
#include "../components/RenderComponents.h"
#include "../../backend/renderer/IRenderDevice.h"
#include "../../core/Logger.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <unordered_map>

namespace engine {

namespace {

float presentationAlpha(const EngineContext& ctx) {
    if (!ctx.systems.has<PhysicsSystem>()) {
        return 1.f;
    }
    return ctx.systems.get<PhysicsSystem>().interpolationAlpha();
}

}

UISystem::UISystem(EngineContext& ctx) : ctx_(ctx) {}
UISystem::~UISystem() = default;

void UISystem::init() {
    // 注：renderDevice 在 EngineContext::init 之后才可用，此处不主动建白纹理。
    // 第一次 update() 里再 ensureWhiteTexture()。
}

void UISystem::shutdown() {
    if (whiteTexture_.valid()) {
        ctx_.renderDevice().destroyTexture(whiteTexture_);
        whiteTexture_ = TextureHandle{};
    }
}

void UISystem::ensureWhiteTexture() {
    if (whiteTexture_.valid()) return;
    backend::TextureDesc desc{};
    desc.width  = 1;
    desc.height = 1;
    desc.channels = 4;
    static const uint8_t kWhite[4] = {255, 255, 255, 255};
    desc.data = kWhite;
    whiteTexture_ = ctx_.renderDevice().createTexture(desc);
}

// ───────────────────────────────────────────────────────────────────────────────

void UISystem::update(float dt) {
    ensureWhiteTexture();

    if (ctx_.windowWidth > 0 && ctx_.windowHeight > 0) {
        screenW_ = static_cast<float>(ctx_.windowWidth);
        screenH_ = static_cast<float>(ctx_.windowHeight);
    } else if (ctx_.window) {
        screenW_ = static_cast<float>(ctx_.window->width());
        screenH_ = static_cast<float>(ctx_.window->height());
    }

    updateCanvases();
    runLayout();
    runInteraction(ctx_.inputState);
    // 更新所有聚焦文本输入框的光标闪烁计时器
    {
        auto tiv = ctx_.world.view<UITextInput>();
        for (auto [e, ti] : tiv.each()) {
            if (ti.focused) ti.caretTimer += dt;
        }
    }
    updateTooltip(dt);
    buildCommands();
}

void UISystem::updateCanvases() {
    auto view = ctx_.world.view<UICanvas>();
    for (auto [e, c] : view.each()) {
        if (c.scaleMode == UICanvas::ScaleMode::ScaleWithScreen && c.referenceWidth > 0 && c.referenceHeight > 0) {
            const float sx = screenW_ / static_cast<float>(c.referenceWidth);
            const float sy = screenH_ / static_cast<float>(c.referenceHeight);
            c.scaleFactor = std::min(sx, sy);
        } else {
            c.scaleFactor = 1.f;
        }
    }
}

// ───────────────────────────────────────────────────────────────────────────────

void UISystem::getWorldCamera(float& camX, float& camY, float& zoom) const {
    camX = 0.f;
    camY = 0.f;
    zoom = 1.f;

    // UIWorldAnchor 必须和 RenderSystem 使用同一套相机解析规则。
    //
    // 以前这里直接读 Camera::zoom，窗口一变大，UI 锚点与真实世界绘制结果就会
    // 分叉：RenderSystem 若改成 FixedVertical，而 UISystem 还按旧 zoom 投影，
    // 血条/名字牌会“漂”。
    //
    // 因此这里不再自己推导 world->screen，而是复用 RenderSystem 的公共解析入口。
    const ResolvedCameraView2D view = RenderSystem::resolveActiveWorldCamera(ctx_);
    if (!view.valid) return;

    camX = view.x;
    camY = view.y;
    zoom = view.zoom;
}

void UISystem::screenToCamera(float sx, float sy, float& cx, float& cy) const {
    // UI/Screen 相机假设位于 (0,0)、zoom=1。屏幕像素 (sx,sy)（y-down，左上角原点）
    // 在该相机的 world 空间中即 (sx - vpW/2, sy - vpH/2)。
    cx = sx - screenW_ * 0.5f;
    cy = sy - screenH_ * 0.5f;
}

// ───────────────────────────────────────────────────────────────────────────────

void UISystem::runLayout() {
    auto& world = ctx_.world;

    auto canvasView = world.view<UICanvas>();
    for (auto [canvasE, c] : canvasView.each()) {
        const float cx = c.safeAreaLeft;
        const float cy = c.safeAreaTop;
        const float cw = std::max(0.f, screenW_ - c.safeAreaLeft - c.safeAreaRight);
        const float ch = std::max(0.f, screenH_ - c.safeAreaTop - c.safeAreaBottom);

        auto nv = world.view<UINode>();
        for (auto [e, n] : nv.each()) {
            if (n.parent != canvasE) continue;
            layoutNode(e, cx, cy, cw, ch, 0.f, 0.f);
        }
    }
}

void UISystem::measureScrollContent(entt::entity sv) {
    auto& world = ctx_.world;
    auto* svc = world.try_get<UIScrollView>(sv);
    if (!svc) return;
    // 仅当用户没有显式给 contentW/H (<=0) 时才自动测量
    bool autoW = svc->contentW <= 0.f;
    bool autoH = svc->contentH <= 0.f;
    if (!autoW && !autoH) return;
    float maxW = 0.f, maxH = 0.f;
    auto nv = world.view<UINode>();
    for (auto [child, cn] : nv.each()) {
        if (cn.parent != sv) continue;
        // 子节点假设以 topLeft/pivot=0 排布（演示约定）；用 offset+size 估算
        const float r = cn.offsetX + cn.width;
        const float b = cn.offsetY + cn.height;
        if (r > maxW) maxW = r;
        if (b > maxH) maxH = b;
    }
    if (autoW) svc->contentW = maxW;
    if (autoH) svc->contentH = maxH;
}

void UISystem::layoutNode(entt::entity e, float parentX, float parentY,
                          float parentW, float parentH,
                          float scrollOffX, float scrollOffY) {
    auto& world = ctx_.world;
    auto& n = world.get<UINode>(e);

    bool placed = false;

    // 世界锚点：把目标实体的世界坐标投影到屏幕像素，作为本节点 anchor 中心。
    if (auto* wa = world.try_get<UIWorldAnchor>(e)) {
        if (wa->target != entt::null && world.all_of<Transform>(wa->target)) {
            const Transform tf =
                sampleInterpolatedTransform(world, wa->target, presentationAlpha(ctx_));
            float camX, camY, camZoom;
            getWorldCamera(camX, camY, camZoom);

            const float ax = (tf.x + wa->offsetX - camX) * camZoom + screenW_ * 0.5f;
            const float ay = (tf.y + wa->offsetY - camY) * camZoom + screenH_ * 0.5f;

            n.screenW = n.width;
            n.screenH = n.height;
            n.screenX = ax + n.offsetX - n.pivotX * n.screenW;
            n.screenY = ay + n.offsetY - n.pivotY * n.screenH;
            placed = true;
        }
    }

    if (!placed) {
        const float aMinX = parentX + n.anchor.minX * parentW;
        const float aMinY = parentY + n.anchor.minY * parentH;
        const float aMaxX = parentX + n.anchor.maxX * parentW;
        const float aMaxY = parentY + n.anchor.maxY * parentH;

        n.screenW = (n.anchor.minX != n.anchor.maxX) ? (aMaxX - aMinX) : n.width;
        n.screenH = (n.anchor.minY != n.anchor.maxY) ? (aMaxY - aMinY) : n.height;

        const float cx = (aMinX + aMaxX) * 0.5f;
        const float cy = (aMinY + aMaxY) * 0.5f;
        n.screenX = cx + n.offsetX - n.pivotX * n.screenW;
        n.screenY = cy + n.offsetY - n.pivotY * n.screenH;
    }

    // 父节点是滚动容器时，应用其 scroll 偏移（scrollOffX/Y 由调用方传入）
    n.screenX += scrollOffX;
    n.screenY += scrollOffY;

    // 本节点若是 ScrollView，先测量 content 并夹紧 scroll，再向下递归。
    float childScrollX = 0.f, childScrollY = 0.f;
    if (auto* sv = world.try_get<UIScrollView>(e)) {
        measureScrollContent(e);
        if (sv->clamp) {
            const float maxX = std::max(0.f, sv->contentW - n.screenW);
            const float maxY = std::max(0.f, sv->contentH - n.screenH);
            sv->scrollX = std::clamp(sv->scrollX, 0.f, maxX);
            sv->scrollY = std::clamp(sv->scrollY, 0.f, maxY);
        }
        childScrollX = -sv->scrollX;
        childScrollY = -sv->scrollY;
    }

    // 若挂了 UILayoutGroup，交由布局组逻辑统一摆放子节点；否则按各子节点自身的
    // anchor/offset 正常递归。
    if (world.all_of<UILayoutGroup>(e)) {
        applyLayoutGroup(e, childScrollX, childScrollY);
    } else {
        auto nv = world.view<UINode>();
        for (auto [child, cn] : nv.each()) {
            if (cn.parent == e) {
                layoutNode(child, n.screenX, n.screenY, n.screenW, n.screenH,
                           childScrollX, childScrollY);
            }
        }
    }
}

// ───────────────────────────────────────────────────────────────────────────────
// 布局组：把直接子节点排成行/列/网格
// ───────────────────────────────────────────────────────────────────────────────
void UISystem::applyLayoutGroup(entt::entity e, float scrollOffX, float scrollOffY) {
    auto& world = ctx_.world;
    const auto& parent = world.get<UINode>(e);
    const auto& g      = world.get<UILayoutGroup>(e);

    // 排布区域：父矩形减去 padding
    const float innerX = parent.screenX + g.paddingL;
    const float innerY = parent.screenY + g.paddingT;
    const float innerW = std::max(0.f, parent.screenW - g.paddingL - g.paddingR);
    const float innerH = std::max(0.f, parent.screenH - g.paddingT - g.paddingB);

    // 收集 visible 直接子节点，按 sortOrder 升序保持稳定
    struct Item { entt::entity ent; int order; };
    std::vector<Item> kids;
    {
        auto nv = world.view<UINode>();
        for (auto [c, cn] : nv.each()) {
            if (cn.parent == e && cn.visible) kids.push_back({c, cn.sortOrder});
        }
    }
    std::stable_sort(kids.begin(), kids.end(),
                     [](const Item& a, const Item& b) { return a.order < b.order; });
    if (g.reverse) std::reverse(kids.begin(), kids.end());

    // 把一个子节点放到 (cellX, cellY, cellW, cellH)：覆盖其 anchor/pivot/offset，
    // 让 layoutNode 重新跑时 screenXY 落到我们指定的位置；再走 layoutNode 复用它
    // 自带的 ScrollView 测量 / 嵌套 LayoutGroup / 滚动偏移逻辑。
    auto placeChild = [&](entt::entity child, float cellX, float cellY,
                          float cellW, float cellH) {
        auto& cn = world.get<UINode>(child);
        cn.anchor  = UIAnchor::topLeft();
        cn.pivotX  = 0.f;
        cn.pivotY  = 0.f;
        cn.offsetX = cellX - parent.screenX;   // anchor=topLeft 下 screenX = parent + offset
        cn.offsetY = cellY - parent.screenY;
        // Stretch 模式：交叉轴撑满（仅当本身没有交叉轴需求时改）
        if (g.crossAlign == UILayoutGroup::CrossAlign::Stretch) {
            if (g.type == UILayoutGroup::Type::Horizontal)      cn.height = cellH;
            else if (g.type == UILayoutGroup::Type::Vertical)   cn.width  = cellW;
            else /* Grid */                                   { cn.width  = cellW; cn.height = cellH; }
        }
        layoutNode(child, parent.screenX, parent.screenY,
                   parent.screenW, parent.screenH, scrollOffX, scrollOffY);
    };

    if (g.type == UILayoutGroup::Type::Horizontal) {
        float cursor = 0.f;
        for (auto& k : kids) {
            const auto& cn = world.get<UINode>(k.ent);
            const float w = cn.width;
            const float h = cn.height;
            float cellY = innerY;
            switch (g.crossAlign) {
                case UILayoutGroup::CrossAlign::Start:   cellY = innerY; break;
                case UILayoutGroup::CrossAlign::Center:  cellY = innerY + (innerH - h) * 0.5f; break;
                case UILayoutGroup::CrossAlign::End:     cellY = innerY + (innerH - h); break;
                case UILayoutGroup::CrossAlign::Stretch: cellY = innerY; break;
            }
            placeChild(k.ent, innerX + cursor, cellY, w, innerH);
            cursor += w + g.spacingX;
        }
    } else if (g.type == UILayoutGroup::Type::Vertical) {
        float cursor = 0.f;
        for (auto& k : kids) {
            const auto& cn = world.get<UINode>(k.ent);
            const float w = cn.width;
            const float h = cn.height;
            float cellX = innerX;
            switch (g.crossAlign) {
                case UILayoutGroup::CrossAlign::Start:   cellX = innerX; break;
                case UILayoutGroup::CrossAlign::Center:  cellX = innerX + (innerW - w) * 0.5f; break;
                case UILayoutGroup::CrossAlign::End:     cellX = innerX + (innerW - w); break;
                case UILayoutGroup::CrossAlign::Stretch: cellX = innerX; break;
            }
            placeChild(k.ent, cellX, innerY + cursor, innerW, h);
            cursor += h + g.spacingY;
        }
    } else { // Grid：以"首个子节点"的尺寸作为单元格大小
        const int cols = std::max(1, g.gridColumns);
        float cellW = 32.f, cellH = 32.f;
        if (!kids.empty()) {
            const auto& first = world.get<UINode>(kids.front().ent);
            cellW = first.width;
            cellH = first.height;
        }
        for (size_t i = 0; i < kids.size(); ++i) {
            const int col = static_cast<int>(i) % cols;
            const int row = static_cast<int>(i) / cols;
            const float cx = innerX + col * (cellW + g.spacingX);
            const float cy = innerY + row * (cellH + g.spacingY);
            placeChild(kids[i].ent, cx, cy, cellW, cellH);
        }
    }
}

// ───────────────────────────────────────────────────────────────────────────────

namespace {
inline bool pointInRect(float px, float py, const UINode& n) {
    return px >= n.screenX && px < n.screenX + n.screenW &&
           py >= n.screenY && py < n.screenY + n.screenH;
}
inline bool pointInRectXYWH(float px, float py, float x, float y, float w, float h) {
    return px >= x && px < x + w && py >= y && py < y + h;
}
} // namespace

// ── ScrollBar 几何换算 ──────────────────────────────────────────────────────
// 把 UIScrollBar 节点的屏幕矩形（width × height）+ target ScrollView 的状态
// (contentW/H, screenW/H, scrollX/Y) 折算成主轴像素坐标。所有公式集中在这里：
//   主轴长度 L = (Vertical: screenH, Horizontal: screenW) - 2 * trackPadding
//   thumbLen  = clamp( L * V / C, minThumbSize, L )
//   thumbStart = trackStart + (L - thumbLen) * S / max(C - V, eps)
// C <= V 时设 needed=false，让调用方决定是否跳过（autoHide）。
bool UISystem::computeScrollBarMetrics(entt::entity sb, ScrollBarMetrics& out) const {
    auto& world = ctx_.world;
    out = {};

    auto* bar  = world.try_get<UIScrollBar>(sb);
    auto* node = world.try_get<UINode>(sb);
    if (!bar || !node) return false;
    if (bar->target == entt::null || !world.valid(bar->target)) return false;
    auto* sv  = world.try_get<UIScrollView>(bar->target);
    auto* svn = world.try_get<UINode>(bar->target);
    if (!sv || !svn) return false;

    const bool vertical = (bar->orientation == UIScrollBar::Orientation::Vertical);

    // 主轴 / 交叉轴几何
    const float pad      = std::max(0.f, bar->trackPadding);
    const float mainNode = vertical ? node->screenH : node->screenW;
    const float crossNd  = vertical ? node->screenW : node->screenH;
    const float trackLen = std::max(0.f, mainNode - pad * 2.f);
    const float trackStart = (vertical ? node->screenY : node->screenX) + pad;
    const float crossStart = vertical ? node->screenX : node->screenY;

    // target 内容/视口/滚动量
    const float C = vertical ? sv->contentH : sv->contentW;
    const float V = vertical ? svn->screenH : svn->screenW;
    const float S = vertical ? sv->scrollY  : sv->scrollX;

    out.trackStart = trackStart;
    out.trackLen   = trackLen;
    out.crossStart = crossStart;
    out.crossLen   = crossNd;
    out.contentLen = C;
    out.viewLen    = V;
    out.scroll     = S;

    // 内容装得下视口：不需要滚动条
    if (C <= V + 1e-3f || trackLen <= 0.f) {
        out.needed   = false;
        out.thumbLen = trackLen;     // 视觉上仍当作"满滑块"，给关闭 autoHide 的场景兜底
        out.thumbStart = trackStart;
        return true;
    }

    float thumbLen = trackLen * (V / C);
    thumbLen = std::clamp(thumbLen, std::min(bar->minThumbSize, trackLen), trackLen);

    const float travel = std::max(1e-3f, C - V);
    const float t      = std::clamp(S / travel, 0.f, 1.f);
    out.thumbLen   = thumbLen;
    out.thumbStart = trackStart + (trackLen - thumbLen) * t;
    out.needed     = true;
    return true;
}

// ── Modal helpers ───────────────────────────────────────────────────────────
// 选出"最顶层"的激活模态：先按 layer 大者优先，layer 相同则按 entt 内部句柄
// (DFS 序的近似稳定 tiebreak 已经在 buildCommands 里另行处理；这里挑根节点只
// 需要一个确定性的 fallback)。无则返回 entt::null。
entt::entity UISystem::findActiveModalRoot() const {
    const auto& world = ctx_.world;
    entt::entity best = entt::null;
    int          bestLayer = std::numeric_limits<int>::min();
    auto view = world.view<const UIModal, const UINode>();
    for (auto [e, m, n] : view.each()) {
        if (!m.enabled || !n.visible) continue;
        if (best == entt::null || n.layer > bestLayer ||
            (n.layer == bestLayer && static_cast<uint32_t>(e) > static_cast<uint32_t>(best))) {
            best      = e;
            bestLayer = n.layer;
        }
    }
    return best;
}

// 沿 parent 链上溯检查 e 是否落在 modalRoot 的子树内 (含 modalRoot 自身)。
// modalRoot==null 时永远返回 true (无激活模态 = 全部可命中)。
bool UISystem::isUnderModal(entt::entity e, entt::entity modalRoot) const {
    if (modalRoot == entt::null) return true;
    auto& world = ctx_.world;
    entt::entity cur = e;
    while (cur != entt::null && world.valid(cur)) {
        if (cur == modalRoot) return true;
        auto* n = world.try_get<UINode>(cur);
        if (!n) return false;
        cur = n->parent;
    }
    return false;
}

bool UISystem::insideScissorAncestors(entt::entity e, float px, float py) const {
    auto& world = ctx_.world;
    entt::entity cur = world.get<UINode>(e).parent;
    while (cur != entt::null && world.valid(cur)) {
        if (auto* pn = world.try_get<UINode>(cur)) {
            const bool clips =
                world.all_of<UIScrollView>(cur) ||
                (world.all_of<UIMask>(cur) && world.get<UIMask>(cur).enabled);
            if (clips) {
                if (!pointInRect(px, py, *pn)) return false;
            }
            cur = pn->parent;
        } else {
            break;  // 到 canvas
        }
    }
    return true;
}

void UISystem::runInteraction(InputState& input) {
    auto& world = ctx_.world;

    const float px = input.pointerX(0) * screenW_;
    const float py = input.pointerY(0) * screenH_;
    pointerSx_ = px;
    pointerSy_ = py;
    const bool  down         = input.pointerDown(0);
    const bool  justPressed  =  down && !prevPointerDown_;
    const bool  justReleased = !down &&  prevPointerDown_;
    prevPointerDown_ = down;

    // 先清掉 hovered 标记，hit-test 命中后再回填。
    {
        auto nv = world.view<UINode>();
        for (auto [e, n] : nv.each()) n.hovered = false;
    }

    // ── 0. 计算"绘制排名"：DFS 顺序 == buildNodeCommands 的顺序（同级按
    //     sortOrder 稳定排序，父在子之前）。同位置两节点重叠时，drawRank 大者
    //     在视觉上覆盖小者，命中也归它。entt 的 view 迭代顺序与绘制顺序无关，
    //     必须用这张映射做 tiebreak，否则 scrollbar 与 button 重叠时会把点击
    //     穿透到下层。
    std::unordered_map<entt::entity, uint64_t> drawRank;
    {
        uint32_t seq = 0;
        std::function<void(entt::entity)> dfs = [&](entt::entity e) {
            auto* n = world.try_get<UINode>(e);
            if (!n || !n->visible) return;
            // 高 32 位 = layer（带偏移使负数也能比较），低 32 位 = DFS 序号。
            const uint64_t lay = static_cast<uint64_t>(
                static_cast<int64_t>(n->layer) + (1ll << 31));
            drawRank[e] = (lay << 32) | seq++;
            struct Entry { entt::entity c; int o; };
            std::vector<Entry> kids;
            auto nv = world.view<UINode>();
            for (auto [c, cn] : nv.each())
                if (cn.parent == e) kids.push_back({c, cn.sortOrder});
            std::stable_sort(kids.begin(), kids.end(),
                [](const Entry& a, const Entry& b) { return a.o < b.o; });
            for (auto& k : kids) dfs(k.c);
        };
        auto canvasView = world.view<UICanvas>();
        for (auto [canvasE, c] : canvasView.each()) {
            struct Entry { entt::entity c; int o; };
            std::vector<Entry> roots;
            auto nv = world.view<UINode>();
            for (auto [child, cn] : nv.each())
                if (cn.parent == canvasE) roots.push_back({child, cn.sortOrder});
            std::stable_sort(roots.begin(), roots.end(),
                [](const Entry& a, const Entry& b) { return a.o < b.o; });
            for (auto& r : roots) dfs(r.c);
        }
    }
    auto rankOf = [&](entt::entity e) -> uint64_t {
        auto it = drawRank.find(e);
        return it == drawRank.end() ? 0ull : it->second;
    };

    // ── 0b. 模态对话框激活检测：拿到"最顶层激活模态"，hit-test / 滚轮 /
    //       dragToScroll 都只对其子树内的节点生效，外部 UI 一律被屏蔽。
    //       同时，按下落在子树之外即视为"点了遮罩"，触发 onClickOutside。
    const entt::entity modalRoot = findActiveModalRoot();

    // ── 1. 命中测试：在所有可交互节点里挑 drawRank 最大者（最上层）。
    entt::entity bestHit  = entt::null;
    uint64_t     bestRank = 0;
    {
        auto nv = world.view<UINode>();
        for (auto [e, n] : nv.each()) {
            if (!n.visible || !n.interactable) continue;
            if (!pointInRect(px, py, n)) continue;
            // 模态激活时：不在模态子树内的节点全部排除。这一行就是
            // "禁止穿透到下方 UI" 的全部实现 —— 配合下面的 ScrollView
            // 过滤即可让模态期间所有非模态控件失活。
            if (!isUnderModal(e, modalRoot)) continue;
            // 如果该节点位于某个 ScrollView 子树内，命中点还必须落在那些视口里
            if (!insideScissorAncestors(e, px, py)) continue;
            // 只把真正的交互组件视作命中目标。否则贴在按钮上的 UILabel
            // (interactable 默认 true) 会盖住父按钮、屏幕角落的提示文字会
            // 偷走拖拽块附近的点击 —— 表现为"按钮命中错位"和"拖几次后无法
            // 再拖"。如果业务确实想让背景/标签接受 raycast，自行加上
            // UIButton 或调用 setUIInteractable(false) 由用户显式控制。
            const bool hasInteractor = world.any_of<UIButton, UIToggle, UISlider, UIDraggable, UITooltip, UITextInput, UIScrollBar>(e);
            if (!hasInteractor) continue;
            // 隐藏的 ScrollBar (autoHide && 内容装得下视口) 不参与命中，避免在
            // 上方遮挡其他控件——视觉上不画的东西不应该接收点击。
            if (auto* bar = world.try_get<UIScrollBar>(e); bar && bar->autoHide) {
                ScrollBarMetrics m;
                if (computeScrollBarMetrics(e, m) && !m.needed) continue;
            }

            const uint64_t r = rankOf(e);
            if (bestHit == entt::null || r >= bestRank) {
                bestRank = r;
                bestHit  = e;
            }
        }
    }
    hovered_ = bestHit;
    if (hovered_ != entt::null) world.get<UINode>(hovered_).hovered = true;

    // ── 1c. 模态遮罩点击：按下落在模态子树之外 (= 命中已被过滤为 null)
    //       且模态激活，触发 onClickOutside。点击同时被消费 (无 hovered_ →
    //       无 pressed_ → 不会触发任何业务控件)。
    if (justPressed && modalRoot != entt::null && bestHit == entt::null) {
        if (auto* m = world.try_get<UIModal>(modalRoot); m && m->onClickOutside) {
            m->onClickOutside();
        }
    }

    // ── 1b. 滚轮 / 拖拽滚动：找到鼠标下"最深"的 ScrollView，把 wheel delta
    //       注入它的 scrollX/Y。dragToScroll 在 hovered_ 命中其他交互体时让位。
    {
        const float wheelDx = input.wheelDeltaX();
        const float wheelDy = input.wheelDeltaY();

        entt::entity scrollTarget = entt::null;
        // 取最内层（drawRank 最大）含鼠标的 ScrollView：嵌套时由内向外覆盖。
        uint64_t bestSvRank = 0;
        auto svv = world.view<UINode, UIScrollView>();
        for (auto [e, n, sv] : svv.each()) {
            if (!n.visible) continue;
            if (!pointInRect(px, py, n)) continue;
            // 模态激活时：模态子树外的 ScrollView 不接受滚轮 / dragToScroll，
            // 否则用户能透过遮罩滚动底层列表。
            if (!isUnderModal(e, modalRoot)) continue;
            if (!insideScissorAncestors(e, px, py)) continue;
            const uint64_t r = rankOf(e);
            if (scrollTarget == entt::null || r >= bestSvRank) {
                bestSvRank = r;
                scrollTarget = e;
            }
        }
        if (scrollTarget != entt::null && (wheelDx != 0.f || wheelDy != 0.f)) {
            auto& sv = world.get<UIScrollView>(scrollTarget);
            // SDL: wheel.y 向上为正，UI 习惯：内容向下移 = scrollY 增大；
            // 故鼠标向上滚 (dy>0) → scrollY -= speed*dy (露出顶部内容)。
            const bool allowH = (sv.direction != UIScrollView::Direction::Vertical);
            const bool allowV = (sv.direction != UIScrollView::Direction::Horizontal);
            if (allowH) sv.scrollX -= wheelDx * sv.wheelSpeed;
            if (allowV) sv.scrollY -= wheelDy * sv.wheelSpeed;
            if (sv.onScroll) sv.onScroll(sv.scrollX, sv.scrollY);
        }

        // 拖拽滚动开始：按下点在 ScrollView 视口内、且 hovered_ 没有命中可交互子节点
        if (justPressed && hovered_ == entt::null && scrollTarget != entt::null) {
            auto& sv = world.get<UIScrollView>(scrollTarget);
            if (sv.dragToScroll) {
                sv.dragging = true;
                sv.lastPx = px;
                sv.lastPy = py;
                pressed_ = scrollTarget;   // 占住 pressed_，免得抬起时触发别处
                world.get<UINode>(scrollTarget).pressed = true;
            }
        }

        // 拖拽滚动跟随
        if (down) {
            auto sv2 = world.view<UINode, UIScrollView>();
            for (auto [e, n, sv] : sv2.each()) {
                if (!sv.dragging) continue;
                const float dx = px - sv.lastPx;
                const float dy = py - sv.lastPy;
                sv.lastPx = px;
                sv.lastPy = py;
                const bool allowH = (sv.direction != UIScrollView::Direction::Vertical);
                const bool allowV = (sv.direction != UIScrollView::Direction::Horizontal);
                if (allowH) sv.scrollX -= dx;
                if (allowV) sv.scrollY -= dy;
                if (sv.onScroll) sv.onScroll(sv.scrollX, sv.scrollY);
            }
        }
    }

    // ── 2. 按下：抓取 pressed_，触发 onDown，开始拖拽/滑条 ───────────────────
    if (justPressed && hovered_ != entt::null) {
        pressed_ = hovered_;
        auto& n = world.get<UINode>(pressed_);
        n.pressed = true;

        if (auto* btn = world.try_get<UIButton>(pressed_)) {
            if (!btn->isDisabled && btn->onDown) btn->onDown();
        }
        if (auto* d = world.try_get<UIDraggable>(pressed_)) {
            d->dragging = true;
            d->grabOffsetX = px - n.screenX;
            d->grabOffsetY = py - n.screenY;
            d->originX = n.offsetX;
            d->originY = n.offsetY;
            d->dropAccepted = false;
            currentDropTarget_ = entt::null;
            if (d->onDragStart) d->onDragStart();
        }
        if (auto* s = world.try_get<UISlider>(pressed_)) {
            s->dragging = true;
        }
        // ── ScrollBar 按下 ────────────────────────────────────────────────
        // 1) 命中 thumb：进入拖动模式（drag 跟随见 §5b）
        // 2) 命中 track 空白：根据点在 thumb 哪一侧，把 target 翻一页
        if (auto* bar = world.try_get<UIScrollBar>(pressed_)) {
            ScrollBarMetrics m;
            if (computeScrollBarMetrics(pressed_, m) && m.needed) {
                const bool vertical = (bar->orientation == UIScrollBar::Orientation::Vertical);
                const float pmain = vertical ? py : px;
                const bool onThumb =
                    pmain >= m.thumbStart && pmain < m.thumbStart + m.thumbLen;
                if (onThumb) {
                    bar->dragging         = true;
                    bar->dragStartPointer = pmain;
                    bar->dragStartScroll  = m.scroll;
                } else if (auto* sv = world.try_get<UIScrollView>(bar->target)) {
                    // 翻页：方向由点在 thumb 之前/之后决定，步长 = 一屏视口
                    const float dir  = (pmain < m.thumbStart) ? -1.f : 1.f;
                    const float step = m.viewLen;
                    if (vertical) sv->scrollY += dir * step;
                    else          sv->scrollX += dir * step;
                    if (sv->onScroll) sv->onScroll(sv->scrollX, sv->scrollY);
                    // 真正的 clamp 由下一帧 layoutNode 完成 (sv->clamp 默认 true)
                }
            }
        }
    }

    // ── 3. 文本输入框焦点管理 ──────────────────────────────────────────────────
    {
        // 找出当前聚焦的文本输入框
        entt::entity focusedTI = entt::null;
        {
            auto tiv = world.view<UITextInput>();
            for (auto [fe, fti] : tiv.each()) {
                if (fti.focused) { focusedTI = fe; break; }
            }
        }

        if (justPressed) {
            entt::entity hitTI = (pressed_ != entt::null && world.all_of<UITextInput>(pressed_))
                                    ? pressed_ : entt::null;

            if (hitTI != focusedTI) {
                // 旧聚焦失焦
                if (focusedTI != entt::null) {
                    auto& fti = world.get<UITextInput>(focusedTI);
                    fti.focused = false; fti.caretTimer = 0.f;
                    fti.selectionStart = std::string::npos;
                    if (fti.onBlur) fti.onBlur();
                }
                // 新聚焦
                if (hitTI != entt::null) {
                    auto& nti = world.get<UITextInput>(hitTI);
                    nti.focused = true; nti.caretTimer = 0.f;
                    nti.selectionStart = std::string::npos;
                    // 根据点击位置定位光标（基于真实字形 advance）
                    const auto& n = world.get<UINode>(hitTI);
                    float relX = px - (n.screenX + nti.paddingX) + nti.scrollOffsetX;
                    nti.cursorPos = textInputIndexAtX(nti, relX);
                    if (nti.onFocus) nti.onFocus();
                }
            } else if (hitTI != entt::null) {
                // 已聚焦，只挪光标
                auto& nti = world.get<UITextInput>(hitTI);
                nti.selectionStart = std::string::npos;
                nti.caretTimer = 0.f;
                const auto& n = world.get<UINode>(hitTI);
                float relX = px - (n.screenX + nti.paddingX) + nti.scrollOffsetX;
                nti.cursorPos = textInputIndexAtX(nti, relX);
            }
        }

        // 点击不在任何文本输入框上 → 失焦
        if (justPressed && focusedTI != entt::null) {
            bool hitAnyTI = (pressed_ != entt::null && world.all_of<UITextInput>(pressed_));
            if (!hitAnyTI) {
                auto& fti = world.get<UITextInput>(focusedTI);
                fti.focused = false; fti.caretTimer = 0.f;
                fti.selectionStart = std::string::npos;
                if (fti.onBlur) fti.onBlur();
            }
        }
    }

    // ── 4. 键盘输入处理（聚焦的文本输入框）────────────────────────────────────
    {
        entt::entity focusedTI = entt::null;
        {
            auto tiv = world.view<UITextInput>();
            for (auto [fe, fti] : tiv.each()) {
                if (fti.focused) { focusedTI = fe; break; }
            }
        }

        if (focusedTI != entt::null) {
            auto& ti = world.get<UITextInput>(focusedTI);
            const auto& n = world.get<UINode>(focusedTI);

            // 当前一帧内的所有原始键盘事件逐一处理
            for (const auto& evt : input.frameEvents()) {
                if (evt.type != platform::InputRawEvent::Type::KEY_DOWN) continue;
                const int kc = evt.keyCode;

                // 跳过单纯的修饰键
                if (kc == SDLK_LSHIFT || kc == SDLK_RSHIFT ||
                    kc == SDLK_LCTRL  || kc == SDLK_RCTRL  ||
                    kc == SDLK_LALT   || kc == SDLK_RALT) continue;

                const bool ctrlHeld  = input.isKeyDown(SDLK_LCTRL) || input.isKeyDown(SDLK_RCTRL);
                const bool shiftHeld = input.isKeyDown(SDLK_LSHIFT) || input.isKeyDown(SDLK_RSHIFT);

                // ── Ctrl 快捷键 ─────────────────────────────────────────────────
                if (ctrlHeld) {
                    if (kc == SDLK_A) { // 全选
                        ti.selectionStart = 0;
                        ti.cursorPos = ti.text.size();
                        ti.caretTimer = 0.f;
                        continue;
                    }
                    if (kc == SDLK_V) { // 粘贴
                        if (ti.readOnly) continue;
                        char* clip = SDL_GetClipboardText();
                        if (clip && *clip) {
                            size_t clen = SDL_strlen(clip);
                            if (ti.selectionStart != std::string::npos && ti.selectionStart != ti.cursorPos) {
                                size_t beg = std::min(ti.selectionStart, ti.cursorPos);
                                size_t end = std::max(ti.selectionStart, ti.cursorPos);
                                ti.text.erase(beg, end - beg);
                                ti.cursorPos = beg;
                                ti.selectionStart = std::string::npos;
                            }
                            size_t room = (ti.text.size() < ti.maxLength) ? ti.maxLength - ti.text.size() : 0;
                            if (room > 0 && clen > room) clen = room;
                            ti.text.insert(ti.cursorPos, clip, clen);
                            ti.cursorPos += clen;
                            ti.caretTimer = 0.f;
                            if (ti.onChanged) ti.onChanged(ti.text);
                        }
                        SDL_free(clip);
                        continue;
                    }
                    if (kc == SDLK_C || kc == SDLK_X) { // 复制/剪切
                        if (!ti.text.empty() && ti.selectionStart != std::string::npos && ti.selectionStart != ti.cursorPos) {
                            size_t beg = std::min(ti.selectionStart, ti.cursorPos);
                            size_t end = std::max(ti.selectionStart, ti.cursorPos);
                            std::string sel = ti.text.substr(beg, end - beg);
                            SDL_SetClipboardText(sel.c_str());
                            if (kc == SDLK_X && !ti.readOnly) { // 剪切
                                ti.text.erase(beg, end - beg);
                                ti.cursorPos = beg;
                                ti.selectionStart = std::string::npos;
                                ti.caretTimer = 0.f;
                                if (ti.onChanged) ti.onChanged(ti.text);
                            }
                        }
                        continue;
                    }
                    // 其余 Ctrl+ 按键直接忽略
                    continue;
                }

                // ── Backspace ───────────────────────────────────────────────────
                if (kc == SDLK_BACKSPACE) {
                    if (ti.readOnly) continue;
                    bool changed = false;
                    if (ti.selectionStart != std::string::npos && ti.selectionStart != ti.cursorPos) {
                        size_t beg = std::min(ti.selectionStart, ti.cursorPos);
                        size_t end = std::max(ti.selectionStart, ti.cursorPos);
                        ti.text.erase(beg, end - beg);
                        ti.cursorPos = beg;
                        ti.selectionStart = std::string::npos;
                        changed = true;
                    } else if (ti.cursorPos > 0) {
                        ti.text.erase(ti.cursorPos - 1, 1);
                        ti.cursorPos--;
                        changed = true;
                    }
                    if (changed) { ti.caretTimer = 0.f; if (ti.onChanged) ti.onChanged(ti.text); }
                    continue;
                }

                // ── Delete ──────────────────────────────────────────────────────
                if (kc == SDLK_DELETE) {
                    if (ti.readOnly) continue;
                    bool changed = false;
                    if (ti.selectionStart != std::string::npos && ti.selectionStart != ti.cursorPos) {
                        size_t beg = std::min(ti.selectionStart, ti.cursorPos);
                        size_t end = std::max(ti.selectionStart, ti.cursorPos);
                        ti.text.erase(beg, end - beg);
                        ti.cursorPos = beg;
                        ti.selectionStart = std::string::npos;
                        changed = true;
                    } else if (ti.cursorPos < ti.text.size()) {
                        ti.text.erase(ti.cursorPos, 1);
                        changed = true;
                    }
                    if (changed) { ti.caretTimer = 0.f; if (ti.onChanged) ti.onChanged(ti.text); }
                    continue;
                }

                // ── ← 方向键 ────────────────────────────────────────────────────
                if (kc == SDLK_LEFT) {
                    if (ti.cursorPos > 0) {
                        if (shiftHeld && ti.selectionStart == std::string::npos)
                            ti.selectionStart = ti.cursorPos;
                        ti.cursorPos--;
                        if (!shiftHeld) ti.selectionStart = std::string::npos;
                        ti.caretTimer = 0.f;
                    }
                    continue;
                }

                // ── → 方向键 ────────────────────────────────────────────────────
                if (kc == SDLK_RIGHT) {
                    if (ti.cursorPos < ti.text.size()) {
                        if (shiftHeld && ti.selectionStart == std::string::npos)
                            ti.selectionStart = ti.cursorPos;
                        ti.cursorPos++;
                        if (!shiftHeld) ti.selectionStart = std::string::npos;
                        ti.caretTimer = 0.f;
                    }
                    continue;
                }

                // ── Home ────────────────────────────────────────────────────────
                if (kc == SDLK_HOME) {
                    if (shiftHeld && ti.selectionStart == std::string::npos)
                        ti.selectionStart = ti.cursorPos;
                    ti.cursorPos = 0;
                    if (!shiftHeld) ti.selectionStart = std::string::npos;
                    ti.caretTimer = 0.f;
                    continue;
                }

                // ── End ─────────────────────────────────────────────────────────
                if (kc == SDLK_END) {
                    if (shiftHeld && ti.selectionStart == std::string::npos)
                        ti.selectionStart = ti.cursorPos;
                    ti.cursorPos = ti.text.size();
                    if (!shiftHeld) ti.selectionStart = std::string::npos;
                    ti.caretTimer = 0.f;
                    continue;
                }

                // ── Enter → 提交 ───────────────────────────────────────────────
                if (kc == SDLK_RETURN || kc == SDLK_KP_ENTER) {
                    if (ti.onSubmitted) ti.onSubmitted(ti.text);
                    continue;
                }

                // ── Escape → 失焦 ──────────────────────────────────────────────
                if (kc == SDLK_ESCAPE) {
                    ti.focused = false; ti.caretTimer = 0.f;
                    ti.selectionStart = std::string::npos;
                    if (ti.onBlur) ti.onBlur();
                    continue;
                }

                // ── Tab → 失焦（未来可扩展为切换到下一个输入框）────────────────
                if (kc == SDLK_TAB) {
                    ti.focused = false; ti.caretTimer = 0.f;
                    ti.selectionStart = std::string::npos;
                    if (ti.onBlur) ti.onBlur();
                    continue;
                }

                // ── 可打印 ASCII 字符 ──────────────────────────────────────────
                if (kc >= 32 && kc <= 126) {
                    if (ti.readOnly) continue;
                    if (ti.text.size() >= ti.maxLength) continue;

                    // 有选中时先删选中
                    if (ti.selectionStart != std::string::npos && ti.selectionStart != ti.cursorPos) {
                        size_t beg = std::min(ti.selectionStart, ti.cursorPos);
                        size_t end = std::max(ti.selectionStart, ti.cursorPos);
                        ti.text.erase(beg, end - beg);
                        ti.cursorPos = beg;
                        ti.selectionStart = std::string::npos;
                    }

                    if (ti.text.size() < ti.maxLength) {
                        ti.text.insert(ti.cursorPos, 1, static_cast<char>(kc));
                        ti.cursorPos++;
                        ti.caretTimer = 0.f;
                        if (ti.onChanged) ti.onChanged(ti.text);
                    }
                    continue;
                }
            }

            // ── 水平滚动夹紧：确保光标始终可见 ─────────────────────────────────
            // 使用 FontData 的真实字形 advance，而非 fontSize*0.55 估算，
            // 避免光标与 MSDF 渲染的文本末端产生累积偏差。
            {
                float cursorPixelX = 0.f, textPixelW = 0.f;
                textInputMetrics(ti, textPixelW, cursorPixelX);
                const float visibleW = std::max(1.f, n.screenW - ti.paddingX * 2.f);

                // 光标在可见区域左侧：向左滚
                if (cursorPixelX < ti.scrollOffsetX)
                    ti.scrollOffsetX = cursorPixelX;
                // 光标太靠右（距右缘不足 margin）：向右滚到留出 margin
                const float margin = 4.f;
                if (cursorPixelX - ti.scrollOffsetX + margin > visibleW)
                    ti.scrollOffsetX = cursorPixelX - visibleW + margin;

                // 文本短于可见区域时禁止滚动；否则最多滚到露出文本末尾
                const float maxScroll = std::max(0.f, textPixelW - visibleW);
                ti.scrollOffsetX = std::clamp(ti.scrollOffsetX, 0.f, maxScroll);
            }
        }
    }

    // ── 5. 滑条拖动跟随 ────────────────────────────────────────────────────
    if (down) {
        auto sv = world.view<UINode, UISlider>();
        for (auto [e, n, s] : sv.each()) {
            if (!s.dragging) continue;
            const float w = std::max(1.f, n.screenW);
            float t = (px - n.screenX) / w;
            t = std::clamp(t, 0.f, 1.f);
            if (s.step > 0.f) {
                t = std::round(t / s.step) * s.step;
                t = std::clamp(t, 0.f, 1.f);
            }
            const float v = s.min + t * (s.max - s.min);
            if (v != s.value) {
                s.value = v;
                if (s.onChanged) s.onChanged(v);
            }
        }
    }

    // ── 5b. ScrollBar 拖动跟随 ───────────────────────────────────────────────
    // 把鼠标主轴位移按 (C - V) / (L - thumbLen) 比率换算成内容滚动量。
    // 真正的 clamp 由下一帧 layoutNode 用 sv.clamp 完成 (与滚轮/dragToScroll 一致)。
    if (down) {
        auto sbv = world.view<UINode, UIScrollBar>();
        for (auto [e, n, bar] : sbv.each()) {
            if (!bar.dragging) continue;
            ScrollBarMetrics m;
            if (!computeScrollBarMetrics(e, m) || !m.needed) continue;
            auto* sv = world.try_get<UIScrollView>(bar.target);
            if (!sv) continue;

            const bool vertical = (bar.orientation == UIScrollBar::Orientation::Vertical);
            const float pmain   = vertical ? py : px;
            const float dragRange = std::max(1e-3f, m.trackLen - m.thumbLen);
            const float scrollRange = std::max(0.f, m.contentLen - m.viewLen);
            const float deltaPointer = pmain - bar.dragStartPointer;
            const float newScroll = bar.dragStartScroll
                                  + deltaPointer * (scrollRange / dragRange);

            if (vertical) {
                if (sv->scrollY != newScroll) {
                    sv->scrollY = newScroll;
                    if (sv->onScroll) sv->onScroll(sv->scrollX, sv->scrollY);
                }
            } else {
                if (sv->scrollX != newScroll) {
                    sv->scrollX = newScroll;
                    if (sv->onScroll) sv->onScroll(sv->scrollX, sv->scrollY);
                }
            }
        }
    }

    // ── 4. 拖拽元素跟随：直接改写 offsetX/Y（按工厂约定，此时 anchor=topLeft, pivot=0,0）
    //     offset 必须相对父节点原点存储，否则下一帧 layoutNode 会再叠一次
    //     parent 的 safeArea/screen 偏移，导致命中区相对视觉漂移。
    if (down) {
        auto dv = world.view<UINode, UIDraggable>();
        for (auto [e, n, d] : dv.each()) {
            if (!d.dragging) continue;
            float nx = px - d.grabOffsetX;
            float ny = py - d.grabOffsetY;
            if (d.clamp) {
                nx = std::clamp(nx, d.minX, std::max(d.minX, d.maxX - n.screenW));
                ny = std::clamp(ny, d.minY, std::max(d.minY, d.maxY - n.screenH));
            }

            float parentX = 0.f, parentY = 0.f;
            if (n.parent != entt::null) {
                if (auto* pc = world.try_get<UICanvas>(n.parent)) {
                    parentX = pc->safeAreaLeft;
                    parentY = pc->safeAreaTop;
                } else if (auto* pn = world.try_get<UINode>(n.parent)) {
                    parentX = pn->screenX;
                    parentY = pn->screenY;
                }
            }
            n.offsetX = nx - parentX;
            n.offsetY = ny - parentY;
            n.screenX = nx;
            n.screenY = ny;
            if (d.onDrag) d.onDrag(nx, ny);
        }
    }

    // ── 4b. 拖拽中的 DropTarget 命中：找到最上层接受当前 payload 的目标，
    //       维护 onDragEnter/Leave，并把 target 的 hovering 标志记到组件里
    //       供渲染层 emit 高亮。仅当 pressed_ 是 UIDraggable 且 dragging 时执行。
    // 仅在"按住且正在拖拽"时更新 hover 与 enter/leave；释放那一帧 down=false，
    // 必须保持 currentDropTarget_ 不变，留给下面 step 5 的释放分支判定 onDrop。
    {
        UIDraggable* pd = (pressed_ != entt::null) ? world.try_get<UIDraggable>(pressed_) : nullptr;
        if (down && pd && pd->dragging) {
            entt::entity newTarget = entt::null;
            uint64_t bestDtRank = 0;
            auto dtv = world.view<UINode, UIDropTarget>();
            for (auto [e, n, dt] : dtv.each()) {
                if (e == pressed_) continue;          // 不能投放到自己身上
                if (!n.visible || !n.interactable) continue;
                if (!pointInRect(px, py, n)) continue;
                if (!insideScissorAncestors(e, px, py)) continue;
                if (!dt.acceptedPayload.empty() && dt.acceptedPayload != pd->payload) continue;
                if (dt.canAccept && !dt.canAccept(pressed_)) continue;
                const uint64_t r = rankOf(e);
                if (newTarget == entt::null || r >= bestDtRank) {
                    bestDtRank = r;
                    newTarget  = e;
                }
            }
            if (newTarget != currentDropTarget_) {
                if (currentDropTarget_ != entt::null) {
                    if (auto* old = world.try_get<UIDropTarget>(currentDropTarget_)) {
                        old->hovering = false;
                        old->hoveringEntity = entt::null;
                        if (old->onDragLeave) old->onDragLeave(pressed_);
                    }
                }
                if (newTarget != entt::null) {
                    auto& nt = world.get<UIDropTarget>(newTarget);
                    nt.hovering = true;
                    nt.hoveringEntity = pressed_;
                    if (nt.onDragEnter) nt.onDragEnter(pressed_);
                }
                currentDropTarget_ = newTarget;
            }
        }
    }

    // ── 5. 抬起：触发 onUp/onClick/onChanged，结束拖拽/滑条 ─────────────────
    if (justReleased && pressed_ != entt::null) {
        auto& n = world.get<UINode>(pressed_);
        n.pressed = false;

        if (auto* btn = world.try_get<UIButton>(pressed_)) {
            if (!btn->isDisabled) {
                if (btn->onUp) btn->onUp();
                if (hovered_ == pressed_ && btn->onClick) btn->onClick();
            }
        }
        if (auto* tog = world.try_get<UIToggle>(pressed_)) {
            if (hovered_ == pressed_) {
                tog->isOn = !tog->isOn;
                if (tog->onChanged) tog->onChanged(tog->isOn);
            }
        }
        if (auto* d = world.try_get<UIDraggable>(pressed_)) {
            if (d->dragging) {
                d->dragging = false;
                // 命中 DropTarget → 触发 onDrop，记录 dropAccepted；否则按需回弹
                if (currentDropTarget_ != entt::null) {
                    auto& dt = world.get<UIDropTarget>(currentDropTarget_);
                    d->dropAccepted = true;
                    if (dt.onDrop) dt.onDrop(pressed_);
                    dt.hovering = false;
                    dt.hoveringEntity = entt::null;
                } else if (d->snapBackOnMiss) {
                    auto& dn = world.get<UINode>(pressed_);
                    dn.offsetX = d->originX;
                    dn.offsetY = d->originY;
                }
                currentDropTarget_ = entt::null;
                if (d->onDragEnd) d->onDragEnd();
            }
        }
        if (auto* s = world.try_get<UISlider>(pressed_)) {
            if (s->dragging) {
                s->dragging = false;
                if (s->onReleased) s->onReleased(s->value);
            }
        }
        if (auto* sv = world.try_get<UIScrollView>(pressed_)) {
            sv->dragging = false;
        }
        if (auto* bar = world.try_get<UIScrollBar>(pressed_)) {
            bar->dragging = false;
        }
        pressed_ = entt::null;
    }
}

// ───────────────────────────────────────────────────────────────────────────────

// ── TextInput 字形度量（基于 FontData::Glyph::advance）─────────────────────

const engine::FontData* UISystem::getTextInputFontData(const UITextInput& ti) const {
    if (!ti.font.valid()) return nullptr;
    return ctx_.renderDevice().getFont(ti.font);
}

float UISystem::textInputGlyphAdvance(const engine::FontData* fd,
                                       const UITextInput& ti, uint32_t codepoint) const {
    if (fd) {
        const Glyph* g = fd->getGlyph(codepoint);
        if (g) return g->advance * (ti.fontSize / fd->fontSize);
    }
    return ti.fontSize * 0.55f;  // fallback 估算
}

void UISystem::textInputMetrics(const UITextInput& ti,
                                 float& outTextPixelW, float& outCursorPixelX) const {
    if (ti.text.empty()) { outTextPixelW = outCursorPixelX = 0.f; return; }
    const engine::FontData* fd = getTextInputFontData(ti);
    const size_t cursorEnd = std::min(ti.cursorPos, ti.text.size());
    // 密码模式用统一密码字符的 advance 确保光标对齐视觉 '*'
    const float fixedAdv = (ti.passwordMode)
        ? textInputGlyphAdvance(fd, ti, static_cast<uint32_t>(ti.passwordChar))
        : 0.f;
    float x = 0.f;
    for (size_t i = 0; i < ti.text.size(); ++i) {
        const float adv = ti.passwordMode
            ? fixedAdv
            : textInputGlyphAdvance(fd, ti,
                  static_cast<uint32_t>(static_cast<unsigned char>(ti.text[i])));
        x += adv;
        if (i + 1 == cursorEnd) outCursorPixelX = x;
    }
    outTextPixelW = x;
    if (cursorEnd == ti.text.size()) outCursorPixelX = x;
}

size_t UISystem::textInputIndexAtX(const UITextInput& ti, float pixelX) const {
    if (ti.text.empty() || pixelX <= 0.f) return 0;
    const engine::FontData* fd = getTextInputFontData(ti);
    const float fixedAdv = (ti.passwordMode)
        ? textInputGlyphAdvance(fd, ti, static_cast<uint32_t>(ti.passwordChar))
        : 0.f;
    float x = 0.f;
    for (size_t i = 0; i < ti.text.size(); ++i) {
        const float adv = ti.passwordMode
            ? fixedAdv
            : textInputGlyphAdvance(fd, ti,
                  static_cast<uint32_t>(static_cast<unsigned char>(ti.text[i])));
        if (pixelX < x + adv * 0.5f) return i;
        x += adv;
    }
    return ti.text.size();
}

void UISystem::emitRect(float x, float y, float w, float h,
                        core::Color tint, TextureHandle tex,
                        core::Rect src, int sortKey) {
    if (w <= 0.f || h <= 0.f) return;

    backend::DrawSpriteCmd c{};
    if (tex.valid()) {
        c.texture = tex;
        if (src.w <= 0.f || src.h <= 0.f) {
            int tw = 1, th = 1;
            ctx_.renderDevice().getTextureDimensions(tex, tw, th);
            src = core::Rect{0.f, 0.f, static_cast<float>(tw), static_cast<float>(th)};
        }
    } else {
        c.texture = whiteTexture_;
        src = core::Rect{0.f, 0.f, 1.f, 1.f};
    }
      // CPU 后端 (SDL_GPU / GL) 把 (cmd.x, cmd.y) 当 quad 中心硬编码绘制，
           // 完全忽略 pivot；GPU-driven 后端则按 pivot 走矩阵。统一传中心坐标 +
          // pivot=0.5 即可让两条路径都把矩形左上角落在调用方指定的 (x, y)。
    float cx, cy;
    screenToCamera(x, y, cx, cy);
    c.x        = cx + w * 0.5f;
    c.y        = cy + h * 0.5f;
    c.pivotX   = 0.5f;
    c.pivotY   = 0.5f;
    c.rotation = 0.f;
    c.scaleX   = w / src.w;
    c.scaleY   = h / src.h;
    c.srcRect  = src;
    c.tint     = tint;
    c.layer    = 0;
    c.sortKey  = sortKey;
    c.ySort    = false;
    c.pass     = RenderPass::Screen;
    uiCommands_.emplace_back(c);
}

void UISystem::emitText(const UILabel& lbl, const UINode& n, int sortKey) {
    if (lbl.text.empty()) return;

    backend::DrawTextCmd t{};
    t.font     = lbl.font;
    t.text     = lbl.text;
    t.fontSize = lbl.fontSize;
    t.color    = lbl.color;
    t.layer    = 0;
    t.sortKey  = sortKey;
    t.ySort    = false;
    t.pass     = RenderPass::Screen;

    // 文字以 baseline 为锚（y 向下增长），近似估算字体度量：
    //   ascent ≈ fontSize * 0.8, total height ≈ fontSize。
    float ox = n.screenX, oy = n.screenY;
    switch (lbl.halign) {
        case UILabel::HAlign::Left:   ox = n.screenX; break;
        case UILabel::HAlign::Center: {
            const float approxW = static_cast<float>(lbl.text.size()) * lbl.fontSize * 0.55f;
            ox = n.screenX + (n.screenW - approxW) * 0.5f;
            break;
        }
        case UILabel::HAlign::Right: {
            const float approxW = static_cast<float>(lbl.text.size()) * lbl.fontSize * 0.55f;
            ox = n.screenX + n.screenW - approxW;
            break;
        }
    }
    const float ascent = lbl.fontSize * 0.8f;
    switch (lbl.valign) {
        case UILabel::VAlign::Top:    oy = n.screenY + ascent; break;
        case UILabel::VAlign::Middle: oy = n.screenY + (n.screenH + ascent) * 0.5f - lbl.fontSize * 0.1f; break;
        case UILabel::VAlign::Bottom: oy = n.screenY + n.screenH; break;
    }

    float cx, cy;
    screenToCamera(ox, oy, cx, cy);
    t.x = cx;
    t.y = cy;
    uiCommands_.emplace_back(std::move(t));
}

void UISystem::emitNineSlice(const UINineSlice& ns, const UINode& n, int sortKey) {
    if (!ns.texture.valid() || n.screenW <= 0.f || n.screenH <= 0.f) return;

    // 解析 srcRect：空表示整张纹理
    core::Rect src = ns.srcRect;
    if (src.w <= 0.f || src.h <= 0.f) {
        int tw = 1, th = 1;
        ctx_.renderDevice().getTextureDimensions(ns.texture, tw, th);
        src = core::Rect{0.f, 0.f, static_cast<float>(tw), static_cast<float>(th)};
    }

    // 边框宽度限制：四角横/纵和不能超过节点尺寸（否则会出现负宽）。
    // 同样不能超过 srcRect 自身的尺寸，否则中心 src 会反向。
    const float bl = std::max(0.f, std::min({ns.borderLeft,   n.screenW * 0.5f, src.w * 0.5f}));
    const float br = std::max(0.f, std::min({ns.borderRight,  n.screenW - bl,    src.w - bl}));
    const float bt = std::max(0.f, std::min({ns.borderTop,    n.screenH * 0.5f, src.h * 0.5f}));
    const float bb = std::max(0.f, std::min({ns.borderBottom, n.screenH - bt,    src.h - bt}));

    // 9 个矩形的"目标位置 / 大小"
    const float x0 = n.screenX,                 x1 = n.screenX + bl,            x2 = n.screenX + n.screenW - br;
    const float y0 = n.screenY,                 y1 = n.screenY + bt,            y2 = n.screenY + n.screenH - bb;
    const float wM = std::max(0.f, n.screenW - bl - br);   // 中部宽度
    const float hM = std::max(0.f, n.screenH - bt - bb);   // 中部高度

    // 9 个矩形的"源纹理 srcRect"
    const float sx0 = src.x,                     sx1 = src.x + bl,               sx2 = src.x + src.w - br;
    const float sy0 = src.y,                     sy1 = src.y + bt,               sy2 = src.y + src.h - bb;
    const float swM = std::max(0.f, src.w - bl - br);
    const float shM = std::max(0.f, src.h - bt - bb);

    auto put = [&](float dx, float dy, float dw, float dh,
                   float sx, float sy, float sw, float sh) {
        if (dw <= 0.f || dh <= 0.f || sw <= 0.f || sh <= 0.f) return;
        emitRect(dx, dy, dw, dh, ns.tint, ns.texture,
                 core::Rect{sx, sy, sw, sh}, sortKey);
    };

    // 4 个角（不缩放：dst=border, src=border）
    put(x0, y0, bl, bt,  sx0, sy0, bl, bt);   // TL
    put(x2, y0, br, bt,  sx2, sy0, br, bt);   // TR
    put(x0, y2, bl, bb,  sx0, sy2, bl, bb);   // BL
    put(x2, y2, br, bb,  sx2, sy2, br, bb);   // BR
    // 4 条边（边框尺寸在交叉轴保持，主轴拉伸）
    put(x1, y0, wM, bt,  sx1, sy0, swM, bt);  // T
    put(x1, y2, wM, bb,  sx1, sy2, swM, bb);  // B
    put(x0, y1, bl, hM,  sx0, sy1, bl,  shM); // L
    put(x2, y1, br, hM,  sx2, sy1, br,  shM); // R
    // 中心
    if (ns.fillCenter) put(x1, y1, wM, hM, sx1, sy1, swM, shM);
}

void UISystem::emitNodeVisuals(entt::entity e, int baseSort) {
    auto& world = ctx_.world;
    const auto& n = world.get<UINode>(e);

    if (auto* bg = world.try_get<UIBackground>(e)) {
        emitRect(n.screenX, n.screenY, n.screenW, n.screenH,
                 bg->color, bg->texture, bg->srcRect, baseSort);
    }
    if (auto* ns = world.try_get<UINineSlice>(e)) {
        emitNineSlice(*ns, n, baseSort);
    }
    if (auto* btn = world.try_get<UIButton>(e)) {
        core::Color col = btn->normal;
        if (btn->isDisabled) col = btn->disabled;
        else if (n.pressed)  col = btn->pressed;
        else if (n.hovered)  col = btn->hover;
        emitRect(n.screenX, n.screenY, n.screenW, n.screenH,
                 col, {}, {}, baseSort);
    }
    if (auto* tog = world.try_get<UIToggle>(e)) {
        const core::Color trackCol = tog->isOn ? tog->onColor : tog->offColor;
        emitRect(n.screenX, n.screenY, n.screenW, n.screenH,
                 trackCol, {}, {}, baseSort);
        const float pad  = 3.f;
        const float side = std::max(2.f, std::min(n.screenW, n.screenH) - pad * 2.f);
        const float ky   = n.screenY + (n.screenH - side) * 0.5f;
        const float kx   = tog->isOn ? (n.screenX + n.screenW - side - pad)
                                     : (n.screenX + pad);
        emitRect(kx, ky, side, side, tog->knobColor, {}, {}, baseSort + 1);
    }
    if (auto* s = world.try_get<UISlider>(e)) {
        const float trackH = std::max(3.f, n.screenH * 0.35f);
        const float trackY = n.screenY + (n.screenH - trackH) * 0.5f;
        emitRect(n.screenX, trackY, n.screenW, trackH,
                 s->trackColor, {}, {}, baseSort);
        float t = (s->max > s->min) ? (s->value - s->min) / (s->max - s->min) : 0.f;
        t = std::clamp(t, 0.f, 1.f);
        emitRect(n.screenX, trackY, n.screenW * t, trackH,
                 s->fillColor, {}, {}, baseSort + 1);
        const float hw = s->handleW;
        const float hx = n.screenX + n.screenW * t - hw * 0.5f;
        emitRect(hx, n.screenY, hw, n.screenH,
                 s->handleColor, {}, {}, baseSort + 2);
    }
    if (auto* bar = world.try_get<UIScrollBar>(e)) {
        // 视觉：track + thumb 都是纯色矩形。autoHide=true 且内容装得下时整个不画。
        // hovered/pressed 状态：节点级 hovered_ / dragging 决定 thumb 颜色。
        ScrollBarMetrics m;
        const bool ok = computeScrollBarMetrics(e, m);
        const bool draw = ok && (!bar->autoHide || m.needed);
        if (draw) {
            const bool vertical = (bar->orientation == UIScrollBar::Orientation::Vertical);
            // track
            emitRect(n.screenX, n.screenY, n.screenW, n.screenH,
                     bar->trackColor, {}, {}, baseSort);
            // thumb
            float tx, ty, tw, th;
            if (vertical) {
                tx = m.crossStart; ty = m.thumbStart;
                tw = m.crossLen;   th = m.thumbLen;
            } else {
                tx = m.thumbStart; ty = m.crossStart;
                tw = m.thumbLen;   th = m.crossLen;
            }
            core::Color tc = bar->thumbColor;
            if (bar->dragging || (pressed_ == e)) tc = bar->thumbPressed;
            else if (hovered_ == e)               tc = bar->thumbHover;
            emitRect(tx, ty, tw, th, tc, {}, {}, baseSort + 1);
        }
    }
    if (auto* pb = world.try_get<UIProgressBar>(e)) {
        emitRect(n.screenX, n.screenY, n.screenW, n.screenH,
                 pb->bgColor, {}, {}, baseSort);
        const float t = std::clamp(pb->value, 0.f, 1.f);
        float fx = n.screenX, fy = n.screenY, fw = n.screenW, fh = n.screenH;
        switch (pb->direction) {
            case UIProgressBar::Direction::LeftToRight:
                fw = n.screenW * t; break;
            case UIProgressBar::Direction::RightToLeft:
                fw = n.screenW * t; fx = n.screenX + n.screenW - fw; break;
            case UIProgressBar::Direction::TopToBottom:
                fh = n.screenH * t; break;
            case UIProgressBar::Direction::BottomToTop:
                fh = n.screenH * t; fy = n.screenY + n.screenH - fh; break;
        }
        emitRect(fx, fy, fw, fh, pb->fillColor, {}, {}, baseSort + 1);
    }
    if (auto* img = world.try_get<UIImage>(e)) {
        emitRect(n.screenX, n.screenY, n.screenW, n.screenH,
                 img->tint, img->texture, img->srcRect, baseSort);
    }
    if (auto* lbl = world.try_get<UILabel>(e)) {
        emitText(*lbl, n, baseSort + 8);
    }
    if (auto* ti = world.try_get<UITextInput>(e)) {
        // 1) 背景矩形
        emitRect(n.screenX, n.screenY, n.screenW, n.screenH,
                 ti->focused ? ti->focusedBgColor : ti->bgColor, {}, {}, baseSort);

        // 2) 四周边框（上、下、左、右四条细矩形）
        const float bw = std::max(0.f, ti->borderWidth);
        const core::Color bc = ti->focused ? ti->focusedBorderColor : ti->borderColor;
        if (bw > 0.f) {
            emitRect(n.screenX, n.screenY, n.screenW, bw, bc, {}, {}, baseSort + 1);
            emitRect(n.screenX, n.screenY + n.screenH - bw, n.screenW, bw, bc, {}, {}, baseSort + 1);
            emitRect(n.screenX, n.screenY + bw, bw, n.screenH - bw * 2.f, bc, {}, {}, baseSort + 1);
            emitRect(n.screenX + n.screenW - bw, n.screenY + bw, bw, n.screenH - bw * 2.f, bc, {}, {}, baseSort + 1);
        }

        // 3) 文本内容区域（padding 后）
        const float innerX = n.screenX + ti->paddingX;
        const float innerY = n.screenY + ti->paddingY;
        const float innerW = std::max(1.f, n.screenW - ti->paddingX * 2.f);
        const float innerH = std::max(1.f, n.screenH - ti->paddingY * 2.f);

        // Push scissor：后续文本/光标将被裁剪到 padding 区域
        {
            backend::PushScissorCmd ps{};
            ps.rect    = core::Rect{ innerX, innerY, innerW, innerH };
            ps.sortKey = baseSort + 2;
            ps.pass    = RenderPass::Screen;
            uiCommands_.emplace_back(ps);
        }

        // 用真实字形 advance 计算文本总宽和光标像素位置（密码模式统一用 '*')
        float textPixelW = 0.f, cursorPixelX = 0.f;
        textInputMetrics(*ti, textPixelW, cursorPixelX);

        // 密码模式：一律显示为密码字符
        const std::string& displayText = ti->passwordMode
            ? std::string(ti->text.size(), ti->passwordChar)
            : ti->text;

        // 文本左侧 X = innerX - scrollOffsetX（scrollOffsetX >= 0 表示内容左移）
        const float textOriginX = innerX - ti->scrollOffsetX;

        // 4) 选中背景（基于真实字形 advance 计算起止像素 X）
        if (ti->selectionStart != std::string::npos && ti->selectionStart != ti->cursorPos) {
            size_t selBeg = std::min(ti->selectionStart, ti->cursorPos);
            size_t selEnd = std::max(ti->selectionStart, ti->cursorPos);
            // 用和 textInputMetrics 相同的方式计算选中区域起止光栅 X
            const engine::FontData* sfd = getTextInputFontData(*ti);
            const float sfa = (ti->passwordMode)
                ? textInputGlyphAdvance(sfd, *ti, static_cast<uint32_t>(ti->passwordChar))
                : 0.f;
            float sx = 0.f, selLX = textOriginX, selRX = textOriginX;
            for (size_t si = 0; si < ti->text.size(); ++si) {
                const float sa = (ti->passwordMode)
                    ? sfa
                    : textInputGlyphAdvance(sfd, *ti,
                          static_cast<uint32_t>(static_cast<unsigned char>(ti->text[si])));
                if (si == selBeg) selLX = textOriginX + sx;
                sx += sa;
                if (si + 1 >= selEnd) { selRX = textOriginX + sx; break; }
            }
            // 若 selEnd == text.size() 且循环正常结束
            if (selEnd >= ti->text.size()) selRX = textOriginX + sx;
            emitRect(selLX, innerY, selRX - selLX, innerH,
                     ti->selectionColor, {}, {}, baseSort + 3);
        }

        // 5) 文本 / 占位文字
        if (displayText.empty() && !ti->focused && !ti->placeholder.empty()) {
            UILabel plcLbl;
            plcLbl.text     = ti->placeholder;
            plcLbl.font     = ti->font;
            plcLbl.fontSize = ti->fontSize;
            plcLbl.color    = ti->placeholderColor;
            plcLbl.halign   = UILabel::HAlign::Left;
            plcLbl.valign   = UILabel::VAlign::Middle;
            UINode plcNode{};
            plcNode.screenX = innerX;
            plcNode.screenY = innerY;
            plcNode.screenW = innerW;
            plcNode.screenH = innerH;
            emitText(plcLbl, plcNode, baseSort + 4);
        } else if (!displayText.empty()) {
            UILabel txtLbl;
            txtLbl.text     = displayText;
            txtLbl.font     = ti->font;
            txtLbl.fontSize = ti->fontSize;
            txtLbl.color    = ti->textColor;
            txtLbl.halign   = UILabel::HAlign::Left;
            txtLbl.valign   = UILabel::VAlign::Middle;
            UINode txtNode{};
            txtNode.screenX = textOriginX;
            txtNode.screenY = innerY;
            // screenW 保证能容纳全部文本，不会被 HAlign::Left 裁切
            txtNode.screenW = std::max(textPixelW + 4.f, innerW);
            txtNode.screenH = innerH;
            emitText(txtLbl, txtNode, baseSort + 4);
        }

        // 6) 光标竖线（聚焦时闪烁）
        if (ti->focused) {
            const float caretX = textOriginX + cursorPixelX;
            // 50% 占空比闪烁：每 1.0s 周期内前 0.5s 可见
            if (fmodf(ti->caretTimer, 1.0f) < 0.5f) {
                emitRect(caretX, innerY, std::max(1.f, ti->fontSize * 0.1f), innerH,
                         ti->caretColor, {}, {}, baseSort + 5);
            }
        }

        // Pop scissor
        {
            backend::PopScissorCmd pp{};
            pp.sortKey = baseSort + 6;
            pp.pass    = RenderPass::Screen;
            uiCommands_.emplace_back(pp);
        }
    }
    // DropTarget 高亮：当拖拽中的元素悬停于本节点之上时叠一层 tint。
    // 放最后画，盖在背景/图片之上但仍在 label 之下 (label 用 +8)。
    if (auto* dt = world.try_get<UIDropTarget>(e)) {
        if (dt->hovering && dt->highlightOnHover) {
            emitRect(n.screenX, n.screenY, n.screenW, n.screenH,
                     dt->highlightColor, {}, {}, baseSort + 7);
        }
    }
}

void UISystem::buildNodeCommands(entt::entity e, int& seq, int /*parentBaseSort*/, int layerBoost) {
    auto& world = ctx_.world;
    const auto& n = world.get<UINode>(e);
    if (!n.visible) return;

    // 进入 UIModal{enabled=true} 节点时：抬升本节点及其全部后代的 layerBoost，
    // 让模态子树整体绘制顺序高于普通 UI。kModalLayerBoost (1<<10) 远大于业务
    // 实际使用的 layer 值范围，因此能可靠覆盖。
    if (auto* m = world.try_get<UIModal>(e); m && m->enabled) {
        layerBoost = (std::max)(layerBoost, kModalLayerBoost);
    }

    // baseSort = (layer + boost) * 2^20 + sortOrder * 16 + seq；layer 占高位 →
    // 跨子树重叠时大 layer 永远在上，与命中测试 (drawRank 的 layer 高 32 位) 一致。
    const int effLayer = n.layer + layerBoost;
    const int baseSort = (effLayer << 20) + n.sortOrder * 16 + (seq++ & 0xFFFF);

    // 1) 自身视觉先画
    emitNodeVisuals(e, baseSort);

    // 2) ScrollView / UIMask：在子树外侧 push/pop 剪裁矩形（同节点同时挂两者
    //    时只 push 一次，矩形即节点自身屏幕矩形）
    const bool clipsChildren =
        world.all_of<UIScrollView>(e) ||
        (world.all_of<UIMask>(e) && world.get<UIMask>(e).enabled);
    if (clipsChildren) {
        backend::PushScissorCmd ps{};
        ps.rect    = core::Rect{ n.screenX, n.screenY, n.screenW, n.screenH };
        ps.sortKey = baseSort + 1;
        ps.pass    = RenderPass::Screen;
        uiCommands_.emplace_back(ps);
    }

    // 3) 子节点：按 sortOrder 升序，稳定排序
    struct Entry { entt::entity child; int order; };
    std::vector<Entry> kids;
    auto nv = world.view<UINode>();
    for (auto [child, cn] : nv.each()) {
        if (cn.parent == e) kids.push_back({child, cn.sortOrder});
    }
    std::stable_sort(kids.begin(), kids.end(),
                     [](const Entry& a, const Entry& b) { return a.order < b.order; });
    for (const auto& k : kids) buildNodeCommands(k.child, seq, baseSort, layerBoost);

    if (clipsChildren) {
        backend::PopScissorCmd pp{};
        pp.sortKey = baseSort + 0xFFF;  // 保证排在所有子树命令之后
        pp.pass    = RenderPass::Screen;
        uiCommands_.emplace_back(pp);
    }
}

void UISystem::buildCommands() {
    uiCommands_.clear();
    auto& world = ctx_.world;

    // 模态优先：CPU 后端按 uiCommands_ 的"插入顺序"绘制 (不按 sortKey 重新
    // 排序)，所以视觉上"模态在最上方"必须靠 emit 顺序来保证：
    //   1) 先 DFS 所有非模态子树
    //   2) emit 全屏遮罩
    //   3) 最后 DFS 模态子树
    // GPU-driven 后端走 sortKey 排序 → 由 buildNodeCommands 的 layerBoost 兜底。
    // 双轨并存即可在两条路径都拿到正确的层级。
    //
    // 约束：UIModal 节点必须是 UICanvas 的直接子节点。把模态深嵌进某个布局组
    // 子树会导致这里 phase 1 把模态的祖先一起渲染，模态视觉次序会错——
    // 业务也不该把"全屏对话框"塞进局部子树，这是合理约束。
    const entt::entity modalRoot = findActiveModalRoot();

    auto canvasView = world.view<UICanvas>();
    int seq = 0;
    for (auto [canvasE, c] : canvasView.each()) {
        struct Entry { entt::entity child; int order; };
        std::vector<Entry> roots;
        auto nv = world.view<UINode>();
        for (auto [child, cn] : nv.each()) {
            if (cn.parent == canvasE) roots.push_back({child, cn.sortOrder});
        }
        std::stable_sort(roots.begin(), roots.end(),
                         [](const Entry& a, const Entry& b) { return a.order < b.order; });
        // Phase 1：DFS 所有非模态根
        for (const auto& r : roots) {
            if (r.child == modalRoot) continue;
            buildNodeCommands(r.child, seq, 0, /*layerBoost=*/0);
        }
    }

    // Phase 2：模态遮罩 (位于普通 UI 之上、模态子树之下)
    emitModalOverlayIfAny();

    // Phase 3：模态子树最后绘制 → 视觉永远盖在遮罩与所有普通 UI 之上
    if (modalRoot != entt::null && world.valid(modalRoot)) {
        buildNodeCommands(modalRoot, seq, 0, /*layerBoost=*/0);
    }

    // tooltip：绘制在所有 UI 之上，且不进入任何 ScrollView 的 scissor 子树。
    emitTooltipIfAny();
}

// 在普通 UI (effLayer < kModalLayerBoost) 与模态子树 (effLayer >= kModalLayerBoost)
// 之间的"夹层"插入一张全屏遮罩。sortKey 用 (kModalLayerBoost << 20) - 1，刚好
// 比模态子树最低 baseSort 小 1，又比所有普通 UI 大 → 视觉上盖住底层、被对话框
// 盖住。点击吞掉的逻辑由 runInteraction 中的 isUnderModal 过滤完成，不依赖此
// rect 是否参与命中。
void UISystem::emitModalOverlayIfAny() {
    const entt::entity modalRoot = findActiveModalRoot();
    if (modalRoot == entt::null) return;
    auto* m = ctx_.world.try_get<UIModal>(modalRoot);
    if (!m || !m->drawOverlay) return;
    const int sortKey = (kModalLayerBoost << 20) - 1;
    emitRect(0.f, 0.f, screenW_, screenH_, m->overlayColor, {}, {}, sortKey);
}

// ───────────────────────────────────────────────────────────────────────────────
// Tooltip
// ───────────────────────────────────────────────────────────────────────────────
void UISystem::updateTooltip(float dt) {
    auto& world = ctx_.world;
    // 当前悬停节点是否带 UITooltip？
    const bool hasTip = (hovered_ != entt::null) && world.all_of<UITooltip>(hovered_);
    if (!hasTip) {
        tooltipHovered_ = entt::null;
        tooltipHoverT_  = 0.f;
        return;
    }
    if (hovered_ != tooltipHovered_) {
        // 切换到新的触发器：重新计时
        tooltipHovered_ = hovered_;
        tooltipHoverT_  = 0.f;
    } else {
        tooltipHoverT_ += dt;
    }
}

void UISystem::emitTooltipIfAny() {
    if (tooltipHovered_ == entt::null) return;
    auto& world = ctx_.world;
    auto* tip = world.try_get<UITooltip>(tooltipHovered_);
    if (!tip || tip->text.empty()) return;
    if (tooltipHoverT_ < tip->delay) return;

    // 估算文本框尺寸（与 emitText 的近似宽度公式保持一致）
    const float approxW = static_cast<float>(tip->text.size()) * tip->fontSize * 0.55f;
    const float boxW = approxW + tip->paddingX * 2.f;
    const float boxH = tip->fontSize * 1.2f + tip->paddingY * 2.f;

    // 默认放在鼠标右下；超出屏幕就反向，保证完整可见
    float x = pointerSx_ + tip->offsetX;
    float y = pointerSy_ + tip->offsetY;
    if (x + boxW > screenW_) x = pointerSx_ - tip->offsetX - boxW;
    if (y + boxH > screenH_) y = pointerSy_ - tip->offsetY - boxH;
    if (x < 0.f) x = 0.f;
    if (y < 0.f) y = 0.f;

    // sortKey 取一个非常大的值，保证在所有 UI 之上；不被 scissor 包围
    const int kTipSort = std::numeric_limits<int>::max() - 16;
    emitRect(x, y, boxW, boxH, tip->bgColor, {}, {}, kTipSort);

    UILabel lbl;
    lbl.text     = tip->text;
    lbl.font     = tip->font;
    lbl.fontSize = tip->fontSize;
    lbl.color    = tip->textColor;
    lbl.halign   = UILabel::HAlign::Left;
    lbl.valign   = UILabel::VAlign::Middle;
    UINode fake{};
    fake.screenX = x + tip->paddingX;
    fake.screenY = y;
    fake.screenW = boxW - tip->paddingX * 2.f;
    fake.screenH = boxH;
    emitText(lbl, fake, kTipSort + 1);
}

void UISystem::emitDrawCommands(backend::CommandBuffer& cb) const {
    for (const auto& cmd : uiCommands_) {
        if (auto* s = std::get_if<backend::DrawSpriteCmd>(&cmd))      cb.drawSprite(*s);
        else if (auto* t = std::get_if<backend::DrawTextCmd>(&cmd))   cb.drawText(*t);
        else if (auto* ps = std::get_if<backend::PushScissorCmd>(&cmd)) cb.pushScissor(*ps);
        else if (auto* pp = std::get_if<backend::PopScissorCmd>(&cmd))  cb.popScissor(*pp);
    }
}

void UISystem::appendDrawCommandPtrs(std::vector<const backend::RenderCmd*>& out) const {
    out.reserve(out.size() + uiCommands_.size());
    for (const auto& cmd : uiCommands_) out.push_back(&cmd);
}

} // namespace engine
