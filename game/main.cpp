#include <SDL3/SDL_main.h>
#include <engine/runtime/EngineContext.h>
#include <engine/runtime/EngineConfig.h>
#include <engine/api/GameAPI.h>
#include <engine/components/RenderComponents.h>
#include <engine/components/PhysicsComponents.h>
#include <SDL3/SDL.h>   // SDLK_* key codes
#include <vector>

// ── 程序化纹理生成（仅依赖 core::Color，不 include backend）────────────────────

static std::vector<uint8_t> makeCheckerboard(
    int w, int h, int cellSize, core::Color a, core::Color b)
{
    std::vector<uint8_t> px(w * h * 4);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            bool even = ((x / cellSize) + (y / cellSize)) % 2 == 0;
            core::Color c = even ? a : b;
            int i = (y * w + x) * 4;
            px[i]=c.r; px[i+1]=c.g; px[i+2]=c.b; px[i+3]=c.a;
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
                    bool border = tx==0||ty==0||tx==tileSize-1||ty==tileSize-1;
                    core::Color fc = border ? core::Color{30,30,30,255} : c;
                    int i = ((row*tileSize+ty)*w + col*tileSize+tx) * 4;
                    px[i]=fc.r; px[i+1]=fc.g; px[i+2]=fc.b; px[i+3]=fc.a;
                }
        }
    return px;
}

int main(int /*argc*/, char* /*argv*/[]) {
    engine::EngineConfig cfg;
    cfg.windowTitle  = "StarEngine — Render Test";
    cfg.windowWidth  = 1280;
    cfg.windowHeight = 720;

    engine::EngineContext ctx;
    ctx.init(cfg);
    engine::GameAPI api{ctx};

    // ── 上传程序化纹理 ────────────────────────────────────────────────────────
    auto checkerPx = makeCheckerboard(64, 64, 8,
        {255,100,100,255}, {100,100,255,255});
    TextureHandle spriteTex = api.createTextureFromMemory(checkerPx.data(), 64, 64);

    auto tilesetPx = makeColorTileset(32, 4, 2);
    TextureHandle tilesetTex = api.createTextureFromMemory(tilesetPx.data(), 128, 64);

    // ── 摄像机 ────────────────────────────────────────────────────────────────
    {
        auto e = api.spawnEntity();
        api.addComponent(e, engine::Transform{0.f, 0.f});
        api.addComponent(e, engine::Camera{1.f, true});
    }

    // ── Sprite 1：原始大小，居左上 ───────────────────────────────────────────
    {
        auto e = api.spawnEntity();
        engine::Transform tf{}; tf.x = 200.f; tf.y = 200.f;
        api.addComponent(e, tf);
        engine::Sprite sp{};
        sp.texture = spriteTex;
        sp.srcRect = {0.f, 0.f, 64.f, 64.f};
        sp.layer   = 1;
        api.addComponent(e, sp);
    }

    // ── Sprite 2：旋转 45°，放大 2×，半透明橙色 tint ─────────────────────────
    {
        auto e = api.spawnEntity();
        engine::Transform tf{}; tf.x=400.f; tf.y=200.f;
        tf.rotation=0.785f; tf.scaleX=2.f; tf.scaleY=2.f;
        api.addComponent(e, tf);
        engine::Sprite sp{};
        sp.texture=spriteTex;
        sp.srcRect={0.f,0.f,64.f,64.f};
        sp.layer=1;
        sp.tint={255,200,100,200};
        api.addComponent(e, sp);
    }

    // ── Sprite 3：横向拉伸，绿色 tint ────────────────────────────────────────
    {
        auto e = api.spawnEntity();
        engine::Transform tf{}; tf.x=640.f; tf.y=200.f;
        tf.scaleX=3.f; tf.scaleY=1.5f;
        api.addComponent(e, tf);
        engine::Sprite sp{};
        sp.texture=spriteTex; sp.srcRect={0.f,0.f,64.f,64.f};
        sp.layer=2; sp.tint={100,255,100,255};
        api.addComponent(e, sp);
    }

    // ── TileMap：20×5 格，覆盖屏幕底部 ──────────────────────────────────────
    {
        auto e = api.spawnEntity();
        engine::Transform tf{}; tf.x=0.f; tf.y=400.f;
        api.addComponent(e, tf);

        engine::TileMap tmap{};
        tmap.width=20; tmap.height=5; tmap.tileSize=32;
        tmap.tileset=tilesetTex;
        for (int y=0;y<5;++y)
            for (int x=0;x<20;++x)
                tmap.layers[0].push_back((x+y)%8);
        tmap.layers[1].resize(100, -1);
        tmap.layers[1][2]=4; tmap.layers[1][12]=5;
        api.addComponent(e, tmap);
    }

    // ── 可控精灵（Month 5：WASD 移动，Escape 退出）────────────────────────────
    entt::entity player;
    {
        player = api.spawnEntity();
        engine::Transform tf{}; tf.x = 640.f; tf.y = 300.f;
        api.addComponent(player, tf);
        engine::Sprite sp{};
        sp.texture = spriteTex; sp.srcRect = {0.f, 0.f, 64.f, 64.f};
        sp.layer = 3; sp.tint = {255, 255, 100, 255};
        api.addComponent(player, sp);
    }

    // 手动主循环：tick() 完成后 inputState 已更新，直接修改 Transform
    constexpr float kSpeed = 200.f;
    while (ctx.scheduler.tick()) {
        float dt = ctx.scheduler.deltaTime();

        auto& tf = api.getComponent<engine::Transform>(player);
        if (api.isKeyDown(SDLK_W) || api.isKeyDown(SDLK_UP))   tf.y -= kSpeed * dt;
        if (api.isKeyDown(SDLK_S) || api.isKeyDown(SDLK_DOWN))  tf.y += kSpeed * dt;
        if (api.isKeyDown(SDLK_A) || api.isKeyDown(SDLK_LEFT))  tf.x -= kSpeed * dt;
        if (api.isKeyDown(SDLK_D) || api.isKeyDown(SDLK_RIGHT)) tf.x += kSpeed * dt;

        if (api.isKeyJustPressed(SDLK_ESCAPE)) { api.quit(); break; }
    }

    ctx.shutdown();
    return 0;
}
