#include <SDL3/SDL_main.h>
#include <engine/runtime/EngineContext.h>
#include <engine/runtime/EngineConfig.h>
#include <engine/api/GameAPI.h>
#include <engine/framework/GameContext.h>
#include <engine/framework/GameInstance.h>
#include <engine/framework/GameManifest.h>
#include <engine/framework/PrefabRegistry.h>
#include <engine/framework/SceneManager.h>
#include <engine/scene/SceneSerializer.h>
#include <engine/components/RenderComponents.h>
#include <engine/components/PhysicsComponents.h>
#include <engine/components/AnimatorComponent.h>
#include <engine/components/ParticleComponent.h>
#include <engine/components/TextComponent.h>
#include <engine/components/UIComponents.h>
#include <engine/components/TweenComponent.h>
#include <engine/prefabs/PlayerPrefab.h>
#include <engine/systems/RenderSystem.h>
#include <SDL3/SDL.h>
#include <nlohmann/json.hpp>
#include <vector>
#include <string>
#include <functional>
#include <fstream>
#include <cstdio>
#include <cstring>
#include <cmath>

#ifndef QGAME_GAME_MANIFEST
#define QGAME_GAME_MANIFEST "game/game.json"
#endif

#ifndef QGAME_DEMO_PREFAB_MANIFEST
#define QGAME_DEMO_PREFAB_MANIFEST "game/prefabs/demo_prefabs.json"
#endif

#ifndef QGAME_DEMO_PREFAB_SCENE
#define QGAME_DEMO_PREFAB_SCENE "game/scenes/prefab_demo.scene.json"
#endif

// DemoGameInstance lets the existing large demo move onto the Game Framework
// lifecycle without first splitting every local variable into a separate state
// object. The callbacks deliberately stay small and explicit: onInit owns demo
// asset bootstrap, onUpdate owns the per-frame gameplay/demo controls, and
// onShutdown is the future hook for game-owned cleanup.
class DemoGameInstance final : public engine::GameInstance {
public:
	using InitFn = std::function<bool(engine::GameContext&)>;
	using UpdateFn = std::function<void(engine::GameContext&, float)>;
	using ShutdownFn = std::function<void(engine::GameContext&)>;

	DemoGameInstance(InitFn init, UpdateFn update, ShutdownFn shutdown = {})
		: init_(std::move(init))
		, update_(std::move(update))
		, shutdown_(std::move(shutdown)) {}

	bool onInit(engine::GameContext& ctx) override {
		return init_ ? init_(ctx) : true;
	}

	void onUpdate(engine::GameContext& ctx, float dt) override {
		if (update_) update_(ctx, dt);
	}

	void onShutdown(engine::GameContext& ctx) override {
		if (shutdown_) shutdown_(ctx);
	}

	void setUpdate(UpdateFn update) {
		update_ = std::move(update);
	}

private:
	InitFn init_;
	UpdateFn update_;
	ShutdownFn shutdown_;
};


static std::vector<uint8_t> makeCheckerboard(int w, int h, int cellSize, core::Color a, core::Color b) {
	std::vector<uint8_t> px(w * h * 4);
	for (int y = 0; y < h; ++y)
		for (int x = 0; x < w; ++x) {
			bool even = ((x / cellSize) + (y / cellSize)) % 2 == 0;
			core::Color c = even ? a : b;
			int i = (y * w + x) * 4;
			px[i] = c.r; px[i + 1] = c.g; px[i + 2] = c.b; px[i + 3] = c.a;
		}
	return px;
}

static std::vector<uint8_t> makeColorTileset(int tileSize, int cols, int rows) {
	int w = tileSize * cols, h = tileSize * rows;
	std::vector<uint8_t> px(w * h * 4, 0);
	core::Color palette[] = {
		{200, 60,  60,  255}, {60, 200, 60,  255},
		{60,  60,  200, 255}, {200,200, 60,  255},
		{200, 60,  200, 255}, {60, 200, 200, 255},
		{200, 140, 60,  255}, {140,140, 140, 255},
	};
	for (int row = 0; row < rows; ++row)
		for (int col = 0; col < cols; ++col) {
			core::Color c = palette[(row * cols + col) % 8];
			for (int ty = 0; ty < tileSize; ++ty)
				for (int tx = 0; tx < tileSize; ++tx) {
					bool border = tx == 0 || ty == 0 || tx == tileSize - 1 || ty == tileSize - 1;
					core::Color fc = border ? core::Color{ 30,30,30,255 } : c;
					int i = ((row * tileSize + ty) * w + col * tileSize + tx) * 4;
					px[i] = fc.r; px[i + 1] = fc.g; px[i + 2] = fc.b; px[i + 3] = fc.a;
				}
		}
	return px;
}

// 32×32 九宫格测试纹理：8 像素边框 + 16 像素中心。
//   - 边框深色 + 1px 内描边亮色（角变化能直观看出"角不缩放、边只拉伸"）
//   - 中心用纯色 → 拉伸到任意尺寸都不会出现颜色渐变
static std::vector<uint8_t> makeNineSliceTexture() {
	const int W = 32, H = 32, B = 8;
	std::vector<uint8_t> px(W * H * 4, 0);
	const core::Color border  = { 90, 70, 50, 255 };
	const core::Color hilight = { 200, 170, 110, 255 };
	const core::Color center  = { 245, 230, 200, 255 };
	for (int y = 0; y < H; ++y)
		for (int x = 0; x < W; ++x) {
			const bool inBorder = (x < B || x >= W - B || y < B || y >= H - B);
			core::Color c = inBorder ? border : center;
			// 内沿(border 与 center 接缝处)绘一圈高亮
			if ((x == B || x == W - B - 1) && y >= B - 1 && y <= H - B) c = hilight;
			if ((y == B || y == H - B - 1) && x >= B - 1 && x <= W - B) c = hilight;
			int i = (y * W + x) * 4;
			px[i] = c.r; px[i + 1] = c.g; px[i + 2] = c.b; px[i + 3] = c.a;
		}
	return px;
}

static std::vector<uint8_t> makeTextTexture(const std::string& text, core::Color color) {
	int charW = 8, charH = 16;
	int texW = static_cast<int>(text.length()) * charW;
	int texH = charH;
	std::vector<uint8_t> pixels(texW * texH * 4, 0);
	for (size_t i = 0; i < text.length(); ++i) {
		int cx = static_cast<int>(i) * charW;
		for (int py = 2; py < charH - 2; ++py) {
			for (int px = cx + 1; px < cx + charW - 1; ++px) {
				int idx = (py * texW + px) * 4;
				pixels[idx] = color.r;
				pixels[idx + 1] = color.g;
				pixels[idx + 2] = color.b;
				pixels[idx + 3] = color.a;
			}
		}
	}
	return pixels;
}

static std::vector<uint8_t> makeParticleTexture(int size) {
	std::vector<uint8_t> pixels(size * size * 4, 0);
	const float center = (size - 1) * 0.5f;
	const float radius = center > 0.f ? center : 1.f;
	for (int y = 0; y < size; ++y) {
		for (int x = 0; x < size; ++x) {
			const float dx = (x - center) / radius;
			const float dy = (y - center) / radius;
			const float d = std::sqrt(dx * dx + dy * dy);
			const float coreGlow = std::max(0.f, 1.f - d);
			const float softEdge = coreGlow * coreGlow;
			const int i = (y * size + x) * 4;
			pixels[i + 0] = static_cast<uint8_t>(255);
			pixels[i + 1] = static_cast<uint8_t>(220);
			pixels[i + 2] = static_cast<uint8_t>(80 + 120 * softEdge);
			pixels[i + 3] = static_cast<uint8_t>(255 * softEdge);
		}
	}
	return pixels;
}

// ─────────────────────────────────────────────────────────────────────────────
// LayoutGroup 测试：在指定画布上放三个布局容器，演示 Horizontal / Vertical / Grid
//
//   ┌─────── Vertical 列表 ────────┐ ┌── Horizontal 工具条 ──┐
//   │  ▣ Item A                    │ │ [A][B][C][D][E]       │
//   │  ▣ Item B                    │ └───────────────────────┘
//   │  ▣ Item C                    │ ┌──── Grid 物品栏 ─────┐
//   │  ▣ Item D                    │ │ [01][02][03][04][05] │
//   └──────────────────────────────┘ │ [06][07][08][09][10] │
//                                    │ ...                  │
//                                    └──────────────────────┘
//
// 子节点不需要再 setUIAnchor / setUIOffset —— LayoutGroup 每帧会覆盖。
// ─────────────────────────────────────────────────────────────────────────────
static void buildLayoutTest(engine::GameAPI& api, entt::entity canvas, engine::FontHandle font) {
	// ── 1) Vertical：左下角的列表 ────────────────────────────────────────────────
	{
		auto box = api.createLayoutGroup(220.f, 200.f, /*type=*/1 /*Vertical*/);
		api.setUIParent(box, canvas);
		api.setUIAnchor(box, 1.f, 1.f, 1.f, 1.f);          // 锚到右下
		api.setUIPivot(box, 1.f, 1.f);
		api.setUIOffset(box, -20.f, -260.f);
		api.setUIBackground(box, { 30, 35, 45, 220 }, {});
		api.setLayoutPadding(box, 8.f, 8.f, 8.f, 8.f);
		api.setLayoutSpacing(box, 0.f, 4.f);
		api.setLayoutCrossAlign(box, /*Stretch*/3);        // 横向铺满 → 每个按钮等宽

		const char* items[] = { "Item A", "Item B", "Item C", "Item D" };
		for (int i = 0; i < 4; ++i) {
			auto btn = api.createButton(0.f, 30.f, [name = std::string(items[i])]() {
				printf("[Layout-V] click %s\n", name.c_str());
			});
			api.setUIParent(btn, box);
			api.setButtonColors(btn,
				{ uint8_t(60 + i * 20), 90, 130, 255 },
				{ 110, 140, 180, 255 },
				{ 50,  70, 100, 255 });

			auto lbl = api.createUIText(0.f, 30.f, items[i]);
			api.setUIParent(lbl, btn);                     // 文本撑满按钮（其父非布局组）
			api.setUIAnchor(lbl, 0.f, 0.f, 1.f, 1.f);
			api.setUITextFont(lbl, font, 14.f);
			api.setUITextColor(lbl, { 230, 230, 230, 255 });
			api.setUISortOrder(lbl, 1);
		}
	}

	// ── 2) Horizontal：底部居中的小工具条 ────────────────────────────────────────
	{
		auto bar = api.createLayoutGroup(0.f, 40.f, /*type=*/0 /*Horizontal*/);
		api.setUIParent(bar, canvas);
		// 自定宽度：5 个 60px + 4 个 6px 间距 + padding 16 = 340
		api.setUISize(bar, 340.f, 40.f);
		api.setUIAnchor(bar, 0.5f, 1.f, 0.5f, 1.f);
		api.setUIPivot(bar, 0.5f, 1.f);
		api.setUIOffset(bar, 0.f, -50.f);
		api.setUIBackground(bar, { 25, 25, 35, 220 }, {});
		api.setLayoutPadding(bar, 8.f, 6.f, 8.f, 6.f);
		api.setLayoutSpacing(bar, 6.f, 0.f);
		api.setLayoutCrossAlign(bar, /*Center*/1);

		for (int i = 0; i < 5; ++i) {
			char label[8]; std::snprintf(label, sizeof(label), "%c", 'A' + i);
			auto btn = api.createButton(60.f, 28.f, [c = label[0]]() {
				printf("[Layout-H] click %c\n", c);
			});
			api.setUIParent(btn, bar);
			api.setButtonColors(btn,
				{ 70, 110, 90, 255 }, { 100, 160, 130, 255 }, { 50, 80, 65, 255 });

			auto lbl = api.createUIText(60.f, 28.f, label);
			api.setUIParent(lbl, btn);
			api.setUIAnchor(lbl, 0.f, 0.f, 1.f, 1.f);
			api.setUITextFont(lbl, font, 16.f);
			api.setUITextColor(lbl, { 240, 240, 240, 255 });
			api.setUISortOrder(lbl, 1);
		}
	}

	// ── 3) Grid：右上角的 5×4 物品栏 ────────────────────────────────────────────
	{
		const int cols = 5, rows = 4;
		const float cellW = 40.f, cellH = 40.f;
		const float pad = 6.f, sp = 4.f;
		const float w = pad * 2 + cols * cellW + (cols - 1) * sp;
		const float h = pad * 2 + rows * cellH + (rows - 1) * sp;

		auto grid = api.createLayoutGroup(w, h, /*type=*/2 /*Grid*/);
		api.setUIParent(grid, canvas);
		api.setUIAnchor(grid, 1.f, 1.f, 1.f, 1.f);
		api.setUIPivot(grid, 1.f, 1.f);
		api.setUIOffset(grid, -20.f, -20.f);
		api.setUIBackground(grid, { 35, 30, 25, 220 }, {});
		api.setLayoutPadding(grid, pad, pad, pad, pad);
		api.setLayoutSpacing(grid, sp, sp);
		api.setLayoutGridColumns(grid, cols);

		// 用 Image + Button 模拟物品格子；首个子节点的 width/height 决定单元格大小
		for (int i = 0; i < cols * rows; ++i) {
			auto slot = api.createButton(cellW, cellH, [i]() {
				printf("[Layout-Grid] slot %02d clicked\n", i + 1);
			});
			api.setUIParent(slot, grid);
			const uint8_t shade = static_cast<uint8_t>(60 + (i * 7) % 80);
			api.setButtonColors(slot,
				{ shade, shade, uint8_t(shade + 30), 255 },
				{ uint8_t(shade + 30), uint8_t(shade + 30), 220, 255 },
				{ uint8_t(shade - 20), uint8_t(shade - 20), 90, 255 });

			char num[4]; std::snprintf(num, sizeof(num), "%02d", i + 1);
			auto lbl = api.createUIText(cellW, cellH, num);
			api.setUIParent(lbl, slot);
			api.setUIAnchor(lbl, 0.f, 0.f, 1.f, 1.f);
			api.setUITextFont(lbl, font, 14.f);
			api.setUITextColor(lbl, { 240, 235, 200, 255 });
			api.setUISortOrder(lbl, 1);
		}
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// NineSlice 测试：放三块尺寸不同的面板，观察四角不缩放、四边只单轴拉伸、中心填满。
// 同时演示挂在按钮上做"高弹性按钮底图"。
// ─────────────────────────────────────────────────────────────────────────────
static void buildNineSliceTest(engine::GameAPI& api, entt::entity canvas,
                               TextureHandle nsTex, engine::FontHandle font) {
	auto makePanel = [&](float w, float h, float anchorX, float anchorY,
	                     float pivotX, float pivotY, float offX, float offY,
	                     const char* text) {
		auto p = api.createUIElement();
		api.setUIParent(p, canvas);
		api.setUISize(p, w, h);
		api.setUIAnchor(p, anchorX, anchorY, anchorX, anchorY);
		api.setUIPivot(p, pivotX, pivotY);
		api.setUIOffset(p, offX, offY);
		// 32×32 源纹理，每边 8 像素边框（与 makeNineSliceTexture 一致）
		api.setUINineSlice(p, nsTex, 8.f, 8.f, 8.f, 8.f);

		auto lbl = api.createUIText(w, h, text);
		api.setUIParent(lbl, p);
		api.setUIAnchor(lbl, 0.f, 0.f, 1.f, 1.f);
		api.setUITextFont(lbl, font, 14.f);
		api.setUITextColor(lbl, { 80, 60, 40, 255 });
		api.setUISortOrder(lbl, 1);
	};

	// 三块面板：宽窄、扁高、正方，验证拉伸正确性
	makePanel(280.f, 90.f, 0.5f, 0.f, 0.5f, 0.f, -200.f, 80.f, "9-slice 280x90");
	makePanel(140.f, 200.f, 0.5f, 0.f, 0.5f, 0.f, 0.f,    80.f, "9-slice 140x200");
	makePanel(180.f, 60.f, 0.5f, 0.f, 0.5f, 0.f, 200.f,  80.f, "9-slice 180x60");

	// 按钮也挂九宫格：保持四角清晰、不再用纯色块
	{
		auto btn = api.createButton(160.f, 50.f, []() {
			printf("[NineSlice] panel button clicked\n");
		});
		api.setUIParent(btn, canvas);
		api.setUIAnchor(btn, 0.5f, 0.f, 0.5f, 0.f);
		api.setUIPivot(btn, 0.5f, 0.f);
		api.setUIOffset(btn, 0.f, 200.f);
		// UIButton 自身的纯色矩形仍会绘制（baseSort 同层），把它调成透明让
		// NineSlice 透出来；hover/pressed 仍能改 tint。
		api.setButtonColors(btn, { 0,0,0,0 }, { 255,255,255,40 }, { 0,0,0,80 });
		api.setUINineSlice(btn, nsTex, 8.f, 8.f, 8.f, 8.f);

		auto lbl = api.createUIText(160.f, 50.f, "9-slice Button");
		api.setUIParent(lbl, btn);
		api.setUIAnchor(lbl, 0.f, 0.f, 1.f, 1.f);
		api.setUITextFont(lbl, font, 16.f);
		api.setUITextColor(lbl, { 80, 60, 40, 255 });
		api.setUISortOrder(lbl, 1);
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// Tooltip 测试：三种延迟、不同位置、含中文/换行的文本，以及一个无交互组件
// 但仅靠 UITooltip 也能触发的纯标签节点（验证命中过滤已包含 UITooltip）。
// ─────────────────────────────────────────────────────────────────────────────
static void buildTooltipTest(engine::GameAPI& api, entt::entity canvas, engine::FontHandle font) {
	// 1) 普通按钮：默认 0.4s 延迟
	{
		auto btn = api.createButton(140.f, 40.f, [](){ printf("[Tip] btn1\n"); });
		api.setUIParent(btn, canvas);
		api.setUIAnchor(btn, 0.f, 0.f, 0.f, 0.f);
		api.setUIOffset(btn, 200.f, 660.f);
		api.setButtonColors(btn, {80,120,80,255}, {110,160,110,255}, {60,90,60,255});
		api.setUITooltip(btn, "Hover 0.4s to see this tooltip", font, 14.f, 0.4f);

		auto lbl = api.createUIText(140.f, 40.f, "Tip A");
		api.setUIParent(lbl, btn);
		api.setUIAnchor(lbl, 0.f, 0.f, 1.f, 1.f);
		api.setUITextFont(lbl, font, 14.f);
		api.setUITextColor(lbl, {240,240,240,255});
		api.setUISortOrder(lbl, 1);
	}

	// 2) 立即弹出 (delay=0)，自定义颜色
	{
		auto btn = api.createButton(140.f, 40.f, nullptr);
		api.setUIParent(btn, canvas);
		api.setUIAnchor(btn, 0.f, 0.f, 0.f, 0.f);
		api.setUIOffset(btn, 350.f, 660.f);
		api.setButtonColors(btn, {120,80,80,255}, {160,110,110,255}, {90,60,60,255});
		api.setUITooltip(btn, "Damage: 24-32  |  Crit: 12%", font, 13.f, 0.f);
		api.setUITooltipColors(btn, {40,20,20,235}, {255,210,180,255});

		auto lbl = api.createUIText(140.f, 40.f, "Tip B (instant)");
		api.setUIParent(lbl, btn);
		api.setUIAnchor(lbl, 0.f, 0.f, 1.f, 1.f);
		api.setUITextFont(lbl, font, 14.f);
		api.setUITextColor(lbl, {240,240,240,255});
		api.setUISortOrder(lbl, 1);
	}

	// 3) 纯标签 + UITooltip：测试无 UIButton 也能触发
	{
		auto lbl = api.createUIText(140.f, 28.f, "Hover me (no button)");
		api.setUIParent(lbl, canvas);
		api.setUIAnchor(lbl, 0.f, 0.f, 0.f, 0.f);
		api.setUIOffset(lbl, 200.f, 710.f);
		api.setUITextFont(lbl, font, 13.f);
		api.setUITextColor(lbl, {180,200,255,255});
		api.setUISize(lbl, 200.f, 28.f);
		api.setUITooltip(lbl, "Plain label can also have a tooltip", font, 12.f, 0.6f);
	}
}

// ── UIMask 测试 ─────────────────────────────────────────────────────────────
// 一个 200x120 的裁剪框，里头放一张超出边界的大色块 + 一行长文本，
// 还有一个会跟动画往返穿过裁剪边界的方块。
// 右侧另一个相同尺寸的"无 mask"对照组，验证未启用 mask 时确实溢出。
// 用一个外部按钮可以运行时切换 mask 启用/禁用。
static void buildMaskTest(engine::GameAPI& api, entt::entity canvas,
                          engine::FontHandle font,
                          entt::entity& outRunner,
                          entt::entity& outMaskNode,
                          entt::entity& outMaskHint) {
	auto title = api.createUIText(420.f, 22.f, "UIMask: clipped vs un-clipped");
	api.setUIParent(title, canvas);
	api.setUIAnchor(title, 0.f, 1.f, 0.f, 1.f);
	api.setUIPivot(title, 0.f, 1.f);
	api.setUIOffset(title, 320.f, -270.f);
	api.setUITextFont(title, font, 14.f);
	api.setUITextColor(title, { 200, 220, 255, 255 });

	auto buildPanel = [&](float ox, const char* tag, bool useMask) -> entt::entity {
		auto panel = api.createUIElement();
		api.setUIParent(panel, canvas);
		api.setUISize(panel, 200.f, 120.f);
		api.setUIAnchor(panel, 0.f, 1.f, 0.f, 1.f);
		api.setUIPivot(panel, 0.f, 1.f);
		api.setUIOffset(panel, ox, -140.f);
		api.setUIBackground(panel, { 30, 35, 50, 255 }, {});
		if (useMask) api.setUIMask(panel, true);

		// 一个故意比父框还大的彩色子块（左上角对齐，向右下溢出）
		auto big = api.createUIImage(320.f, 200.f);
		api.setUIParent(big, panel);
		api.setUIAnchor(big, 0.f, 0.f, 0.f, 0.f);
		api.setUIPivot(big, 0.f, 0.f);
		api.setUIOffset(big, -20.f, -30.f);
		api.setUIImageColor(big, useMask ? core::Color{ 80, 160, 220, 200 }
		                                 : core::Color{ 220, 120, 80, 200 });
		api.setUIInteractable(big, false);

		// 长文本，超出右边界
		auto txt = api.createUIText(420.f, 22.f,
		    "AAAAAAAAAA BBBBBBBBBB CCCCCCCCCC DDDDDDDDDD");
		api.setUIParent(txt, panel);
		api.setUIAnchor(txt, 0.f, 0.f, 0.f, 0.f);
		api.setUIPivot(txt, 0.f, 0.f);
		api.setUIOffset(txt, 6.f, 6.f);
		api.setUITextFont(txt, font, 14.f);
		api.setUITextColor(txt, { 250, 250, 250, 255 });
		api.setUIInteractable(txt, false);

		auto label = api.createUIText(200.f, 18.f, tag);
		api.setUIParent(label, panel);
		api.setUIAnchor(label, 0.f, 1.f, 1.f, 1.f);
		api.setUIPivot(label, 0.5f, 1.f);
		api.setUIOffset(label, 0.f, -2.f);
		api.setUITextFont(label, font, 12.f);
		api.setUITextColor(label, { 200, 200, 200, 255 });
		api.setUIInteractable(label, false);
		return panel;
	};

	outMaskNode = buildPanel(320.f, "with UIMask", true);
	(void)buildPanel(540.f, "no mask",     false);

	// 在带 mask 的面板里放一个会被动画移动的方块——验证它穿越边界时被裁掉。
	outRunner = api.createUIImage(40.f, 40.f);
	api.setUIParent(outRunner, outMaskNode);
	api.setUIAnchor(outRunner, 0.f, 0.f, 0.f, 0.f);
	api.setUIPivot(outRunner, 0.f, 0.f);
	api.setUIOffset(outRunner, 0.f, 70.f);
	api.setUIImageColor(outRunner, { 240, 220, 100, 255 });
	api.setUIInteractable(outRunner, false);

	// 切换按钮：运行时开关 mask，验证视觉立即刷新。
	auto btn = api.createButton(180.f, 28.f, nullptr);
	api.setUIParent(btn, canvas);
	api.setUIAnchor(btn, 0.f, 1.f, 0.f, 1.f);
	api.setUIPivot(btn, 0.f, 1.f);
	api.setUIOffset(btn, 320.f, -110.f);
	api.setButtonColors(btn, {70,90,120,255}, {110,140,180,255}, {50,70,100,255});

	outMaskHint = api.createUIText(180.f, 28.f, "Mask: ON  (click to toggle)");
	api.setUIParent(outMaskHint, btn);
	api.setUIAnchor(outMaskHint, 0.f, 0.f, 1.f, 1.f);
	api.setUITextFont(outMaskHint, font, 12.f);
	api.setUITextColor(outMaskHint, { 240, 240, 240, 255 });
	api.setUISortOrder(outMaskHint, 1);

	api.setButtonCallback(btn, [&api, panel = outMaskNode, hint = outMaskHint]() {
		static bool on = true;
		on = !on;
		api.setUIMask(panel, on);
		api.setUIText(hint, on ? "Mask: ON  (click to toggle)"
		                       : "Mask: OFF (click to toggle)");
	});
}

// ─────────────────────────────────────────────────────────────────────────────
// Modal 测试：
//   左下角放一个 "Open Modal" 触发按钮 + 一个 "Bottom Btn" 干扰按钮 (验证模态
//   打开后它无法被点击)。点击 Open Modal 后构造一个对话框：
//     - 半透明黑底全屏遮罩 (UIModal.drawOverlay 默认开)
//     - 居中 NineSlice 风格面板，上面一行标题、一个 "OK" 按钮、一个 "Cancel" 按钮
//     - 面板内部还塞一个按钮专门验证"模态内的 button 仍可点击"
//   点击遮罩区域 (面板外) → onClickOutside 触发 → 关闭模态 (业务自行决定)。
//   注意：模态对话框节点不需要手动调 setUILayer —— UISystem 通过 layerBoost
//   自动把整棵子树抬到所有普通 UI 之上。
// ─────────────────────────────────────────────────────────────────────────────
static void buildModalTest(engine::GameAPI& api, entt::entity canvas, engine::FontHandle font) {
	// 模态对话框根节点 (默认 visible=true，但我们显式 setUIModal(false) 让它一开始
	// 处于"未激活模态"状态——节点本身仍然存在 + 不可见，等被打开时才上去)。
	auto modal = api.createUIElement();
	api.setUIParent(modal, canvas);
	api.setUISize(modal, 400.f, 220.f);
	api.setUIAnchor(modal, 0.5f, 0.5f, 0.5f, 0.5f);
	api.setUIPivot(modal, 0.5f, 0.5f);
	api.setUIOffset(modal, 0.f, 0.f);
	api.setUIBackground(modal, { 60, 60, 70, 245 }, {});
	api.setUIVisible(modal, false);
	api.setUIModal(modal, false);   // 起始未激活
	api.setUIModalOverlay(modal, true, { 0, 0, 0, 160 });

	// 标题文字
	{
		auto title = api.createUIText(380.f, 30.f, "Modal Dialog");
		api.setUIParent(title, modal);
		api.setUIAnchor(title, 0.5f, 0.f, 0.5f, 0.f);
		api.setUIPivot(title, 0.5f, 0.f);
		api.setUIOffset(title, 0.f, 14.f);
		api.setUITextFont(title, font, 18.f);
		api.setUITextColor(title, { 240, 240, 240, 255 });
	}

	// 说明文字
	{
		auto info = api.createUIText(360.f, 60.f,
			"Click outside the panel\nor press OK / Cancel to close.");
		api.setUIParent(info, modal);
		api.setUIAnchor(info, 0.5f, 0.5f, 0.5f, 0.5f);
		api.setUIPivot(info, 0.5f, 0.5f);
		api.setUIOffset(info, 0.f, -10.f);
		api.setUITextFont(info, font, 13.f);
		api.setUITextColor(info, { 200, 200, 210, 255 });
	}

	// 一个关闭模态的辅助 lambda：同时清掉 modal 与 visible
	auto closeModal = [&api, modal]() {
		api.setUIModal(modal, false);
		api.setUIVisible(modal, false);
	};

	// OK 按钮：打印并关闭
	{
		auto btn = api.createButton(100.f, 36.f, [closeModal]() {
			printf("[Modal] OK clicked\n");
			closeModal();
		});
		api.setUIParent(btn, modal);
		api.setUIAnchor(btn, 0.5f, 1.f, 0.5f, 1.f);
		api.setUIPivot(btn, 1.f, 1.f);
		api.setUIOffset(btn, -10.f, -16.f);
		api.setButtonColors(btn, { 70, 130, 80, 255 }, { 100, 170, 110, 255 }, { 50, 100, 60, 255 });
		auto lbl = api.createUIText(100.f, 36.f, "OK");
		api.setUIParent(lbl, btn);
		api.setUIAnchor(lbl, 0.f, 0.f, 1.f, 1.f);
		api.setUITextFont(lbl, font, 14.f);
		api.setUITextColor(lbl, { 240, 240, 240, 255 });
		api.setUISortOrder(lbl, 1);
	}

	// Cancel 按钮：仅关闭
	{
		auto btn = api.createButton(100.f, 36.f, [closeModal]() {
			printf("[Modal] Cancel clicked\n");
			closeModal();
		});
		api.setUIParent(btn, modal);
		api.setUIAnchor(btn, 0.5f, 1.f, 0.5f, 1.f);
		api.setUIPivot(btn, 0.f, 1.f);
		api.setUIOffset(btn, 10.f, -16.f);
		api.setButtonColors(btn, { 130, 70, 70, 255 }, { 170, 100, 100, 255 }, { 100, 50, 50, 255 });
		auto lbl = api.createUIText(100.f, 36.f, "Cancel");
		api.setUIParent(lbl, btn);
		api.setUIAnchor(lbl, 0.f, 0.f, 1.f, 1.f);
		api.setUITextFont(lbl, font, 14.f);
		api.setUITextColor(lbl, { 240, 240, 240, 255 });
		api.setUISortOrder(lbl, 1);
	}

	// 点遮罩自动关闭
	api.setUIModalOnClickOutside(modal, [closeModal]() {
		printf("[Modal] click outside → close\n");
		closeModal();
	});

	// ── 触发器：左下角的 "Open Modal" 按钮 ──────────────────────────────────────
	{
		auto open = api.createButton(140.f, 36.f, [&api, modal]() {
			printf("[Modal] open\n");
			api.setUIVisible(modal, true);
			api.setUIModal(modal, true);
		});
		api.setUIParent(open, canvas);
		api.setUIAnchor(open, 0.f, 1.f, 0.f, 1.f);
		api.setUIPivot(open, 0.f, 1.f);
		api.setUIOffset(open, 20.f, -80.f);
		api.setButtonColors(open, { 80, 110, 160, 255 }, { 110, 140, 200, 255 }, { 60, 80, 120, 255 });
		auto lbl = api.createUIText(140.f, 36.f, "Open Modal");
		api.setUIParent(lbl, open);
		api.setUIAnchor(lbl, 0.f, 0.f, 1.f, 1.f);
		api.setUITextFont(lbl, font, 14.f);
		api.setUITextColor(lbl, { 240, 240, 240, 255 });
		api.setUISortOrder(lbl, 1);
	}

	// ── 干扰按钮：验证模态打开时它无法被点击 (鼠标穿透被拒) ─────────────────────
	{
		auto noisy = api.createButton(140.f, 36.f, []() {
			// 若模态生效，模态打开期间这条永远不应当出现
			printf("[Modal] BOTTOM BTN clicked (should NOT print while modal open)\n");
		});
		api.setUIParent(noisy, canvas);
		api.setUIAnchor(noisy, 0.f, 1.f, 0.f, 1.f);
		api.setUIPivot(noisy, 0.f, 1.f);
		api.setUIOffset(noisy, 20.f, -36.f);
		api.setButtonColors(noisy, { 120, 80, 80, 255 }, { 160, 110, 110, 255 }, { 90, 60, 60, 255 });
		auto lbl = api.createUIText(140.f, 36.f, "Bottom Btn");
		api.setUIParent(lbl, noisy);
		api.setUIAnchor(lbl, 0.f, 0.f, 1.f, 1.f);
		api.setUITextFont(lbl, font, 14.f);
		api.setUITextColor(lbl, { 240, 240, 240, 255 });
		api.setUISortOrder(lbl, 1);
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// TweenSystem 增强测试：验证三件新事 (delay / onUpdate / onComplete) 能在
// 不耦合 UISystem 的前提下驱动 UI 字段。布局：
//
//   ┌─ "Animate Card" 按钮  (顶部偏右，点击启动动画链)
//   │
//   └─ 一张彩色卡片 + 标题文字 (居中偏右)
//        Phase 1: delay 0.3s → fade in (alpha 0→230) + 下滑 (offsetY -40→0)，0.4s
//        Phase 2 (在 Phase 1 onComplete 里挂)：保持 0.6s (delay) → fade out + 上滑，0.35s
//        Phase 3 (Phase 2 onComplete 里)：把卡片 visible=false，按钮文字改回 "Animate Card"
//
// 关键点：
//   - 通道用 Custom，TweenSystem 完全不知道写到的是 UI 字段
//   - delay 期间 outValue 钉在 from (语义 B)，所以"按下→0.3s 内卡片就已经被
//     拉到 alpha=0、offsetY=-40 的起点"，0.3s 后才开始可见的过渡
//   - onComplete 里安全地 emplace_or_replace<TweenComponent>，因为 system 把
//     回调延迟到了所有遍历结束之后
// ─────────────────────────────────────────────────────────────────────────────
static void buildTweenTest(engine::GameAPI& api, entt::entity canvas, engine::FontHandle font) {
	using engine::TweenComponent;
	using engine::TweenInstance;
	using engine::TweenChannel;
	using engine::Easing;

	// 卡片：纯色背景 + 标题。初始 visible=false，等按钮触发动画时才打开。
	const float cardW = 240.f, cardH = 90.f;
	const float restY = 0.f;            // 动画终点 offsetY
	const float startY = -40.f;         // 动画起点 offsetY (向上偏移 40px)
	auto card = api.createUIElement();
	api.setUIParent(card, canvas);
	api.setUISize(card, cardW, cardH);
	api.setUIAnchor(card, 0.5f, 0.f, 0.5f, 0.f);
	api.setUIPivot(card, 0.5f, 0.f);
	api.setUIOffset(card, 260.f, 60.f + restY);
	api.setUIBackground(card, { 60, 140, 200, 0 }, {});  // 起始 alpha=0
	api.setUIVisible(card, false);
	api.setUISortOrder(card, 100);  // 确保它在按钮之上

	auto cardLabel = api.createUIText(cardW, cardH, "Tween Demo");
	api.setUIParent(cardLabel, card);
	api.setUIAnchor(cardLabel, 0.f, 0.f, 1.f, 1.f);
	api.setUITextFont(cardLabel, font, 16.f);
	api.setUITextColor(cardLabel, { 250, 250, 250, 255 });
	api.setUISortOrder(cardLabel, 1);

	// 触发按钮 + 标签 (entity 提前声明，便于在动画回调里改文字)
	auto btn      = api.createButton(160.f, 36.f, nullptr);
	auto btnLabel = api.createUIText(160.f, 36.f, "Animate Card");
	api.setUIParent(btn, canvas);
	api.setUIAnchor(btn, 0.5f, 0.f, 0.5f, 0.f);
	api.setUIPivot(btn, 0.5f, 0.f);
	api.setUIOffset(btn, 260.f, 16.f);
	api.setButtonColors(btn, { 80, 110, 160, 255 }, { 110, 140, 200, 255 }, { 60, 80, 120, 255 });
	api.setUIParent(btnLabel, btn);
	api.setUIAnchor(btnLabel, 0.f, 0.f, 1.f, 1.f);
	api.setUITextFont(btnLabel, font, 14.f);
	api.setUITextColor(btnLabel, { 240, 240, 240, 255 });
	api.setUISortOrder(btnLabel, 1);

	// onUpdate 闭包共用：把归一化值 v∈[0,1] 写成 (alpha, offsetY)，
	// alpha=v*230, offsetY 在 startY..restY 之间 lerp。
	const auto applyCardProgress = [&api, card, startY, restY](float v) {
		const uint8_t a = static_cast<uint8_t>(v * 230.f + 0.5f);
		api.setUIBackground(card, { 60, 140, 200, a }, {});
		const float y = startY + (restY - startY) * v;
		// 卡片相对 canvas 锚点的偏移：x 不变，y 跟着动画走
		api.setUIOffset(card, 260.f, 60.f + y);
	};

	// 动画启动器 (放进闭包反复用：按钮触发即调；Phase 1 onComplete 里同样
	// 通过 emplace_or_replace 启动 Phase 2)。
	api.setButtonOnClick(btn, [&api, card, btnLabel, applyCardProgress]() {
		// 防重入：如果卡片已经可见，认为动画进行中，按一下不再触发
		if (api.hasComponent<engine::UINode>(card) &&
			api.getComponent<engine::UINode>(card).visible) {
			return;
		}

		api.setUIText(btnLabel, "Animating...");
		api.setUIVisible(card, true);

		// ── Phase 1：延迟 0.3s → fade in + slide down 0.4s ──────────────────
		TweenInstance t1{};
		t1.channel  = TweenChannel::Custom;
		t1.from     = 0.f;
		t1.to       = 1.f;
		t1.duration = 0.4f;
		t1.delay    = 0.3f;            // ← 测 delay：按下后 0.3s 才看到动起来
		t1.easing   = Easing::QuadOut;
		t1.onUpdate = applyCardProgress;
		t1.onComplete = [&api, card, btnLabel, applyCardProgress]() {
			// ── Phase 2：保持 0.6s (delay) → fade out + slide up 0.35s ─────
			TweenInstance t2{};
			t2.channel  = TweenChannel::Custom;
			t2.from     = 1.f;          // 反向：从 1 衰减到 0
			t2.to       = 0.f;
			t2.duration = 0.35f;
			t2.delay    = 0.6f;         // ← 再次测 delay：定格 0.6s 让人看清
			t2.easing   = Easing::QuadIn;
			t2.onUpdate = applyCardProgress;
			t2.onComplete = [&api, card, btnLabel]() {
				// ── Phase 3：清场 ───────────────────────────────────────────
				api.setUIVisible(card, false);
				api.setUIText(btnLabel, "Animate Card");
				printf("[Tween] full cycle done\n");
			};
			TweenComponent tc2;
			tc2.add(t2);
			api.addComponent<TweenComponent>(card, std::move(tc2));
		};

		TweenComponent tc1;
		tc1.add(t1);
		api.addComponent<TweenComponent>(card, std::move(tc1));
	});
}

int main(int argc, char* argv[]) {
	bool useOpenGL = false;
	for (int i = 1; i < argc; ++i) {
		if (strcmp(argv[i], "--opengl") == 0 || strcmp(argv[i], "-gl") == 0) {
			useOpenGL = true;
		}
	}

	engine::EngineConfig cfg;
	cfg.windowTitle = "StarEngine - Two Camera Demo";
	cfg.windowWidth = 1280;
	cfg.windowHeight = 720;
	cfg.debug = true;
	if (useOpenGL) {
		cfg.renderBackend = engine::RenderBackend::OpenGL;
	}
	engine::EngineContext ctx;
	ctx.init(cfg);

	engine::GameAPI api{ ctx };
	engine::GameContext gameCtx{ ctx };
	engine::PrefabRegistry prefabs{ gameCtx };
	engine::SceneManager sceneManager{ gameCtx };
	gameCtx.api = &api;

	engine::GameManifest gameManifest;
	TextureHandle character;
	engine::FontHandle font;
	AnimationHandle playerAnim;
	DemoGameInstance demo{
		[&](engine::GameContext& fw) {
			// game.json is now the demo project's framework entry. The scene is
			// still built programmatically below, so startupScene is used as the
			// stable identity for this demo until Scene/Prefab data migration lands.
			if (!engine::GameManifestLoader::loadFromFile(QGAME_GAME_MANIFEST, gameManifest)) {
				printf("[Game] failed to load game manifest: %s\n", QGAME_GAME_MANIFEST);
				return false;
			}
			printf("[Game] %s (%s) startupScene=%s\n",
				gameManifest.name.c_str(),
				gameManifest.version.c_str(),
				gameManifest.startupScene.c_str());

			const bool manifestOk = api.loadAssetManifest(QGAME_BAKED_MANIFEST);
			character = api.loadTextureById("texture.demo.character");
			font = api.loadFontById("font.demo.main");
			playerAnim = api.loadAnimationById("animation.demo.test");
			if (!manifestOk || !character.valid() || !font.valid() || !playerAnim.valid()) {
				printf("[Assets] failed to load baked QPAK manifest or demo assets\n");
				return false;
			}
			if (!prefabs.registerManifest(QGAME_DEMO_PREFAB_MANIFEST)) {
				printf("[PrefabDataTest] failed to register prefab manifest: %s\n", QGAME_DEMO_PREFAB_MANIFEST);
				return false;
			}

			// Scene/Prefab 数据化自检：把 prefab scene 加载到临时 registry，验证
			// SceneSerializer 能解析 prefab 引用和局部组件覆盖。这里不加载到主
			// world，避免清空后面的程序化 demo 场景。
			entt::registry prefabSceneSmokeWorld;
			if (!engine::SceneSerializer::loadScene(
					prefabSceneSmokeWorld,
					api.assetManager(),
					QGAME_DEMO_PREFAB_SCENE,
					&prefabs)) {
				printf("[PrefabDataTest] failed to load prefab scene: %s\n", QGAME_DEMO_PREFAB_SCENE);
				return false;
			}
			auto smokeView = prefabSceneSmokeWorld.view<engine::Name, engine::Transform, engine::Sprite>();
			printf("[PrefabDataTest] prefab scene entities: %zu\n", static_cast<size_t>(smokeView.size_hint()));

			(void)fw;
			return true;
		},
		{}
	};

	if (!demo.onInit(gameCtx)) {
		ctx.shutdown();
		return 1;
	}
	// ── 上传程序化纹理 ────────────────────────────────────────────────────────
	auto checkerPx = makeCheckerboard(64, 64, 8, { 255,100,100,255 }, { 100,100,255,255 });
	TextureHandle spriteTex = api.createTextureFromMemory(checkerPx.data(), 64, 64);

	auto tilesetPx = makeColorTileset(32, 4, 2);
	TextureHandle tilesetTex = api.createTextureFromMemory(tilesetPx.data(), 128, 64);

	auto movingPx = makeTextTexture("Moving", { 100, 255, 100, 255 });
	TextureHandle movingTex = api.createTextureFromMemory(movingPx.data(), 56, 16);

	auto stoppedPx = makeTextTexture("Stopped", { 255, 100, 100, 255 });
	TextureHandle stoppedTex = api.createTextureFromMemory(stoppedPx.data(), 56, 16);

	auto particlePx = makeParticleTexture(32);
	TextureHandle particleTex = api.createTextureFromMemory(particlePx.data(), 32, 32);

	// Phase 1 测试：构造一个 one-shot "attack" clip，复用 spriteTex
	AnimationHandle attackAnim;
	{
		engine::AnimationClip clip;
		clip.name = "attack_test";
		clip.texture = spriteTex;
		clip.loop = false;
		auto pushFrame = [&](float x, float y, float dur) {
			engine::AnimationFrame f;
			f.srcRect = { x, y, 32.f, 32.f };
			f.duration = dur;
			clip.frames.push_back(f);
			clip.duration += dur;
			};
		pushFrame(0.f, 0.f, 0.10f);
		pushFrame(32.f, 0.f, 0.10f);
		pushFrame(32.f, 32.f, 0.10f);
		pushFrame(0.f, 32.f, 0.10f);
		// Phase 2: 帧事件 — 攻击的 hit window + sfx
		clip.events.push_back({ 0.05f, "sfx",         0, 0.f, "swing" });
		clip.events.push_back({ 0.10f, "hitbox_on",   0, 0.f, "" });
		clip.events.push_back({ 0.30f, "hitbox_off",  0, 0.f, "" });
		clip.events.push_back({ 0.39f, "vfx",         0, 0.f, "spark" });
		attackAnim = api.assetManager().registerAnimation("attack_test", clip);
	}

	// ═══════════════════════════════════════════════════════════════════════════
	// 相机 1：World 渲染 (depth=0，先绘制，清屏)
	// ═══════════════════════════════════════════════════════════════════════════
	entt::entity worldCamera;
	{
		worldCamera = api.spawnEntity();
		api.addComponent(worldCamera, engine::Transform{ 400.0f, 200.0f });
		engine::Camera cam{};
		cam.zoom = 1.5f;
		cam.primary = true;
		cam.depth = 0;
		cam.layerMask = engine::renderPassBit(engine::RenderPass::World);
		cam.clear = true;
		//cam.clearColor = core::Color{ 20, 20, 40, 255 };
		cam.cullEnabled = true;
		api.addComponent(worldCamera, cam);
	}

	// ═══════════════════════════════════════════════════════════════════════════
	// 相机 2：UI 叠加渲染 (depth=1，后绘制，不清屏)
	// ═══════════════════════════════════════════════════════════════════════════
	{
		auto e = api.spawnEntity();
		api.addComponent(e, engine::Transform{ 0.f, 0.f });
		engine::Camera cam{};
		cam.zoom = 1.f;
		cam.primary = true;
		cam.depth = 1;
		cam.layerMask = engine::renderPassBit(engine::RenderPass::UI) | engine::renderPassBit(engine::RenderPass::Screen);
		cam.clear = false;
		cam.cullEnabled = false;
		api.addComponent(e, cam);
	}

	// ── TileMap ────────────────────────────────────────────────────────────────
	{
		auto e = api.spawnEntity();
		api.addComponent(e, engine::Transform{ 0.f, 400.f });
		engine::TileMap tmap{};
		tmap.width = 20; tmap.height = 5; tmap.tileSize = 32;
		engine::TileMap::Tileset ts{};
		ts.texture = tilesetTex;
		ts.firstGid = 0;
		ts.count = 8;
		ts.columns = 8;
		// Demo map keeps every tile non-blocking; the v2 runtime stores explicit
		// collision profiles, while legacyCollision remains available for old data.
	
		tmap.tilesets.push_back(ts);
		engine::TileMap::Layer ground{};
		ground.name = "ground";
		ground.renderLayer = 0;
		for (int y = 0; y < 5; ++y)
			for (int x = 0; x < 20; ++x)
				ground.tiles.push_back((x + y) % 8);
		engine::TileMap::Layer objects{};
		objects.name = "objects";
		objects.renderLayer = 1;
		objects.tiles.resize(100, -1);
		objects.tiles[2] = 4; objects.tiles[12] = 5;
		tmap.layers.push_back(ground);
		tmap.layers.push_back(objects);
		api.addComponent(e, tmap);
	}

	// ── Sprite 1：原始大小 ─────────────────────────────────────────────────────
	{
		auto e = api.spawnEntity();
		api.addComponent(e, engine::Transform{ 200.f, 200.f });
		engine::Sprite sp{};
		sp.texture = spriteTex;
		sp.srcRect = { 0.f, 0.f, 64.f, 64.f };
		sp.layer = 1;
		sp.pass = engine::RenderPass::World;
		api.addComponent(e, sp);
	}

	// ── Sprite 2：旋转 45°，放大 2×，半透明橙色 tint ──────────────────────────
	{
		auto e = api.spawnEntity();
		engine::Transform tf{}; tf.x = 400.f; tf.y = 200.f;
		tf.rotation = 0.785f; tf.scaleX = 2.f; tf.scaleY = 2.f;
		api.addComponent(e, tf);
		engine::Sprite sp{};
		sp.texture = spriteTex;
		sp.srcRect = { 0.f, 0.f, 64.f, 64.f };
		sp.layer = 1;
		sp.pass = engine::RenderPass::World;
		sp.tint = { 255, 200, 100, 200 };
		api.addComponent(e, sp);
	}

	// ── Sprite 3：横向拉伸，绿色 tint ────────────────────────────────────────
	{
		auto e = api.spawnEntity();
		engine::Transform tf{}; tf.x = 640.f; tf.y = 200.f;
		tf.scaleX = 3.f; tf.scaleY = 1.5f;
		api.addComponent(e, tf);
		engine::Sprite sp{};
		sp.texture = spriteTex;
		sp.srcRect = { 0.f, 0.f, 64.f, 64.f };
		sp.layer = 2;
		sp.pass = engine::RenderPass::World;
		sp.tint = { 100, 255, 100, 255 };
		api.addComponent(e, sp);
	}

	// ── GPU Sprite Particle Demo ──────────────────────────────────────────────
	// CPU 只负责发射初始粒子；GPU compute 负责生命周期/位置推进、alive list、
	// 排序和 indirect draw 参数。粒子纹理是程序化 soft sprite，所以不依赖额外资产。
	//entt::entity particleEmitter = entt::null;
	//{
	//	particleEmitter = api.spawnEntity();
	//	engine::Transform tf{};
	//	tf.x = 520.f;
	//	tf.y = 260.f;
	//	tf.rotation = -1.35f;
	//	api.addComponent(particleEmitter, tf);

	//	engine::ParticleComponent particles{};
	//	particles.texture = particleTex;
	//	particles.srcRect = { 0.f, 0.f, 32.f, 32.f };
	//	particles.maxParticles = 256;
	//	particles.emissionRate = 180.f;
	//	particles.lifetime = 1.25f;
	//	particles.speedMin = 35.f;
	//	particles.speedMax = 160.f;
	//	particles.sizeStart = 18.f;
	//	particles.sizeEnd = 2.f;
	//	particles.spread = 1.15f;
	//	particles.colorStart = { 255, 245, 140, 230 };
	//	particles.colorEnd = { 255, 80, 40, 0 };
	//	particles.layer = 8;
	//	particles.sortMode = engine::ParticleSortMode::Y;
	//	particles.pass = engine::RenderPass::World;
	//	api.addComponent(particleEmitter, particles);
	//}

	// ── S2 Scene/Prefab 数据化可视测试 ─────────────────────────────────────────
	// prefab_demo.scene.json 里目前有两个 prefab instance。这里把它们实例化进
	// 当前 world，但同一时间只显示一个；按 N 键在两个 JSON 实例之间切换。
	std::vector<entt::entity> prefabSceneEntities;
	std::vector<engine::Transform> prefabSceneTransforms;
	int activePrefabSceneIndex = 0;
	auto applyPrefabSceneSwitch = [&]() {
		for (size_t i = 0; i < prefabSceneEntities.size(); ++i) {
			entt::entity e = prefabSceneEntities[i];
			if (e == entt::null || !api.hasComponent<engine::Sprite>(e)) continue;
			api.patchComponent<engine::Sprite>(e, [&](engine::Sprite& sprite) {
				sprite.visible = static_cast<int>(i) == activePrefabSceneIndex;
			});
		}
	};
	{
		std::ifstream sceneInput(QGAME_DEMO_PREFAB_SCENE);
		if (!sceneInput) {
			printf("[PrefabDataTest] cannot read prefab scene: %s\n", QGAME_DEMO_PREFAB_SCENE);
		} else {
			nlohmann::json sceneJson;
			sceneInput >> sceneJson;
			int index = 0;
			for (const auto& entityJson : sceneJson.value("entities", nlohmann::json::array())) {
				const std::string prefabId = entityJson.value("prefab", "");
				const nlohmann::json overrides = entityJson.value("components", nlohmann::json::object());
				entt::entity e = prefabs.instantiate(prefabId, ctx.world, api.assetManager(), overrides);
				const bool ok = e != entt::null &&
					api.hasComponent<engine::Name>(e) &&
					api.hasComponent<engine::Transform>(e) &&
					api.hasComponent<engine::Sprite>(e);
				printf("[PrefabDataTest] scene instance %d (%s): %s\n", index, prefabId.c_str(), ok ? "OK" : "FAILED");
				if (ok) {
					prefabSceneEntities.push_back(e);
					prefabSceneTransforms.push_back(api.getComponent<engine::Transform>(e));
				}
				++index;
			}
			applyPrefabSceneSwitch();
		}
	}

	// ── Region Tint Demo：10 个不同 LUT 的角色 + 颜色平滑切换 ───────────────
	//   sprite/char.png 是纯白可染色区域 + 黑色眼睛；sibling char.id.png 提供 region ID 图。
	//   region IDs：1=皮肤  2=头发  3=上衣  4=裤子  5=鞋
	struct TintPalette {
		core::Color skin, hair, shirt, pants, shoes;
	};
	static const TintPalette kTintPalettes[10] = {
		{ {255,210,170,255}, { 80, 50, 30,255}, {240,240,240,255}, { 60, 80,160,255}, { 40, 30, 20,255} },
		{ {220,190,150,255}, { 60,100, 40,255}, { 90,140, 70,255}, { 50, 80, 30,255}, { 70, 50, 30,255} },
		{ {255,225,200,255}, {220,120,180,255}, {255,180,210,255}, {200, 80,140,255}, {120, 60, 90,255} },
		{ {255,200,170,255}, {180, 30, 30,255}, {220, 60, 50,255}, {120, 30, 30,255}, { 60, 20, 20,255} },
		{ {220,200,180,255}, {120, 60,180,255}, {140, 80,200,255}, { 70, 40,120,255}, { 40, 20, 70,255} },
		{ {255,220,180,255}, {240,200, 60,255}, {255,230, 80,255}, {200,160, 30,255}, {120, 90, 30,255} },
		{ {230,230,255,255}, {200,200,220,255}, {220,220,240,255}, {180,180,210,255}, {140,140,170,255} },
		{ {220,220,200,255}, { 30,140,160,255}, { 60,180,200,255}, { 30, 90,120,255}, { 20, 50, 70,255} },
		{ {255,210,170,255}, {200, 80, 30,255}, {255,140, 50,255}, {120, 60, 30,255}, { 60, 30, 10,255} },
		{ { 90, 70, 60,255}, { 30, 30, 40,255}, { 50, 50, 70,255}, { 20, 20, 30,255}, { 10, 10, 20,255} },
	};
	std::vector<entt::entity> tintDemoEntities;
	//{
	//	TextureHandle charTex = api.assetManager().loadTexture("assets/sprites/char.png");
	//	if (charTex.valid()) {
	//		for (int i = 0; i < 10; ++i) {
	//			auto e = api.spawnEntity();
	//			engine::Transform tf{};
	//			tf.x = 60.f + (i % 5) * 110.f;
	//			tf.y = -150.f + (i / 5) * 200.f;
	//			tf.scaleX = tf.scaleY = 2.5f;
	//			api.addComponent(e, tf);

	//			engine::Sprite sp{};
	//			sp.texture = charTex;
	//			sp.srcRect = { 0.f, 0.f, 32.f, 48.f };
	//			sp.layer = 2;
	//			sp.pass = engine::RenderPass::World;
	//			api.addComponent(e, sp);

	//			const TintPalette& p = kTintPalettes[i];
	//			engine::Tinting tnt{};
	//			tnt.slots[1] = { true, p.skin };
	//			tnt.slots[2] = { true, p.hair };
	//			tnt.slots[3] = { true, p.shirt };
	//			tnt.slots[4] = { true, p.pants };
	//			tnt.slots[5] = { true, p.shoes };
	//			api.addComponent(e, tnt);
	//			tintDemoEntities.push_back(e);
	//		}
	//	} else {
	//		SDL_Log("[main] failed to load assets/sprites/char.png — Tinting demo skipped");
	//	}
	//}

	// ── Player (带动画) ───────────────────────────────────────────────────────
	entt::entity player;
	{
		// Prefab smoke test:
		// createPlayer 是“代码工厂式预制体”的最小样例。它把玩家最常见的
		// Transform/Sprite/TextComponent 组合集中到一个 C++ 函数里，game 层
		// 只传入当前场景关心的坐标、纹理、字体和颜色。
		engine::prefabs::PlayerPrefabDesc desc{};
		desc.x = 640.f;
		desc.y = 300.f;
		desc.texture = spriteTex;
		desc.srcW = 32.f;
		desc.srcH = 32.f;
		desc.font = font;
		desc.label = "Prefab Player";
		desc.labelFontSize = 20.f;
		desc.spriteLayer = 3;
		desc.textLayer = 20;
		desc.spriteTint = { 255, 255, 100, 255 };
		desc.textColor = { 255, 230, 120, 255 };
		player = engine::prefabs::createPlayer(api, desc);

		// 这段输出就是 game 侧测试：如果后续有人改坏 prefab 组成，
		// 启动 game 时能第一时间看到哪个基础组件缺了。
		const bool prefabOk =
			api.hasComponent<engine::Transform>(player) &&
			api.hasComponent<engine::Sprite>(player) &&
			api.hasComponent<engine::TextComponent>(player);
		printf("[PrefabTest] createPlayer components: %s\n", prefabOk ? "OK" : "FAILED");

		engine::AnimatorComponent anim{};
		anim.currentAnim = playerAnim;
		anim.playing = false;
		anim.applyTexture = true;
		api.addComponent(player, anim);
	}

	// ── Phase 3 测试: FSM 驱动的 hero ──────────────────────────────────────
	entt::entity fsmHero;
	{
		fsmHero = api.spawnEntity();
		api.addComponent(fsmHero, engine::Transform{ 200.f, 400.f });
		engine::Sprite sp{};
		sp.texture = spriteTex;
		sp.srcRect = { 0.f, 0.f, 32.f, 32.f };
		sp.layer = 5;
		sp.pass = engine::RenderPass::World;
		sp.tint = { 120, 220, 255, 255 };
		api.addComponent(fsmHero, sp);

		auto ctrl = std::make_shared<engine::AnimatorController>();
		ctrl->states = {
			{ "Idle",   playerAnim, 0.5f, engine::PlayMode::Loop },
			{ "Walk",   playerAnim, 1.5f, engine::PlayMode::Loop },
			{ "Attack", attackAnim, 1.0f, engine::PlayMode::Once },
		};
		ctrl->transitions = {
			{ 0, 1, { { "speed",  engine::ConditionOp::Greater, 0.1f } }, false, 1.f, 0.05f, true },
			{ 1, 0, { { "speed",  engine::ConditionOp::Less,    0.1f } }, false, 1.f, 0.05f, true },
			{ engine::kAnyState, 2, { { "attack", engine::ConditionOp::Trigger, 0.f } }, false, 1.f, 0.f, true },
			{ 2, 0, {}, /*hasExitTime*/ true, 1.f, 0.f, true },
		};
		ctrl->defaultState = 0;

		engine::AnimatorComponent ac{};
		ac.applyTexture = true;
		ac.controller = ctrl;
		api.addComponent(fsmHero, ac);
	}

	// ═══════════════════════════════════════════════════════════════════════════
	// Phase 4 测试: 多层动画 (Layered Animation)
	// ═══════════════════════════════════════════════════════════════════════════
	// 实体说明：测试额外层如何覆盖/混合基础层的 SrcRect/Texture
	entt::entity layeredHero;
	{
		layeredHero = api.spawnEntity();
		api.addComponent(layeredHero, engine::Transform{ 500.f, 400.f });
		engine::Sprite sp{};
		sp.texture = spriteTex;
		sp.srcRect = { 0.f, 0.f, 32.f, 32.f };
		sp.layer = 5;
		sp.pass = engine::RenderPass::World;
		sp.tint = { 255, 180, 100, 255 };
		api.addComponent(layeredHero, sp);

		auto ctrl = std::make_shared<engine::AnimatorController>();
		// 基础层状态
		ctrl->states = {
			{ "Idle",   playerAnim, 0.8f, engine::PlayMode::Loop },
		};
		ctrl->transitions = {};
		ctrl->defaultState = 0;

		// 额外层 0: 使用不同的动画和混合模式 (Override)
		engine::AnimatorLayer layer0;
		layer0.name = "Overlay";
		layer0.weight = 0.7f;
		layer0.blendMode = engine::LayerBlendMode::Override;
		layer0.mask = engine::LayerChannel::SrcRect;
		layer0.states = {
			{ "OverlayIdle", attackAnim, 1.2f, engine::PlayMode::Loop },
		};
		layer0.transitions = {};
		layer0.defaultState = 0;

		// 额外层 1: 使用 Additive 混合
		engine::AnimatorLayer layer1;
		layer1.name = "AdditiveLayer";
		layer1.weight = 0.5f;
		layer1.blendMode = engine::LayerBlendMode::Override;
		layer1.mask = engine::LayerChannel::SrcRect;
		layer1.states = {
			{ "AddIdle", playerAnim, 2.0f, engine::PlayMode::Loop },
		};
		layer1.transitions = {};
		layer1.defaultState = 0;

		ctrl->layers = { layer0, layer1 };

		engine::AnimatorComponent ac{};
		ac.applyTexture = true;
		ac.controller = ctrl;
		api.addComponent(layeredHero, ac);
	}

	// ═══════════════════════════════════════════════════════════════════════════
	// Phase 5.3 测试: 程序化动画层 (Procedural Animation Layers)
	// ═══════════════════════════════════════════════════════════════════════════

	// Phase 5.3a: HitShake - 受击抖动
	entt::entity hitShakeTest;
	{
		hitShakeTest = api.spawnEntity();
		api.addComponent(hitShakeTest, engine::Transform{ 700.f, 300.f });
		engine::Sprite sp{};
		sp.texture = spriteTex;
		sp.srcRect = { 0.f, 0.f, 32.f, 32.f };
		sp.layer = 5;
		sp.pass = engine::RenderPass::World;
		sp.tint = { 255, 100, 100, 255 };
		api.addComponent(hitShakeTest, sp);

		auto ctrl = std::make_shared<engine::AnimatorController>();
		ctrl->states = {
			{ "Idle", playerAnim, 0.5f, engine::PlayMode::Loop },
		};
		ctrl->defaultState = 0;

		engine::AnimatorLayer shakeLayer;
		shakeLayer.name = "HitShake";
		shakeLayer.kind = engine::ProceduralKind::HitShake;
		shakeLayer.weight = 1.f;
		shakeLayer.blendMode = engine::LayerBlendMode::Additive;
		shakeLayer.procedural.triggerParam = "hit";
		shakeLayer.procedural.amplitude = 8.f;
		shakeLayer.procedural.frequency = 20.f;
		shakeLayer.procedural.duration = 0.4f;

		ctrl->layers = { shakeLayer };

		engine::AnimatorComponent ac{};
		ac.applyTexture = true;
		ac.controller = ctrl;
		api.addComponent(hitShakeTest, ac);
	}

	// Phase 5.3b: HurtFlash - 受击红闪
	entt::entity hurtFlashTest;
	{
		hurtFlashTest = api.spawnEntity();
		api.addComponent(hurtFlashTest, engine::Transform{ 800.f, 300.f });
		engine::Sprite sp{};
		sp.texture = spriteTex;
		sp.srcRect = { 0.f, 0.f, 32.f, 32.f };
		sp.layer = 5;
		sp.pass = engine::RenderPass::World;
		sp.tint = { 100, 200, 255, 255 };
		api.addComponent(hurtFlashTest, sp);

		auto ctrl = std::make_shared<engine::AnimatorController>();
		ctrl->states = {
			{ "Idle", playerAnim, 0.6f, engine::PlayMode::Loop },
		};
		ctrl->defaultState = 0;

		engine::AnimatorLayer flashLayer;
		flashLayer.name = "HurtFlash";
		flashLayer.kind = engine::ProceduralKind::HurtFlash;
		flashLayer.weight = 1.f;
		flashLayer.blendMode = engine::LayerBlendMode::Additive;
		flashLayer.procedural.triggerParam = "hurt";
		flashLayer.procedural.amplitude = 0.8f;
		flashLayer.procedural.duration = 0.25f;

		ctrl->layers = { flashLayer };

		engine::AnimatorComponent ac{};
		ac.applyTexture = true;
		ac.controller = ctrl;
		api.addComponent(hurtFlashTest, ac);
	}

	// Phase 5.3c: BreatheBob - 呼吸抖动 (持续效果)
	entt::entity breatheBobTest;
	{
		breatheBobTest = api.spawnEntity();
		api.addComponent(breatheBobTest, engine::Transform{ 900.f, 300.f });
		engine::Sprite sp{};
		sp.texture = spriteTex;
		sp.srcRect = { 0.f, 0.f, 32.f, 32.f };
		sp.layer = 5;
		sp.pass = engine::RenderPass::World;
		sp.tint = { 150, 255, 150, 255 };
		api.addComponent(breatheBobTest, sp);

		auto ctrl = std::make_shared<engine::AnimatorController>();
		ctrl->states = {
			{ "Idle", playerAnim, 0.4f, engine::PlayMode::Loop },
		};
		ctrl->defaultState = 0;

		engine::AnimatorLayer bobLayer;
		bobLayer.name = "BreatheBob";
		bobLayer.kind = engine::ProceduralKind::BreatheBob;
		bobLayer.weight = 1.f;
		bobLayer.blendMode = engine::LayerBlendMode::Additive;
		bobLayer.procedural.amplitude = 3.f;
		bobLayer.procedural.frequency = 0.5f;
		bobLayer.procedural.strengthParam = "breathStrength";

		ctrl->layers = { bobLayer };

		engine::AnimatorComponent ac{};
		ac.applyTexture = true;
		ac.controller = ctrl;
		ac.setFloat("breathStrength", 1.f);
		api.addComponent(breatheBobTest, ac);
	}

	// Phase 5.3d: SquashStretchOnLand - 落地挤压
	entt::entity squashTest;
	{
		squashTest = api.spawnEntity();
		api.addComponent(squashTest, engine::Transform{ 1000.f, 300.f });
		engine::Sprite sp{};
		sp.texture = spriteTex;
		sp.srcRect = { 0.f, 0.f, 32.f, 32.f };
		sp.layer = 5;
		sp.pass = engine::RenderPass::World;
		sp.tint = { 255, 200, 150, 255 };
		api.addComponent(squashTest, sp);

		auto ctrl = std::make_shared<engine::AnimatorController>();
		ctrl->states = {
			{ "Idle", playerAnim, 0.5f, engine::PlayMode::Loop },
		};
		ctrl->defaultState = 0;

		engine::AnimatorLayer squashLayer;
		squashLayer.name = "SquashStretch";
		squashLayer.kind = engine::ProceduralKind::SquashStretchOnLand;
		squashLayer.weight = 1.f;
		squashLayer.blendMode = engine::LayerBlendMode::Override;
		squashLayer.procedural.triggerParam = "land";
		squashLayer.procedural.amplitude = 0.3f;
		squashLayer.procedural.duration = 0.3f;

		ctrl->layers = { squashLayer };

		engine::AnimatorComponent ac{};
		ac.applyTexture = true;
		ac.controller = ctrl;
		api.addComponent(squashTest, ac);
	}

	// Phase 5.3e: 综合测试实体 - 多程序化层叠加
	entt::entity proceduralComboTest;
	{
		proceduralComboTest = api.spawnEntity();
		api.addComponent(proceduralComboTest, engine::Transform{ 1100.f, 300.f });
		engine::Sprite sp{};
		sp.texture = spriteTex;
		sp.srcRect = { 0.f, 0.f, 32.f, 32.f };
		sp.layer = 5;
		sp.pass = engine::RenderPass::World;
		sp.tint = { 200, 180, 255, 255 };
		api.addComponent(proceduralComboTest, sp);

		auto ctrl = std::make_shared<engine::AnimatorController>();
		ctrl->states = {
			{ "Idle", playerAnim, 0.5f, engine::PlayMode::Loop },
		};
		ctrl->defaultState = 0;

		// 层 0: 持续呼吸
		engine::AnimatorLayer bobLayer;
		bobLayer.name = "Breathe";
		bobLayer.kind = engine::ProceduralKind::BreatheBob;
		bobLayer.weight = 0.6f;
		bobLayer.blendMode = engine::LayerBlendMode::Additive;
		bobLayer.procedural.amplitude = 2.f;
		bobLayer.procedural.frequency = 0.8f;

		// 层 1: 受击抖动
		engine::AnimatorLayer shakeLayer;
		shakeLayer.name = "HitShake";
		shakeLayer.kind = engine::ProceduralKind::HitShake;
		shakeLayer.weight = 1.f;
		shakeLayer.blendMode = engine::LayerBlendMode::Additive;
		shakeLayer.procedural.triggerParam = "comboHit";
		shakeLayer.procedural.amplitude = 5.f;
		shakeLayer.procedural.frequency = 15.f;
		shakeLayer.procedural.duration = 0.3f;

		// 层 2: 受击红闪
		engine::AnimatorLayer flashLayer;
		flashLayer.name = "HurtFlash";
		flashLayer.kind = engine::ProceduralKind::HurtFlash;
		flashLayer.weight = 0.8f;
		flashLayer.blendMode = engine::LayerBlendMode::Additive;
		flashLayer.procedural.triggerParam = "comboHurt";
		flashLayer.procedural.amplitude = 0.5f;
		flashLayer.procedural.duration = 0.2f;

		ctrl->layers = { bobLayer, shakeLayer, flashLayer };

		engine::AnimatorComponent ac{};
		ac.applyTexture = true;
		ac.controller = ctrl;
		api.addComponent(proceduralComboTest, ac);
	}

	// ── 状态文字 (World 层) ───────────────────────────────────────────────────
	entt::entity statusText;
	{
		statusText = api.spawnEntity();
		api.addComponent(statusText, engine::Transform{ 640.f, 250.f });
		engine::Sprite sp{};
		sp.texture = stoppedTex;
		sp.srcRect = { 0.f, 0.f, 56.f, 16.f };
		sp.layer = 10;
		sp.pass = engine::RenderPass::World;
		sp.ySort = false;
		api.addComponent(statusText, sp);
	}

	// ── MSDF 文字渲染 (World 层) ─────────────────────────────────────────────
	{
		auto e = api.spawnEntity();
		api.addComponent(e, engine::Transform{ 40.f, 80.f });
		engine::TextComponent txt{};
		txt.text = "WASD: player move, up down left right: camera move";
		txt.font = font;
		txt.fontSize = 48.f;
		txt.color = { 255, 255, 255, 255 };
		txt.pass = engine::RenderPass::World;
		api.addComponent(e, txt);
	}

	// ── UI 标题 ───────────────────────────────────────────────────────────────
	{
		auto e = api.spawnEntity();
		api.addComponent(e, engine::Transform{ 20.f, 20.f });
		engine::TextComponent txt{};
		txt.text = "Two Camera Demo";
		txt.font = font;
		txt.fontSize = 32.f;
		txt.color = { 255, 255, 100, 255 };
		txt.pass = engine::RenderPass::UI;
		txt.layer = 100;
		api.addComponent(e, txt);
	}

	// ── UI 说明 ───────────────────────────────────────────────────────────────
	{
		auto e = api.spawnEntity();
		api.addComponent(e, engine::Transform{ 20.f, 60.f });
		engine::TextComponent txt{};
		txt.text = "World: zoom=1.5x | UI: zoom=1.0x";
		txt.font = font;
		txt.fontSize = 20.f;
		txt.color = { 200, 200, 200, 255 };
		txt.pass = engine::RenderPass::UI;
		txt.layer = 100;
		api.addComponent(e, txt);
	}

	// ── Screen 提示 ──────────────────────────────────────────────────────────
	{
		auto e = api.spawnEntity();
		api.addComponent(e, engine::Transform{ 20.f, 620.f });
		engine::TextComponent txt{};
		txt.text = "Physics: R=Raycast T=OverlapBox Y=OverlapCircle Space=ResetBox P=PrintLayers";
		txt.font = font;
		txt.fontSize = 13.f;
		txt.color = { 100, 200, 150, 255 };
		txt.pass = engine::RenderPass::Screen;
		txt.layer = 100;
		api.addComponent(e, txt);
	}
	{
		auto e = api.spawnEntity();
		api.addComponent(e, engine::Transform{ 20.f, 640.f });
		engine::TextComponent txt{};
		txt.text = "WASD: move | Arrows: camera | G: GPU/CPU | J/K/L/U: Phase1 | F: Phase3 | 1-7: Phase5";
		txt.font = font;
		txt.fontSize = 12.f;
		txt.color = { 150, 150, 150, 255 };
		txt.pass = engine::RenderPass::Screen;
		txt.layer = 100;
		api.addComponent(e, txt);
	}
	{
		auto e = api.spawnEntity();
		api.addComponent(e, engine::Transform{ 20.f, 658.f });
		engine::TextComponent txt{};
		txt.text = "Phase5: 1=HitShake 2=HurtFlash 3=BreathStr 4=Squash 5=Combo 6/7:TimeScale | ESC: quit";
		txt.font = font;
		txt.fontSize = 11.f;
		txt.color = { 120, 120, 150, 255 };
		txt.pass = engine::RenderPass::Screen;
		txt.layer = 100;
		api.addComponent(e, txt);
	}
	{
		auto e = api.spawnEntity();
		api.addComponent(e, engine::Transform{ 20.f, 674.f });
		engine::TextComponent txt{};
		txt.text = "GPU: O(n) culling+sort | CPU: O(n log n) | Orange box falls with gravity, yellow bullet passes through ground";
		txt.font = font;
		txt.fontSize = 10.f;
		txt.color = { 100, 100, 130, 255 };
		txt.pass = engine::RenderPass::Screen;
		txt.layer = 100;
		api.addComponent(e, txt);
	}

	// ═══════════════════════════════════════════════════════════════════════════
	// M1/M2 测试：生成大量精灵测试 GPU culling/sorting
	// GPU-Driven 2D Rendering 架构验证
	// ═══════════════════════════════════════════════════════════════════════════
	// 
	// 测试说明：
	// - 按 G 键切换 CPU/GPU 渲染模式
	// - GPU 模式优势：
	//   1. CPU 时间降低 10-50x
	//   2. Draw Calls 降低 10-100x
	//   3. 支持 10K+ 精灵稳定渲染
	// - CPU 模式：传统遍历+排序+批处理
	// 
	// 架构演进：
	// Phase 1: GPU-Driven 2D Rendering (当前) - GPU 剔除+排序
	// Phase 2: Reactive Render Graph - 状态变化驱动渲染
	// Phase 3: Streaming Tile World - 无限地图流式加载
	// ═══════════════════════════════════════════════════════════════════════════

	constexpr bool CREATE_MANY_SPRITES = true;
	constexpr int SPRITE_GRID_SIZE = 50;  // 30x30 = 900 sprites (可调大到 50x50=2500 测试性能)

	TextureHandle smallTex;  // 用于性能测试的纹理

	if (CREATE_MANY_SPRITES) {
		printf("\n");
		printf("+----------------------------------------------------------------+\n");
		printf("|         GPU-Driven 2D Rendering Architecture Test              |\n");
		printf("+----------------------------------------------------------------+\n");
		printf("|  Creating %4d sprites in a grid for stress testing...        |\n", SPRITE_GRID_SIZE * SPRITE_GRID_SIZE);
		printf("+----------------------------------------------------------------+\n");

		auto smallPx = makeCheckerboard(16, 16, 4, { 200, 200, 100, 255 }, { 100, 100, 200, 255 });
		smallTex = api.createTextureFromMemory(smallPx.data(), 16, 16);

		float startX = -800.f;
		float startY = -400.f;
		float spacing = 40.f;

		// 创建网格精灵
		for (int gy = 0; gy < SPRITE_GRID_SIZE; ++gy) {
			for (int gx = 0; gx < SPRITE_GRID_SIZE; ++gx) {
				auto e = api.spawnEntity();
				float x = startX + gx * spacing;
				float y = startY + gy * spacing;
				api.addComponent(e, engine::Transform{ x, y });

				engine::Sprite sp{};
				sp.texture = smallTex;
				sp.srcRect = { 0.f, 0.f, 16.f, 16.f };
				sp.layer = (gx + gy) % 5;
				sp.pass = engine::RenderPass::World;
				sp.ySort = true;
				sp.tint = core::Color{
					static_cast<uint8_t>(100 + gx * 5),
					static_cast<uint8_t>(100 + gy * 5),
					static_cast<uint8_t>(150 + (gx + gy) * 2),
					255
				};
				api.addComponent(e, sp);
			}
		}

		printf("\n");
		printf("+-----------------------------------------------------------------+\n");
		printf("|  GPU-Driven Rendering - Controls                               |\n");
		printf("+-----------------------------------------------------------------+\n");
		printf("|  G       : Toggle GPU/CPU rendering mode                       |\n");
		printf("|  Arrows  : Move camera (view sprites outside viewport)         |\n");
		printf("|  WASD    : Move player sprite                                  |\n");
		printf("|  ESC     : Quit                                                |\n");
		printf("+-----------------------------------------------------------------+\n");
		printf("|  Current: %d sprites created                              |\n", SPRITE_GRID_SIZE * SPRITE_GRID_SIZE);
		printf("|  Tip: Move camera to see GPU culling in action!                |\n");
		printf("+-----------------------------------------------------------------+\n\n");
	}

	// ── GPU-driven 状态与性能显示 ─────────────────────────────────────────────
	entt::entity gpuStatusText;
	{
		gpuStatusText = api.spawnEntity();
		api.addComponent(gpuStatusText, engine::Transform{ 20.f, 100.f });
		engine::TextComponent txt{};
		txt.text = (cfg.renderBackend == engine::RenderBackend::OpenGL ? "[Render Mode] CPU" : "[Render Mode] GPU");
		txt.font = font;
		txt.fontSize = 18.f;
		txt.color = { 255, 200, 100, 255 };
		txt.pass = engine::RenderPass::UI;
		txt.layer = 100;
		api.addComponent(gpuStatusText, txt);
	}

	// ── 性能统计显示 ─────────────────────────────────────────────────────────
	entt::entity perfText;
	{
		perfText = api.spawnEntity();
		api.addComponent(perfText, engine::Transform{ 20.f, 125.f });
		engine::TextComponent txt{};
		txt.text = "FPS: -- | Sprites: 0 | Visible: 0 | Draw Calls: --";
		txt.font = font;
		txt.fontSize = 14.f;
		txt.color = { 180, 180, 180, 255 };
		txt.pass = engine::RenderPass::UI;
		txt.layer = 100;
		api.addComponent(perfText, txt);
	}

	// ── GPU 架构说明 ─────────────────────────────────────────────────────────
	entt::entity archText;
	{
		archText = api.spawnEntity();
		api.addComponent(archText, engine::Transform{ 20.f, 145.f });
		engine::TextComponent txt{};
		txt.text = "Architecture: CPU-Driven (Traditional)";
		txt.font = font;
		txt.fontSize = 12.f;
		txt.color = { 120, 120, 150, 255 };
		txt.pass = engine::RenderPass::UI;
		txt.layer = 100;
		api.addComponent(archText, txt);
	}

	// ═══════════════════════════════════════════════════════════════════════════
	// 物理测试：重力 + 碰撞 + 碰撞层 + 射线检测 + 区域查询
	// ═══════════════════════════════════════════════════════════════════════════
	api.setGravity(0.f, 500.f);  // 向下重力
	api.setFixedTimestep(1.f / 60.f);  // 60Hz 物理更新

	// 自定义碰撞层（扩展预定义层）
	constexpr engine::CollisionLayer COLLISION_LAYER_BULLET = 16;  // 第5层
	constexpr engine::CollisionLayer COLLISION_LAYER_PICKUP = 32;  // 第6层

	// 地面 (static collider, STATIC 层，只与 DEFAULT/PLAYER/ENEMY 碰撞)
	{
		auto e = api.spawnEntity();
		api.addComponent(e, engine::Transform{ 0.f, 550.f });
		engine::Sprite sp{};
		sp.texture = tilesetTex;
		sp.srcRect = { 0.f, 0.f, 640.f, 32.f };
		sp.layer = 0;
		sp.pass = engine::RenderPass::World;
		sp.tint = { 100, 100, 100, 255 };
		api.addComponent(e, sp);
		engine::Collider col{ 1280.f, 32.f, 0.f, 0.f, false };
		col.layer = engine::COLLISION_LAYER_STATIC;
		col.mask = engine::COLLISION_LAYER_DEFAULT | engine::COLLISION_LAYER_PLAYER | engine::COLLISION_LAYER_ENEMY;
		api.addComponent(e, col);
	}

	// 下落的物理方块 (PLAYER 层)
	entt::entity physicsBox;
	{
		physicsBox = api.spawnEntity();
		api.addComponent(physicsBox, engine::Transform{ 600.f, 50.f });
		engine::Sprite sp{};
		sp.texture = spriteTex;
		sp.srcRect = { 0.f, 0.f, 32.f, 32.f };
		sp.layer = 10;
		sp.pass = engine::RenderPass::World;
		sp.tint = { 255, 150, 50, 255 };
		api.addComponent(physicsBox, sp);
		api.addComponent(physicsBox, engine::RigidBody{ 0.f, 0.f, 1.f, false });
		engine::Collider col{ 32.f, 32.f, 0.f, 0.f, false };
		col.layer = engine::COLLISION_LAYER_PLAYER;
		col.mask = engine::COLLISION_LAYER_ALL;
		api.addComponent(physicsBox, col);
	}

	// 子弹实体 (BULLET 层，只与 ENEMY 碰撞，穿过地面和玩家)
	entt::entity bullet;
	{
		bullet = api.spawnEntity();
		api.addComponent(bullet, engine::Transform{ 700.f, 200.f });
		engine::Sprite sp{};
		sp.texture = spriteTex;
		sp.srcRect = { 32.f, 0.f, 16.f, 16.f };
		sp.layer = 11;
		sp.pass = engine::RenderPass::World;
		sp.tint = { 255, 255, 0, 255 };
		api.addComponent(bullet, sp);
		api.addComponent(bullet, engine::RigidBody{ 50.f, 0.f, 0.f, false });  // 向右飞
		engine::Collider col{ 16.f, 16.f, 0.f, 0.f, false };
		col.layer = COLLISION_LAYER_BULLET;
		col.mask = engine::COLLISION_LAYER_ENEMY;  // 只碰敌人
		api.addComponent(bullet, col);
	}

	// 敌人实体 (ENEMY 层)
	entt::entity enemy;
	{
		enemy = api.spawnEntity();
		api.addComponent(enemy, engine::Transform{ 900.f, 500.f });
		engine::Sprite sp{};
		sp.texture = spriteTex;
		sp.srcRect = { 0.f, 0.f, 32.f, 32.f };
		sp.layer = 10;
		sp.pass = engine::RenderPass::World;
		sp.tint = { 255, 50, 50, 255 };
		api.addComponent(enemy, sp);
		engine::Collider col{ 32.f, 32.f, 0.f, 0.f, false };
		col.layer = engine::COLLISION_LAYER_ENEMY;
		col.mask = engine::COLLISION_LAYER_ALL;
		api.addComponent(enemy, col);
	}

	// Trigger 区域 (拾取物，不参与物理分离)
	entt::entity pickupZone;
	{
		pickupZone = api.spawnEntity();
		api.addComponent(pickupZone, engine::Transform{ 400.f, 480.f });
		engine::Sprite sp{};
		sp.texture = spriteTex;
		sp.srcRect = { 0.f, 32.f, 48.f, 48.f };
		sp.layer = 1;
		sp.pass = engine::RenderPass::World;
		sp.tint = { 100, 255, 100, 150 };
		api.addComponent(pickupZone, sp);
		engine::Collider col{ 48.f, 48.f, 0.f, 0.f, true };  // isTrigger=true
		col.layer = COLLISION_LAYER_PICKUP;
		col.mask = engine::COLLISION_LAYER_PLAYER;
		api.addComponent(pickupZone, col);
	}

	// 射线检测结果显示实体
	entt::entity rayHitMarker;
	{
		rayHitMarker = api.spawnEntity();
		api.addComponent(rayHitMarker, engine::Transform{ 0.f, 0.f });
		engine::Sprite sp{};
		sp.texture = spriteTex;
		sp.srcRect = { 48.f, 48.f, 8.f, 8.f };
		sp.layer = 100;
		sp.pass = engine::RenderPass::World;
		sp.tint = { 255, 255, 0, 255 };
		api.addComponent(rayHitMarker, sp);
	}

	// ═══════════════════════════════════════════════════════════════════════════
	// UI System v2 测试
	// ═══════════════════════════════════════════════════════════════════════════
	printf("\n");
	printf("+----------------------------------------------------------------+\n");
	printf("|                  UI System v2 Test                             |\n");
	printf("+----------------------------------------------------------------+\n");
	printf("|  - Click button to trigger callback                            |\n");
	printf("|  - Drag slider to change volume                                |\n");
	printf("|  - Toggle the switch                                           |\n");
	printf("|  - Watch progress bar animate                                  |\n");
	printf("|  - Drag the red square anywhere                                |\n");
	printf("|  - Player entity has a world-anchored health bar               |\n");
	printf("+----------------------------------------------------------------+\n\n");

	auto canvas = api.createCanvas(1280, 720);

	static int clickCount = 0;
	static float volume = 0.5f;
	static bool soundEnabled = true;
	static float progressValue = 0.f;
	static float playerHealth = 1.f;  // 0..1，每帧脉动

	// ── 标题 ─────────────────────────────────────────────────────────────────────
	{
		auto title = api.createUIText(400.f, 40.f, "UI System v2 Demo");
		api.setUIParent(title, canvas);
		api.setUIAnchor(title, 0.5f, 0.f, 0.5f, 0.f);
		api.setUIOffset(title, 0.f, 30.f);
		api.setUITextFont(title, font, 28.f);
		api.setUITextColor(title, { 255, 220, 100, 255 });
	}

	// ── 按钮 + 文本 (右上角) ────────────────────────────────────────────────────
	entt::entity testButton;
	entt::entity buttonLabel;
	{
		testButton = api.createButton(200.f, 50.f, []() {
			clickCount++;
			printf("[UI] Button clicked! Count: %d\n", clickCount);
			});
		api.setUIParent(testButton, canvas);
		api.setUIAnchor(testButton, 1.f, 0.f, 1.f, 0.f);
		api.setUIPivot(testButton, 1.f, 0.f);
		api.setUIOffset(testButton, -20.f, 20.f);
		api.setButtonColors(testButton,
			{ 80, 120, 180, 255 },
			{ 100, 150, 210, 255 },
			{ 60,  90, 140, 255 });

		buttonLabel = api.createUIText(200.f, 50.f, "Click Me!");
		api.setUIParent(buttonLabel, testButton);
		api.setUIAnchor(buttonLabel, 0.f, 0.f, 1.f, 1.f);   // 撑满父按钮
		api.setUIOffset(buttonLabel, 0.f, 0.f);
		api.setUITextFont(buttonLabel, font, 20.f);
		api.setUITextColor(buttonLabel, { 255, 255, 255, 255 });
		api.setUISortOrder(buttonLabel, 1);
	}

	// ── 音量滑动条 + 标签 ───────────────────────────────────────────────────────
	entt::entity volumeSlider;
	entt::entity volumeLabel;
	{
		volumeSlider = api.createSlider(200.f, 24.f, 0.f, 100.f, [](float v) {
			volume = v / 100.f;
			});
		api.setUIParent(volumeSlider, canvas);
		api.setUIAnchor(volumeSlider, 1.f, 0.f, 1.f, 0.f);
		api.setUIPivot(volumeSlider, 1.f, 0.f);
		api.setUIOffset(volumeSlider, -20.f, 100.f);
		api.setSliderValue(volumeSlider, 50.f);

		volumeLabel = api.createUIText(200.f, 22.f, "Volume: 50%");
		api.setUIParent(volumeLabel, canvas);
		api.setUIAnchor(volumeLabel, 1.f, 0.f, 1.f, 0.f);
		api.setUIPivot(volumeLabel, 1.f, 0.f);
		api.setUIOffset(volumeLabel, -20.f, 130.f);
		api.setUITextFont(volumeLabel, font, 14.f);
		api.setUITextColor(volumeLabel, { 200, 200, 200, 255 });
	}

	// ── 音效开关 + 标签 ─────────────────────────────────────────────────────────
	entt::entity soundToggle;
	entt::entity soundLabel;
	{
		soundToggle = api.createToggle(60.f, 30.f, [](bool on) {
			soundEnabled = on;
			});
		api.setUIParent(soundToggle, canvas);
		api.setUIAnchor(soundToggle, 1.f, 0.f, 1.f, 0.f);
		api.setUIPivot(soundToggle, 1.f, 0.f);
		api.setUIOffset(soundToggle, -20.f, 170.f);
		api.setToggleValue(soundToggle, true);

		soundLabel = api.createUIText(140.f, 30.f, "Sound: ON");
		api.setUIParent(soundLabel, canvas);
		api.setUIAnchor(soundLabel, 1.f, 0.f, 1.f, 0.f);
		api.setUIPivot(soundLabel, 1.f, 0.f);
		api.setUIOffset(soundLabel, -90.f, 170.f);
		api.setUITextFont(soundLabel, font, 16.f);
		api.setUITextColor(soundLabel, { 200, 200, 200, 255 });
		api.setUITextAlignment(soundLabel, 2);  // right align
	}

	// ── 进度条 + 标签 (居中下方) ────────────────────────────────────────────────
	entt::entity progressBar;
	{
		auto progressLabel = api.createUIText(220.f, 22.f, "Loading...");
		api.setUIParent(progressLabel, canvas);
		api.setUIAnchor(progressLabel, 0.5f, 0.5f, 0.5f, 0.5f);
		api.setUIPivot(progressLabel, 0.5f, 1.f);
		api.setUIOffset(progressLabel, 0.f, 50.f);
		api.setUITextFont(progressLabel, font, 14.f);
		api.setUITextColor(progressLabel, { 200, 200, 200, 255 });

		progressBar = api.createProgressBar(220.f, 22.f);
		api.setUIParent(progressBar, canvas);
		api.setUIAnchor(progressBar, 0.5f, 0.5f, 0.5f, 0.5f);
		api.setUIPivot(progressBar, 0.5f, 0.f);
		api.setUIOffset(progressBar, 0.f, 60.f);
		api.setProgressColors(progressBar, { 50, 50, 50, 255 }, { 100, 200, 100, 255 });
	}

	// ── 可拖拽方块 (左侧) ───────────────────────────────────────────────────────
	{
		auto dragHint = api.createUIText(120.f, 20.f, "Drag the square ↓");
		api.setUIParent(dragHint, canvas);
		api.setUIAnchor(dragHint, 0.f, 0.5f, 0.f, 0.5f);
		api.setUIOffset(dragHint, 40.f, -50.f);
		api.setUITextFont(dragHint, font, 12.f);
		api.setUITextColor(dragHint, { 200, 200, 200, 255 });

		auto draggable = api.createUIImage(80.f, 80.f);
		api.setUIParent(draggable, canvas);
		// makeDraggable 会强制 anchor=topLeft + pivot=(0,0)；offset 即屏幕坐标
		api.makeDraggable(draggable, [](float x, float y) {
			(void)x; (void)y;
			});
		api.setUIOffset(draggable, 40.f, 360.f);
		api.setUIImageColor(draggable, { 200, 80, 80, 255 });
		api.setDragBounds(draggable, 0.f, 0.f, 1280.f, 720.f);
	}

	// ── DropTarget 测试 ─────────────────────────────────────────────────────────
	// 两个投放槽：左槽接受 payload="item"，右槽用 canAccept 拒绝同一物品。
	// 一个带 payload="item" 的小拖拽块，未命中槽位时自动回弹到起点。
	{
		auto dropHint = api.createUIText(280.f, 20.f, "DropTarget: drag GEM into a slot");
		api.setUIParent(dropHint, canvas);
		api.setUIAnchor(dropHint, 0.5f, 0.f, 0.5f, 0.f);
		api.setUIPivot(dropHint, 0.5f, 0.f);
		api.setUIOffset(dropHint, 0.f, 90.f);
		api.setUITextFont(dropHint, font, 13.f);
		api.setUITextColor(dropHint, { 200, 200, 200, 255 });

		auto makeSlot = [&](float ox, const char* label,
		                    bool acceptItem, bool useReject) {
			auto slot = api.createUIElement();
			api.setUIParent(slot, canvas);
			api.setUISize(slot, 96.f, 96.f);
			api.setUIAnchor(slot, 0.5f, 0.f, 0.5f, 0.f);
			api.setUIPivot(slot, 0.5f, 0.f);
			api.setUIOffset(slot, ox, 120.f);
			api.setUIInteractable(slot, true);
			api.setUIBackground(slot, { 40, 50, 60, 255 }, {});
			if (acceptItem) api.setDropAcceptedPayload(slot, "item");
			if (useReject) {
				api.setDropAcceptCallback(slot,
					[](entt::entity) { return false; });
			}
			api.makeDropTarget(slot, [label](entt::entity dragged) {
				printf("[Drop] %s received entity %u\n", label,
					(unsigned)entt::to_integral(dragged));
			});
			api.setDropEnterCallback(slot, [label](entt::entity) {
				printf("[Drop] enter %s\n", label);
			});
			api.setDropLeaveCallback(slot, [label](entt::entity) {
				printf("[Drop] leave %s\n", label);
			});
			api.setDropHoverHighlight(slot, true, { 255, 255, 120, 90 });

			auto tag = api.createUIText(96.f, 18.f, label);
			api.setUIParent(tag, slot);
			api.setUIAnchor(tag, 0.5f, 1.f, 0.5f, 1.f);
			api.setUIPivot(tag, 0.5f, 1.f);
			api.setUIOffset(tag, 0.f, -4.f);
			api.setUITextFont(tag, font, 11.f);
			api.setUITextColor(tag, { 220, 220, 220, 255 });
			api.setUIInteractable(tag, false);
			return slot;
		};
		makeSlot(-80.f, "ACCEPT(item)", true,  false);
		makeSlot( 30.f, "REJECT",       true,  true);

		// 可拖拽宝石。注意：makeDraggable 之后不要再改 anchor/pivot——drag-follow
		// 把 offset 当 topLeft 屏幕坐标存，改回 center 锚点会让视觉位置每帧偏移。
		auto gem = api.createUIImage(40.f, 40.f);
		api.setUIParent(gem, canvas);
		api.makeDraggable(gem, nullptr);
		api.setDragPayload(gem, "item");
		api.setDragSnapBack(gem, true);          // 没投中就回弹
		api.setUIOffset(gem, 750.f, 145.f);      // 1280/2 + 110, topLeft 屏幕坐标
		api.setUIImageColor(gem, { 80, 200, 220, 255 });
		api.setUISortOrder(gem, 100);            // 置顶，避免被槽底盖住
	}

	// ── ScrollView 测试（中下部）：垂直列表，滚轮 / 拖拽 / 内嵌按钮 ─────────────
	{
		auto scrollHint = api.createUIText(220.f, 20.f, "ScrollView (wheel / drag)");
		api.setUIParent(scrollHint, canvas);
		api.setUIAnchor(scrollHint, 0.f, 1.f, 0.f, 1.f);
		api.setUIPivot(scrollHint, 0.f, 1.f);
		api.setUIOffset(scrollHint, 40.f, -270.f);
		api.setUITextFont(scrollHint, font, 13.f);
		api.setUITextColor(scrollHint, { 200, 200, 200, 255 });

		// 视口 240x220
		auto scroll = api.createScrollView(240.f, 220.f, 0 /*Vertical*/);
		api.setUIParent(scroll, canvas);
		api.setUIAnchor(scroll, 0.f, 1.f, 0.f, 1.f);
		api.setUIPivot(scroll, 0.f, 1.f);
		api.setUIOffset(scroll, 40.f, -40.f);
		// 视口背景：贴一个深色背景便于观察裁剪
		api.setUIBackground(scroll, { 30, 30, 40, 255 }, {});

		// 在 ScrollView 内塞 20 个按钮
		const int kItems = 20;
		const float itemH = 36.f;
		const float pad = 6.f;
		for (int i = 0; i < kItems; ++i) {
			char label[64];
			std::snprintf(label, sizeof(label), "Item %02d", i + 1);

			auto btn = api.createButton(220.f, itemH, [i, label = std::string(label)]() {
				printf("[Scroll] clicked %s\n", label.c_str());
				});
			api.setUIParent(btn, scroll);
			// 子节点遵循"约定"：anchor=topLeft + pivot=(0,0)，offset 即视口内坐标
			api.setUIAnchor(btn, 0.f, 0.f, 0.f, 0.f);
			api.setUIPivot(btn, 0.f, 0.f);
			api.setUIOffset(btn, pad, pad + i * (itemH + pad));
			api.setUISize(btn, 220.f, itemH);
			api.setButtonColors(btn,
				{ uint8_t(70 + (i * 5) % 80), 90, 130, 255 },
				{ 110, 140, 180, 255 },
				{ 50, 70, 100, 255 });

			auto lbl = api.createUIText(220.f, itemH, label);
			api.setUIParent(lbl, btn);
			api.setUIAnchor(lbl, 0.f, 0.f, 1.f, 1.f);
			api.setUITextFont(lbl, font, 16.f);
			api.setUITextColor(lbl, { 240, 240, 240, 255 });
			api.setUISortOrder(lbl, 1);
		}
		// contentH 留 0 让 UISystem 自动测量

		// ── ScrollBar：贴在视口右侧 8px 厚的竖条 ──────────────────────────────
		// 与 ScrollView 同级（同 parent canvas），靠 anchor/offset 把它对齐到视口
		// 右边缘内侧。target=scroll 让滑块自动反映 scrollY/contentH。
		{
			auto sb = api.createScrollBar(scroll, /*Vertical*/0,
			                              /*thickness*/8.f, /*length*/220.f);
			api.setUIParent(sb, canvas);
			// 视口左下角对齐 (40, -40)，宽 240、高 220 → 右侧 = 40+240 = 280
			// 竖滚条贴在视口内侧右边缘：x=280-8=272, y 跟视口同
			api.setUIAnchor(sb, 0.f, 1.f, 0.f, 1.f);
			api.setUIPivot(sb, 0.f, 1.f);
			api.setUIOffset(sb, 40.f + 240.f, -40.f);
			api.setScrollBarColors(sb,
				{ 20, 20, 25, 200 },     // track
				{ 110, 130, 160, 220 },  // thumb
				{ 150, 170, 200, 240 },  // hover
				{ 200, 220, 240, 255 }); // pressed
			api.setScrollBarTrackPadding(sb, 2.f);
		}
	}

	// ── 世界空间血条 (跟随 player) ──────────────────────────────────────────────
	entt::entity playerHealthBar;
	{
		playerHealthBar = api.createProgressBar(60.f, 8.f);
		api.setUIParent(playerHealthBar, canvas);
		api.setUIPivot(playerHealthBar, 0.5f, 1.f);
		api.attachToWorld(playerHealthBar, player, 0.f, -32.f);   // 头顶上方
		api.setProgressColors(playerHealthBar, { 30, 30, 30, 220 }, { 220, 60, 60, 255 });

		auto playerName = api.createUIText(120.f, 18.f, "Player");
		api.setUIParent(playerName, canvas);
		api.setUIPivot(playerName, 0.5f, 1.f);
		api.attachToWorld(playerName, player, 0.f, -44.f);
		api.setUITextFont(playerName, font, 12.f);
		api.setUITextColor(playerName, { 255, 230, 180, 255 });
	}

	// ── LayoutGroup 测试（Horizontal / Vertical / Grid） ────────────────────────
	buildLayoutTest(api, canvas, font);

	// ── NineSlice 测试 ──────────────────────────────────────────────────────────
	{
		auto nsPx  = makeNineSliceTexture();
		auto nsTex = api.createTextureFromMemory(nsPx.data(), 32, 32);
		buildNineSliceTest(api, canvas, nsTex, font);
	}

	// ── Tooltip 测试 ────────────────────────────────────────────────────────────
	buildTooltipTest(api, canvas, font);

	// ── UIMask 测试 ─────────────────────────────────────────────────────────────
	entt::entity maskRunner    = entt::null;
	entt::entity maskPanel     = entt::null;
	entt::entity maskHintLabel = entt::null;
	buildMaskTest(api, canvas, font, maskRunner, maskPanel, maskHintLabel);
	(void)maskPanel; (void)maskHintLabel;

	// ── Modal 测试 ──────────────────────────────────────────────────────────────
	buildModalTest(api, canvas, font);
	buildTweenTest(api, canvas, font);

	// ── TextInput 测试 ──────────────────────────────────────────────────────────
	entt::entity textInput;
	entt::entity inputLabel;
	{
		// 标题
		auto tiTitle = api.createUIText(200.f, 24.f, "TextInput Demo");
		api.setUIParent(tiTitle, canvas);
		api.setUIAnchor(tiTitle, 0.f, 0.f, 0.f, 0.f);
		api.setUIOffset(tiTitle, 200.f, 280.f);
		api.setUITextFont(tiTitle, font, 18.f);
		api.setUITextColor(tiTitle, { 200, 220, 255, 255 });

		// 文本输入框
		textInput = api.createTextInput(280.f, 36.f);
		api.setUIParent(textInput, canvas);
		api.setUIAnchor(textInput, 0.f, 0.f, 0.f, 0.f);
		api.setUIOffset(textInput, 200.f, 310.f);
		api.setTextInputFont(textInput, font, 20.f);
		api.setTextInputPlaceholder(textInput, "Type here...");
		api.setTextInputMaxLength(textInput, 30);
		api.setTextInputCallback(textInput, [](const std::string& t) {
			printf("[TextInput] text changed: \"%s\"\n", t.c_str());
		});
		api.setTextInputSubmitCallback(textInput, [](const std::string& t) {
			printf("[TextInput] SUBMITTED: \"%s\"\n", t.c_str());
		});

		// 输入反馈标签
		inputLabel = api.createUIText(280.f, 20.f, "");
		api.setUIParent(inputLabel, canvas);
		api.setUIAnchor(inputLabel, 0.f, 0.f, 0.f, 0.f);
		api.setUIOffset(inputLabel, 200.f, 350.f);
		api.setUITextFont(inputLabel, font, 14.f);
		api.setUITextColor(inputLabel, { 150, 255, 150, 255 });

		// 密码模式测试
		auto pwTitle = api.createUIText(200.f, 24.f, "Password Mode");
		api.setUIParent(pwTitle, canvas);
		api.setUIAnchor(pwTitle, 0.f, 0.f, 0.f, 0.f);
		api.setUIOffset(pwTitle, 200.f, 380.f);
		api.setUITextFont(pwTitle, font, 15.f);
		api.setUITextColor(pwTitle, { 200, 200, 255, 255 });

		auto pwInput = api.createTextInput(220.f, 32.f);
		api.setUIParent(pwInput, canvas);
		api.setUIAnchor(pwInput, 0.f, 0.f, 0.f, 0.f);
		api.setUIOffset(pwInput, 200.f, 408.f);
		api.setTextInputFont(pwInput, font, 18.f);
		api.setTextInputPlaceholder(pwInput, "Enter password...");
		api.setTextInputPasswordMode(pwInput, true);

		// 确认提交按钮
		auto submitBtn = api.createButton(100.f, 32.f, []() {
			printf("[TextInput] Submit button clicked\n");
		});
		api.setUIParent(submitBtn, canvas);
		api.setUIAnchor(submitBtn, 0.f, 0.f, 0.f, 0.f);
		api.setUIOffset(submitBtn, 430.f, 310.f);
		api.setButtonColors(submitBtn,
			{ 70, 120, 180, 255 },
			{ 100, 150, 210, 255 },
			{ 50, 90, 140, 255 });
		api.setUITooltip(submitBtn, "Submit (same as Enter)", font, 12.f, 0.f);

		auto submitLbl = api.createUIText(100.f, 32.f, "Submit");
		api.setUIParent(submitLbl, submitBtn);
		api.setUIAnchor(submitLbl, 0.f, 0.f, 1.f, 1.f);
		api.setUITextFont(submitLbl, font, 16.f);
		api.setUITextColor(submitLbl, { 255, 255, 255, 255 });
		api.setUISortOrder(submitLbl, 1);
	}

	// ── 底部提示 ─────────────────────────────────────────────────────────────────
	{
		auto hint = api.createUIText(700.f, 24.f,
			"F1 GPU-driven | N switch prefab scene instance | ESC quit | arrows move camera");
		api.setUIParent(hint, canvas);
		api.setUIAnchor(hint, 0.5f, 1.f, 0.5f, 1.f);
		api.setUIPivot(hint, 0.5f, 1.f);
		api.setUIOffset(hint, 0.f, -16.f);
		api.setUITextFont(hint, font, 14.f);
		api.setUITextColor(hint, { 150, 150, 150, 255 });
	}

	// ── 主循环 ────────────────────────────────────────────────────────────────
	constexpr float kSpeed = 200.f;
	bool gpuDrivenEnabled = false;

	float maskRunT = 0.f;
	float tintDemoT = 0.f;
	float particleDemoT = 0.f;
	bool demoRunning = true;
	demo.setUpdate([&](engine::GameContext& fw, float dt) {
		(void)fw;
		auto& playerAnimator = api.getComponent<engine::AnimatorComponent>(player);

		//if (particleEmitter != entt::null) {
		//	particleDemoT += dt;
		//	api.patchComponent<engine::Transform>(particleEmitter, [&](engine::Transform& tf) {
		//		tf.rotation = -1.35f + std::sin(particleDemoT * 1.8f) * 0.65f;
		//		tf.x = 520.f + std::cos(particleDemoT * 0.9f) * 42.f;
		//		tf.y = 260.f + std::sin(particleDemoT * 1.1f) * 24.f;
		//	});
		//}

		// Region Tint demo：每个 entity 在调色板间循环平滑过渡。
		// 每段 transition 持续 kCycleSec 秒；entity i 的相位偏移 i*0.6 错峰。
		if (!tintDemoEntities.empty()) {
			tintDemoT += dt;
			constexpr float kCycleSec = 2.5f;
			auto lerpU8 = [](uint8_t a, uint8_t b, float t) -> uint8_t {
				return static_cast<uint8_t>(int(a) + int(int(b) - int(a)) * t);
			};
			auto lerpColor = [&](core::Color a, core::Color b, float t) {
				return core::Color{ lerpU8(a.r,b.r,t), lerpU8(a.g,b.g,t),
				                    lerpU8(a.b,b.b,t), lerpU8(a.a,b.a,t) };
			};
			for (size_t i = 0; i < tintDemoEntities.size(); ++i) {
				const float phase = tintDemoT / kCycleSec + static_cast<float>(i) * 0.6f;
				const int   pIdx  = static_cast<int>(std::floor(phase));
				float       f     = phase - static_cast<float>(pIdx);
				f = f * f * (3.f - 2.f * f);  // smoothstep
				const TintPalette& a = kTintPalettes[((pIdx)     % 10 + 10) % 10];
				const TintPalette& b = kTintPalettes[((pIdx + 1) % 10 + 10) % 10];
				auto& tnt = api.getComponent<engine::Tinting>(tintDemoEntities[i]);
				tnt.slots[1].color = lerpColor(a.skin,  b.skin,  f);
				tnt.slots[2].color = lerpColor(a.hair,  b.hair,  f);
				tnt.slots[3].color = lerpColor(a.shirt, b.shirt, f);
				tnt.slots[4].color = lerpColor(a.pants, b.pants, f);
				tnt.slots[5].color = lerpColor(a.shoes, b.shoes, f);
			}
		}

		// UIMask runner: 在 200 宽的面板里左右往返 (-30 → 230)，方块本身 40 宽，
		// 所以两侧各有 ~30px 会越过边界 —— 启用 mask 时被裁掉、关闭时溢出可见。
		if (maskRunner != entt::null) {
			maskRunT += dt;
			float u = 0.5f - 0.5f * std::cos(maskRunT * 1.6f);   // 0..1
			float x = -30.f + u * (230.f - (-30.f));
			api.setUIOffset(maskRunner, x, 70.f);
		}

		// WASD 控制 player 移动
		float dx = 0.f, dy = 0.f;
		if (api.isKeyDown(SDLK_W)) dy -= 1.f;
		if (api.isKeyDown(SDLK_S)) dy += 1.f;
		if (api.isKeyDown(SDLK_A)) dx -= 1.f;
		if (api.isKeyDown(SDLK_D)) dx += 1.f;
		const bool isMoving = (dx != 0.f || dy != 0.f);

		// 只有真正移动时才 patch —— 触发 on_update 让 RenderSystem 把新位置回写 GPU；
		// 直接通过 get<>() 的引用赋值不会触发信号，GPU 缓冲会停留在旧位置。
		if (isMoving) {
			api.patchComponent<engine::Transform>(player, [&](engine::Transform& tf) {
				tf.x += dx * kSpeed * dt;
				tf.y += dy * kSpeed * dt;
				});
		}

		// 方向键控制 World 摄像机位置
		float camDx = 0.f, camDy = 0.f;
		if (api.isKeyDown(SDLK_UP))    camDy -= 1.f;
		if (api.isKeyDown(SDLK_DOWN))  camDy += 1.f;
		if (api.isKeyDown(SDLK_LEFT))  camDx -= 1.f;
		if (api.isKeyDown(SDLK_RIGHT)) camDx += 1.f;
		if (camDx != 0.f || camDy != 0.f) {
			api.patchComponent<engine::Transform>(worldCamera, [&](engine::Transform& tf) {
				tf.x += camDx * kSpeed * dt;
				tf.y += camDy * kSpeed * dt;
				});
		}

		// Phase 1: walk 走最低优先级；attack 锁定时不打断
		if (isMoving) {
			if (!playerAnimator.playing && playerAnimator.interruptible) {
				engine::PlayOptions o; o.priority = 0;
				playerAnimator.play(playerAnim, o);
			}
		}
		else {
			if (playerAnimator.playing && playerAnimator.interruptible && playerAnimator.currentAnim == playerAnim) playerAnimator.stop();
		}

		// Phase 1 手动测试键
		// J: 高优先级 attack + lock（不可被打断的 one-shot）
		if (api.isKeyJustPressed(SDLK_J)) {
			engine::PlayOptions opts;
			opts.priority = 10;
			opts.forceRestart = true;
			opts.mode = engine::PlayMode::Once;
			playerAnimator.play(attackAnim, opts);
			playerAnimator.lock();
			printf("[Phase1] J: attack play (prio=10, locked, one-shot)\n");
		}
		// K: 低优先级 walk 请求 — 锁定时应入队、否则立即播
		if (api.isKeyJustPressed(SDLK_K)) {
			engine::PlayOptions opts; opts.priority = 1;
			bool wasLocked = !playerAnimator.interruptible;
			AnimationHandle prev = playerAnimator.currentAnim;
			playerAnimator.play(playerAnim, opts);
			const char* result = (playerAnimator.currentAnim == playerAnim && prev != playerAnim) ? "switched"
				: playerAnimator.hasQueued ? "queued"
				: "kept-current";
			printf("[Phase1] K: low-prio walk -> %s (locked=%d)\n", result, wasLocked ? 1 : 0);
		}
		// L: 入队 walk —— 在 attack 完成后自动接续
		if (api.isKeyJustPressed(SDLK_L)) {
			engine::PlayOptions opts; opts.priority = 0;
			playerAnimator.queue(playerAnim, opts);
			printf("[Phase1] L: queued walk for after current\n");
		}
		// U: 解锁当前
		if (api.isKeyJustPressed(SDLK_U)) {
			playerAnimator.unlock();
			printf("[Phase1] U: unlocked\n");
		}

		// Phase 2: 消费帧事件队列 (AnimatorSystem 在每帧 update 起始清空)
		if (auto* eq = ctx.world.try_get<engine::AnimEventQueue>(player)) {
			for (auto& ev : eq->events) {
				printf("[Phase2] event '%s' @ t=%.3f (int=%d float=%.2f str='%s')\n",
					ev.name.c_str(), ev.time, ev.intParam, ev.floatParam, ev.stringParam.c_str());
			}
		}

		// ── Phase 3: 仅写参数, 不调 play() ──────────────────────────────────
		{
			auto& fsm = api.getComponent<engine::AnimatorComponent>(fsmHero);
			fsm.setFloat("speed", isMoving ? 1.f : 0.f);
			if (api.isKeyJustPressed(SDLK_F)) {
				fsm.setTrigger("attack");
				printf("[Phase3] F: setTrigger(attack)\n");
			}
			if (auto* eq = ctx.world.try_get<engine::AnimEventQueue>(fsmHero)) {
				for (auto& ev : eq->events) {
					if (ev.name.rfind("state_", 0) == 0) {
						printf("[Phase3] %s\n", ev.name.c_str());
					}
					else {
						printf("[Phase3] anim event '%s' @ t=%.3f\n", ev.name.c_str(), ev.time);
					}
				}
			}
		}

		// ═════════════════════════════════════════════════════════════════════
		// Phase 5.3: 程序化层触发测试
		// ═════════════════════════════════════════════════════════════════════
		// 1: HitShake 受击抖动
		if (api.isKeyJustPressed(SDLK_1)) {
			auto& anim = api.getComponent<engine::AnimatorComponent>(hitShakeTest);
			anim.setTrigger("hit");
			printf("[Phase5] 1: HitShake triggered on entity @700,300\n");
		}
		// 2: HurtFlash 受击红闪
		if (api.isKeyJustPressed(SDLK_2)) {
			auto& anim = api.getComponent<engine::AnimatorComponent>(hurtFlashTest);
			anim.setTrigger("hurt");
			printf("[Phase5] 2: HurtFlash triggered on entity @800,300\n");
		}
		// 3: BreatheBob 强度调整 (循环增加)
		if (api.isKeyJustPressed(SDLK_3)) {
			auto& anim = api.getComponent<engine::AnimatorComponent>(breatheBobTest);
			float strength = anim.getFloat("breathStrength") + 0.5f;
			if (strength > 3.f) strength = 0.5f;
			anim.setFloat("breathStrength", strength);
			printf("[Phase5] 3: BreatheBob strength = %.1f\n", strength);
		}
		// 4: SquashStretch 落地挤压
		if (api.isKeyJustPressed(SDLK_4)) {
			auto& anim = api.getComponent<engine::AnimatorComponent>(squashTest);
			anim.setTrigger("land");
			printf("[Phase5] 4: SquashStretch triggered on entity @1000,300\n");
		}
		// 5: 综合测试 - 同时触发多个效果
		if (api.isKeyJustPressed(SDLK_5)) {
			auto& anim = api.getComponent<engine::AnimatorComponent>(proceduralComboTest);
			anim.setTrigger("comboHit");
			anim.setTrigger("comboHurt");
			printf("[Phase5] 5: Combo (HitShake + HurtFlash) triggered on entity @1100,300\n");
		}

		// Phase 5 时间缩放测试 (6/7 键)
		if (api.isKeyJustPressed(SDLK_6)) {
			auto& anim = api.getComponent<engine::AnimatorComponent>(hitShakeTest);
			anim.localTimeScale = anim.localTimeScale * 0.5f;
			if (anim.localTimeScale < 0.125f) anim.localTimeScale = 1.f;
			printf("[Phase5] 6: HitShake timeScale = %.2f\n", anim.localTimeScale);
		}
		if (api.isKeyJustPressed(SDLK_7)) {
			auto& anim = api.getComponent<engine::AnimatorComponent>(breatheBobTest);
			anim.localTimeScale = anim.localTimeScale * 0.5f;
			if (anim.localTimeScale < 0.125f) anim.localTimeScale = 1.f;
			printf("[Phase5] 7: BreatheBob timeScale = %.2f\n", anim.localTimeScale);
		}

		// S2 prefab scene 切换测试：prefab_demo.scene.json 里有两个 instance，
		// N 键在它们之间切换，验证 scene-local overrides 都能恢复到各自原值。
		if (api.isKeyJustPressed(SDLK_N) && !prefabSceneEntities.empty()) {
			activePrefabSceneIndex = (activePrefabSceneIndex + 1) % static_cast<int>(prefabSceneEntities.size());
			applyPrefabSceneSwitch();
			if (api.hasComponent<engine::Name>(prefabSceneEntities[activePrefabSceneIndex])) {
				const auto& name = api.getComponent<engine::Name>(prefabSceneEntities[activePrefabSceneIndex]);
				printf("[PrefabDataTest] switched to prefab scene instance %d: %s\n",
					activePrefabSceneIndex, name.c_str());
			} else {
				printf("[PrefabDataTest] switched to prefab scene instance %d\n", activePrefabSceneIndex);
			}
		}

		auto& statusSpr = api.getComponent<engine::Sprite>(statusText);
		statusSpr.texture = isMoving ? movingTex : stoppedTex;

		// ── G 键切换 GPU-driven 渲染 ───────────────────────────────────────────
		if (api.isKeyJustPressed(SDLK_G)) {
			gpuDrivenEnabled = !gpuDrivenEnabled;
			if (ctx.systems.has<engine::RenderSystem>()) {
				auto& renderSystem = ctx.systems.get<engine::RenderSystem>();
				renderSystem.setGPUDrivenEnabled(gpuDrivenEnabled);

				printf("\n");
				printf("+----------------------------------------------------------------+\n");
				printf("|  Rendering Mode: %-45s  |\n",
					gpuDrivenEnabled ? "GPU-DRIVEN (Fast)" : "CPU-DRIVEN (Traditional)");
				printf("+----------------------------------------------------------------+\n");
				if (gpuDrivenEnabled) {
					printf("|  + GPU Culling:  Parallel O(n/64) on GPU                      |\n");
					printf("|  + GPU Sorting:  Radix Sort O(n) on GPU                       |\n");
					printf("|  + Draw Calls:   1-10 (batched)                               |\n");
					printf("|  + CPU Time:     < 0.5ms                                      |\n");
				}
				else {
					printf("|  - CPU Culling:  Linear O(n) scan                             |\n");
					printf("|  - CPU Sorting:  QuickSort O(n log n)                         |\n");
					printf("|  - Draw Calls:   10-100+ (texture switches)                   |\n");
					printf("|  - CPU Time:     5-20ms (depends on sprite count)             |\n");
				}
				printf("+----------------------------------------------------------------+\n\n");
			}

			// 更新状态文本
			auto& gpuTxt = api.getComponent<engine::TextComponent>(gpuStatusText);
			if (gpuDrivenEnabled) {
				gpuTxt.text = "[GPU Mode] Press G to switch to CPU mode";
				gpuTxt.color = { 100, 255, 100, 255 };
			}
			else {
				gpuTxt.text = "[CPU Mode] Press G to enable GPU-driven rendering";
				gpuTxt.color = { 255, 200, 100, 255 };
			}

			// 更新架构说明
			auto& archTxt = api.getComponent<engine::TextComponent>(archText);
			if (gpuDrivenEnabled) {
				archTxt.text = "Architecture: GPU-Driven (Compute Culling + Indirect Draw)";
				archTxt.color = { 100, 200, 150, 255 };
			}
			else {
				archTxt.text = "Architecture: CPU-Driven (Traditional Scan + Sort)";
				archTxt.color = { 120, 120, 150, 255 };
			}
		}

		// ── 更新性能统计显示 ───────────────────────────────────────────────────
		{
			static float fpsAccum = 0.f;
			static int fpsFrameCount = 0;
			static float displayFps = 0.f;

			fpsAccum += dt;
			fpsFrameCount++;

			// 每 0.5 秒更新一次 FPS 显示
			if (fpsAccum >= 0.5f) {
				displayFps = fpsFrameCount / fpsAccum;
				fpsAccum = 0.f;
				fpsFrameCount = 0;
			}

			if (ctx.systems.has<engine::RenderSystem>()) {
				auto& renderSystem = ctx.systems.get<engine::RenderSystem>();
				uint32_t spriteCount = renderSystem.spriteBuffer().activeCount();

				// 计算可见精灵数 (简化：使用精灵总数作为估计)
				// 实际可见数由 GPU/视口裁剪决定
				uint32_t estimatedVisible = spriteCount;

				auto& perfTxt = api.getComponent<engine::TextComponent>(perfText);
				char buf[128];
				snprintf(buf, sizeof(buf),
					"FPS: %.0f | Sprites: %u | Mode: %s",
					displayFps,
					spriteCount,
					gpuDrivenEnabled ? "GPU" : "CPU");
				perfTxt.text = buf;

				// 根据性能调整颜色
				if (displayFps >= 55.f) {
					perfTxt.color = { 100, 255, 100, 255 };  // 绿色 - 良好
				}
				else if (displayFps >= 30.f) {
					perfTxt.color = { 255, 255, 100, 255 };  // 黄色 - 一般
				}
				else {
					perfTxt.color = { 255, 100, 100, 255 };  // 红色 - 需优化
				}
			}
		}

		// ═════════════════════════════════════════════════════════════════════
		// 物理系统测试：射线检测 + 区域查询
		// ═════════════════════════════════════════════════════════════════════

		// R 键：从玩家位置向下发射射线，检测地面
		if (api.isKeyJustPressed(SDLK_R)) {
			auto& playerTf = api.getComponent<engine::Transform>(player);
			auto hit = api.raycast(playerTf.x, playerTf.y, 0.f, 1.f, 1000.f, engine::COLLISION_LAYER_STATIC);
			if (hit.hit) {
				printf("[Physics] Raycast hit: entity at (%.1f, %.1f), dist=%.1f, normal=(%.2f, %.2f)\n",
					hit.hitX, hit.hitY, hit.distance, hit.normalX, hit.normalY);
				// 显示命中点标记
				api.patchComponent<engine::Transform>(rayHitMarker, [&](engine::Transform& tf) {
					tf.x = hit.hitX - 4.f;
					tf.y = hit.hitY - 4.f;
					});
			}
			else {
				printf("[Physics] Raycast missed\n");
			}
		}

		// T 键：以玩家为中心进行盒形区域查询
		if (api.isKeyJustPressed(SDLK_T)) {
			auto& playerTf = api.getComponent<engine::Transform>(player);
			auto hits = api.overlapBox(playerTf.x, playerTf.y, 100.f, 100.f, engine::COLLISION_LAYER_ALL);
			printf("[Physics] OverlapBox found %zu entities:\n", hits.size());
			for (auto& h : hits) {
				if (api.hasComponent<engine::EntityID>(h.entity)) {
					auto& id = api.getComponent<engine::EntityID>(h.entity);
					printf("  - %s (overlap: %.1fx%.1f)\n", id.c_str(), h.overlapX, h.overlapY);
				}
			}
		}

		// Y 键：以玩家为中心进行圆形区域查询
		if (api.isKeyJustPressed(SDLK_Y)) {
			auto& playerTf = api.getComponent<engine::Transform>(player);
			auto hits = api.overlapCircle(playerTf.x, playerTf.y, 150.f, engine::COLLISION_LAYER_ALL);
			printf("[Physics] OverlapCircle found %zu entities in radius 150\n", hits.size());
		}

		// Space 键：重置物理方块位置
		if (api.isKeyJustPressed(SDLK_SPACE)) {
			api.patchComponent<engine::Transform>(physicsBox, [&](engine::Transform& tf) {
				tf.x = 600.f;
				tf.y = 50.f;
				});
			auto& rb = api.getComponent<engine::RigidBody>(physicsBox);
			rb.velocityX = 0.f;
			rb.velocityY = 0.f;
			printf("[Physics] Reset physics box to (600, 50)\n");
		}

		// P 键：打印碰撞层测试结果
		if (api.isKeyJustPressed(SDLK_P)) {
			printf("\n[Physics] Collision Layer Test:\n");
			printf("  - Ground: layer=STATIC, mask=DEFAULT|PLAYER|ENEMY\n");
			printf("  - PhysicsBox: layer=PLAYER, mask=ALL\n");
			printf("  - Bullet: layer=BULLET, mask=ENEMY (should pass through ground)\n");
			printf("  - Enemy: layer=ENEMY, mask=ALL\n");
			printf("  - PickupZone: layer=PICKUP, mask=PLAYER (isTrigger=true)\n");
			printf("  Expected: Bullet passes through ground, stops at enemy\n");
		}

		// ═════════════════════════════════════════════════════════════════════
		// UI System v2 更新测试
		// ═════════════════════════════════════════════════════════════════════
		{
			progressValue += dt * 0.3f;
			if (progressValue > 1.f) progressValue = 0.f;
			api.setProgressValue(progressBar, progressValue);

			// 实时刷新音量/开关/输入标签
			{
				char buf[64];
				snprintf(buf, sizeof(buf), "Volume: %.0f%%", api.getSliderValue(volumeSlider));
				api.setUIText(volumeLabel, buf);

				api.setUIText(soundLabel, api.getToggleValue(soundToggle) ? "Sound: ON" : "Sound: OFF");
			}

			// 实时显示 TextInput 内容
			{
				const char* t = api.getTextInputText(textInput);
				if (*t) {
					char buf[128];
					snprintf(buf, sizeof(buf), "Current: \"%s\" (len=%zu)", t, strlen(t));
					api.setUIText(inputLabel, buf);
				} else {
					api.setUIText(inputLabel, "(empty — click to type)");
				}
			}

			// 玩家血条脉动 (1 → 0 → 1)
			playerHealth -= dt * 0.15f;
			if (playerHealth < 0.f) playerHealth = 1.f;
			api.setProgressValue(playerHealthBar, playerHealth);

			// 悬停日志
			auto hoveredUI = api.getHoveredUI();
			static entt::entity lastHovered = entt::null;
			if (hoveredUI != lastHovered && hoveredUI != entt::null) {
				if (api.hasComponent<engine::UINode>(hoveredUI)) {
					float x, y, w, h;
					api.getUIComputedRect(hoveredUI, &x, &y, &w, &h);
					printf("[UI] Hovered element at (%.0f, %.0f) size (%.0f x %.0f)\n", x, y, w, h);
				}
			}
			lastHovered = hoveredUI;
		}

		if (api.isKeyJustPressed(SDLK_ESCAPE)) {
			api.quit();
			demoRunning = false;
		}
	});

	while (demoRunning && ctx.scheduler.tick()) {
		demo.onUpdate(gameCtx, ctx.scheduler.deltaTime());
	}

	demo.onShutdown(gameCtx);
	ctx.shutdown();
	return 0;
}
