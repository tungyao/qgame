#include <cassert>
#include <cstdio>
#include <cstring>

// 直接 include engine 层（不需要 GPU）
#include <entt/entt.hpp>
#include "engine/components/RenderComponents.h"
#include "backend/renderer/CommandBuffer.h"

// ── 工具：遍历 CommandBuffer 统计命令数量 ─────────────────────────────────────
struct CmdStats {
    int clears  = 0;
    int sprites = 0;
    int tiles   = 0;
    int cameras = 0;
};

static CmdStats countCmds(const backend::CommandBuffer& cb) {
    CmdStats s;
    for (const auto& cmd : cb.commands()) {
        std::visit([&](const auto& c) {
            using T = std::decay_t<decltype(c)>;
            if constexpr (std::is_same_v<T, backend::ClearCmd>)      ++s.clears;
            if constexpr (std::is_same_v<T, backend::DrawSpriteCmd>) ++s.sprites;
            if constexpr (std::is_same_v<T, backend::DrawTileCmd>)   ++s.tiles;
            if constexpr (std::is_same_v<T, backend::SetCameraCmd>)  ++s.cameras;
        }, cmd);
    }
    return s;
}

// ── 测试：CommandBuffer 基本录制 ─────────────────────────────────────────────
void testCommandBufferRecording() {
    backend::CommandBuffer cb;
    cb.begin();
    cb.clear({0, 0, 0, 255});
    cb.setCamera({});
    cb.drawSprite({});
    cb.drawSprite({});
    cb.drawTile({});
    cb.end();

    auto s = countCmds(cb);
    assert(s.clears  == 1);
    assert(s.sprites == 2);
    assert(s.tiles   == 1);
    assert(s.cameras == 1);
    assert(!cb.isRecording());
    ::printf("  CommandBuffer recording OK\n");
}

// ── 测试：DrawSpriteCmd 字段正确写入 ─────────────────────────────────────────
void testSpriteCmdFields() {
    backend::CommandBuffer cb;
    cb.begin();
    backend::DrawSpriteCmd cmd{};
    cmd.x        = 100.f;
    cmd.y        = 200.f;
    cmd.rotation = 1.57f;
    cmd.scaleX   = 2.f;
    cmd.scaleY   = 3.f;
    cmd.layer    = 5;
    cmd.tint     = {255, 0, 128, 200};
    cmd.srcRect  = {0.f, 0.f, 32.f, 32.f};
    cb.drawSprite(cmd);
    cb.end();

    const auto& cmds = cb.commands();
    assert(cmds.size() == 1);
    const auto& stored = std::get<backend::DrawSpriteCmd>(cmds[0]);
    assert(stored.x        == 100.f);
    assert(stored.y        == 200.f);
    assert(stored.layer    == 5);
    assert(stored.tint.r   == 255);
    assert(stored.tint.g   == 0);
    assert(stored.scaleX   == 2.f);
    ::printf("  DrawSpriteCmd fields OK\n");
}

// ── 测试：DrawTileCmd 字段 ────────────────────────────────────────────────────
void testTileCmdFields() {
    backend::CommandBuffer cb;
    cb.begin();
    backend::DrawTileCmd cmd{};
    cmd.tileId   = 42;
    cmd.gridX    = 3;
    cmd.gridY    = 7;
    cmd.tileSize = 16;
    cmd.layer    = 1;
    cb.drawTile(cmd);
    cb.end();

    const auto& stored = std::get<backend::DrawTileCmd>(cb.commands()[0]);
    assert(stored.tileId   == 42);
    assert(stored.gridX    == 3);
    assert(stored.gridY    == 7);
    assert(stored.tileSize == 16);
    ::printf("  DrawTileCmd fields OK\n");
}

// ── 测试：CommandBuffer reset 后可重用 ───────────────────────────────────────
void testCommandBufferReset() {
    backend::CommandBuffer cb;
    cb.begin(); cb.drawSprite({}); cb.end();
    assert(cb.commands().size() == 1);

    cb.reset();
    assert(cb.commands().empty());
    assert(!cb.isRecording());

    cb.begin(); cb.drawTile({}); cb.drawTile({}); cb.end();
    assert(cb.commands().size() == 2);
    ::printf("  CommandBuffer reset OK\n");
}

// ── 测试：ECS 组件写入和读取 ─────────────────────────────────────────────────
void testECSComponents() {
    entt::registry world;

    auto e1 = world.create();
    world.emplace<engine::Transform>(e1, 10.f, 20.f, 0.f, 1.f, 1.f);
    world.emplace<engine::Sprite>(e1);

    auto e2 = world.create();
    world.emplace<engine::Transform>(e2, 0.f, 0.f, 0.f, 1.f, 1.f);
    engine::TileMap tmap;
    tmap.width = 4; tmap.height = 4; tmap.tileSize = 16;
    for (int i = 0; i < 16; ++i) tmap.layers[0].push_back(i % 8);
    world.emplace<engine::TileMap>(e2, std::move(tmap));

    // 验证 view 能正确找到组件
    int spriteCount = 0;
    for (auto [ent, tf, sp] : world.view<engine::Transform, engine::Sprite>().each()) {
        assert(tf.x == 10.f && tf.y == 20.f);
        ++spriteCount;
    }
    assert(spriteCount == 1);

    int tileCount = 0;
    for (auto [ent, tf, tm] : world.view<engine::Transform, engine::TileMap>().each()) {
        assert(tm.tileAt(0, 2, 1) == 6);  // index=6, tileId = 6 % 8 = 6
        ++tileCount;
    }
    assert(tileCount == 1);
    ::printf("  ECS components OK\n");
}

// ── 测试：TileMap::tileAt 边界检查 ───────────────────────────────────────────
void testTileMapBounds() {
    engine::TileMap tm;
    tm.width = 3; tm.height = 3; tm.tileSize = 16;
    tm.layers[0] = {1,2,3, 4,5,6, 7,8,9};

    assert(tm.tileAt(0, 0, 0) == 1);
    assert(tm.tileAt(0, 2, 2) == 9);
    assert(tm.tileAt(0, -1, 0) == -1);   // 越界
    assert(tm.tileAt(0,  3, 0) == -1);
    assert(tm.tileAt(3,  0, 0) == -1);   // 无效 layer
    ::printf("  TileMap bounds OK\n");
}

// ── 测试：多个图形和不同颜色 ─────────────────────────────────────────────────
void testMultipleShapesWithColors() {
    backend::CommandBuffer cb;
    cb.begin();
    cb.clear({50, 50, 50, 255});

    backend::DrawSpriteCmd sprite1;
    sprite1.x = 10.f; sprite1.y = 10.f;
    sprite1.layer = 0;
    sprite1.tint = {255, 0, 0, 255};    // 红色
    sprite1.srcRect = {0.f, 0.f, 32.f, 32.f};
    cb.drawSprite(sprite1);

    backend::DrawSpriteCmd sprite2;
    sprite2.x = 50.f; sprite2.y = 10.f;
    sprite2.layer = 0;
    sprite2.tint = {0, 255, 0, 255};    // 绿色
    sprite2.srcRect = {32.f, 0.f, 32.f, 32.f};
    cb.drawSprite(sprite2);

    backend::DrawSpriteCmd sprite3;
    sprite3.x = 90.f; sprite3.y = 10.f;
    sprite3.layer = 1;
    sprite3.tint = {0, 0, 255, 255};    // 蓝色
    sprite3.srcRect = {64.f, 0.f, 32.f, 32.f};
    cb.drawSprite(sprite3);

    backend::DrawSpriteCmd sprite4;
    sprite4.x = 10.f; sprite4.y = 50.f;
    sprite4.layer = 1;
    sprite4.tint = {255, 255, 0, 255};  // 黄色
    sprite4.srcRect = {0.f, 32.f, 32.f, 32.f};
    cb.drawSprite(sprite4);

    backend::DrawSpriteCmd sprite5;
    sprite5.x = 50.f; sprite5.y = 50.f;
    sprite5.layer = 2;
    sprite5.tint = {255, 0, 255, 255};  // 紫色
    sprite5.srcRect = {32.f, 32.f, 32.f, 32.f};
    cb.drawSprite(sprite5);

    backend::DrawSpriteCmd sprite6;
    sprite6.x = 90.f; sprite6.y = 50.f;
    sprite6.layer = 2;
    sprite6.tint = {0, 255, 255, 255};  // 青色
    sprite6.srcRect = {64.f, 32.f, 32.f, 32.f};
    cb.drawSprite(sprite6);

    cb.end();

    auto s = countCmds(cb);
    assert(s.clears  == 1);
    assert(s.sprites == 6);
    assert(s.cameras == 0);

    const auto& cmds = cb.commands();
    assert(cmds.size() == 7);

    const auto& cmd1 = std::get<backend::DrawSpriteCmd>(cmds[1]);
    assert(cmd1.tint.r == 255 && cmd1.tint.g == 0 && cmd1.tint.b == 0);

    const auto& cmd2 = std::get<backend::DrawSpriteCmd>(cmds[2]);
    assert(cmd2.tint.r == 0 && cmd2.tint.g == 255 && cmd2.tint.b == 0);

    const auto& cmd3 = std::get<backend::DrawSpriteCmd>(cmds[3]);
    assert(cmd3.tint.r == 0 && cmd3.tint.g == 0 && cmd3.tint.b == 255);
    assert(cmd3.layer == 1);

    const auto& cmd4 = std::get<backend::DrawSpriteCmd>(cmds[4]);
    assert(cmd4.tint.r == 255 && cmd4.tint.g == 255 && cmd4.tint.b == 0);

    const auto& cmd5 = std::get<backend::DrawSpriteCmd>(cmds[5]);
    assert(cmd5.tint.r == 255 && cmd5.tint.g == 0 && cmd5.tint.b == 255);

    const auto& cmd6 = std::get<backend::DrawSpriteCmd>(cmds[6]);
    assert(cmd6.tint.r == 0 && cmd6.tint.g == 255 && cmd6.tint.b == 255);

    ::printf("  Multiple shapes with colors OK\n");
}

int main() {
    ::printf("Running render system tests...\n");
    testCommandBufferRecording();
    testSpriteCmdFields();
    testTileCmdFields();
    testCommandBufferReset();
    testECSComponents();
    testTileMapBounds();
    testMultipleShapesWithColors();
    ::printf("All render tests passed.\n");
    return 0;
}
