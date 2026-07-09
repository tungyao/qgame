/**
 * Box2D Physics Test Suite
 */
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <vector>
#include "box2d/box2d.h"

static int gPass = 0;
static int gFail = 0;

static void check(const char* name, bool cond, const char* detail = nullptr) {
    if (cond) { std::printf("  PASS: %s\n", name); gPass++; }
    else { std::printf("  FAIL: %s", name); if (detail) std::printf("  \xe2\x80\x94 %s", detail); std::printf("\n"); gFail++; }
}

static void stepMany(b2WorldId wid, int n, float dt = 1.0f/60.0f) {
    for (int i = 0; i < n; ++i) b2World_Step(wid, dt, 4);
}

// ── Test 1: Rigid → Static (Box) ───────────────────────────────────
static void test_rigid_vs_static_box() {
    std::printf("\n\xe2\x94\x80\xe2\x94\x80 Test 1: Rigid \xe2\x86\x92 Static (Box) \xe2\x94\x80\xe2\x94\x80\n");

    b2WorldDef wd = b2DefaultWorldDef();
    b2WorldId wid = b2CreateWorld(&wd);

    b2BodyDef wallBd = b2DefaultBodyDef();
    wallBd.type = b2_staticBody;
    wallBd.position = {200.0f, 100.0f};
    b2BodyId wallId = b2CreateBody(wid, &wallBd);
    b2Polygon wallPoly = b2MakeBox(10.0f, 100.0f);
    b2ShapeDef wallSd1 = b2DefaultShapeDef();
    b2CreatePolygonShape(wallId, &wallSd1, &wallPoly);

    b2BodyDef ballBd = b2DefaultBodyDef();
    ballBd.type = b2_dynamicBody;
    ballBd.position = {100.0f, 100.0f};
    b2BodyId ballId = b2CreateBody(wid, &ballBd);
    b2Body_SetLinearVelocity(ballId, {300.0f, 0.0f});
    b2Polygon ballPoly = b2MakeBox(10.0f, 10.0f);
    b2ShapeDef ballSd = b2DefaultShapeDef();
    ballSd.density = 1.0f;
    b2CreatePolygonShape(ballId, &ballSd, &ballPoly);

    stepMany(wid, 40);
    b2Vec2 pos = b2Body_GetPosition(ballId);

    check("ball right edge at wall surface", pos.x + 10.0f <= 191.0f);
    check("ball NOT tunneled (x+10 >= 180)", pos.x + 10.0f >= 179.0f);
    b2Vec2 vel = b2Body_GetLinearVelocity(ballId);
    check("ball velocity near zero", std::abs(vel.x) < 15.0f);

    b2DestroyWorld(wid);
}

// ── Test 2: Rigid → Static (Circle) ────────────────────────────────
static void test_rigid_vs_static_circle() {
    std::printf("\n\xe2\x94\x80\xe2\x94\x80 Test 2: Rigid \xe2\x86\x92 Static (Circle) \xe2\x94\x80\xe2\x94\x80\n");

    b2WorldDef wd = b2DefaultWorldDef();
    b2WorldId wid = b2CreateWorld(&wd);

    b2BodyDef wallBd = b2DefaultBodyDef();
    wallBd.type = b2_staticBody;
    wallBd.position = {200.0f, 100.0f};
    b2BodyId wallId = b2CreateBody(wid, &wallBd);
    b2Polygon wallPoly = b2MakeBox(10.0f, 100.0f);
    b2ShapeDef wallSd2 = b2DefaultShapeDef();
    b2CreatePolygonShape(wallId, &wallSd2, &wallPoly);

    b2BodyDef ballBd = b2DefaultBodyDef();
    ballBd.type = b2_dynamicBody;
    ballBd.position = {100.0f, 100.0f};
    b2BodyId ballId = b2CreateBody(wid, &ballBd);
    b2Body_SetLinearVelocity(ballId, {300.0f, 0.0f});
    b2Circle circle = {{0.0f, 0.0f}, 10.0f};
    b2ShapeDef sd = b2DefaultShapeDef();
    sd.density = 1.0f;
    b2CreateCircleShape(ballId, &sd, &circle);

    stepMany(wid, 40);
    b2Vec2 pos = b2Body_GetPosition(ballId);
    check("circle at wall surface (x+10 <= 191)", pos.x + 10.0f <= 191.0f);
    check("circle NOT tunneled", pos.x + 10.0f >= 179.0f);
    b2Vec2 vel = b2Body_GetLinearVelocity(ballId);
    check("circle velocity near zero", std::abs(vel.x) < 1.0f);

    b2DestroyWorld(wid);
}

// ── Test 3: Static-Static no collisions ────────────────────────────
static void test_static_vs_static_noop() {
    std::printf("\n\xe2\x94\x80\xe2\x94\x80 Test 3: Static-Static (no pairs) \xe2\x94\x80\xe2\x94\x80\n");

    b2WorldDef wd = b2DefaultWorldDef();
    b2WorldId wid = b2CreateWorld(&wd);

    b2BodyDef bd = b2DefaultBodyDef();
    bd.type = b2_staticBody;

    bd.position = {100.0f, 100.0f};
    b2BodyId a = b2CreateBody(wid, &bd);
    b2Polygon pa = b2MakeBox(20.0f, 20.0f);
    b2ShapeDef sd_a = b2DefaultShapeDef();
    b2CreatePolygonShape(a, &sd_a, &pa);

    bd.position = {110.0f, 110.0f};
    b2BodyId b = b2CreateBody(wid, &bd);
    b2Polygon pb = b2MakeBox(20.0f, 20.0f);
    b2ShapeDef sd_b = b2DefaultShapeDef();
    b2CreatePolygonShape(b, &sd_b, &pb);

    b2World_Step(wid, 1.0f/60.0f, 4);
    b2ContactEvents ev = b2World_GetContactEvents(wid);
    check("no contacts between static bodies", ev.beginCount == 0 && ev.hitCount == 0);

    (void)a.index1; (void)b.index1;
    b2DestroyWorld(wid);
}

// ── Test 4: Rigid ↔ Rigid ──────────────────────────────────────────
static void test_rigid_vs_rigid() {
    std::printf("\n\xe2\x94\x80\xe2\x94\x80 Test 4: Rigid \xe2\x86\x94 Rigid \xe2\x94\x80\xe2\x94\x80\n");

    b2WorldDef wd = b2DefaultWorldDef();
    b2WorldId wid = b2CreateWorld(&wd);

    b2BodyDef bd = b2DefaultBodyDef();
    bd.type = b2_dynamicBody;
    b2ShapeDef sd = b2DefaultShapeDef();
    sd.density = 1.0f;

    bd.position = {100.0f, 100.0f};
    b2BodyId a = b2CreateBody(wid, &bd);
    b2Body_SetLinearVelocity(a, {150.0f, 0.0f});
    b2Polygon pa = b2MakeBox(10.0f, 10.0f);
    b2CreatePolygonShape(a, &sd, &pa);

    bd.position = {150.0f, 100.0f};
    b2BodyId b = b2CreateBody(wid, &bd);
    b2Body_SetLinearVelocity(b, {-150.0f, 0.0f});
    b2Polygon pb = b2MakeBox(10.0f, 10.0f);
    b2CreatePolygonShape(b, &sd, &pb);

    stepMany(wid, 30);

    b2Vec2 pa_pos = b2Body_GetPosition(a);
    b2Vec2 pb_pos = b2Body_GetPosition(b);
    check("bodies not overlapping", pa_pos.x + 10.0f <= pb_pos.x - 10.0f + 0.5f);

    b2DestroyWorld(wid);
}

// ── Test 5: Raycast ────────────────────────────────────────────────
struct RayTestResult { bool hit; float dist; };
static float rayCB(b2ShapeId shapeId, b2Vec2 point, b2Vec2 normal, float fraction, void* ctx) {
    auto* r = static_cast<RayTestResult*>(ctx);
    r->hit = true; r->dist = fraction * 500.0f; return fraction;
}

static void test_raycast() {
    std::printf("\n\xe2\x94\x80\xe2\x94\x80 Test 5: Raycast \xe2\x94\x80\xe2\x94\x80\n");

    b2WorldDef wd = b2DefaultWorldDef();
    b2WorldId wid = b2CreateWorld(&wd);

    b2BodyDef bd = b2DefaultBodyDef();
    bd.type = b2_staticBody;
    bd.position = {300.0f, 100.0f};
    b2BodyId target = b2CreateBody(wid, &bd);
    b2Polygon poly = b2MakeBox(10.0f, 10.0f);
    b2ShapeDef sd_target = b2DefaultShapeDef();
    b2CreatePolygonShape(target, &sd_target, &poly);

    b2QueryFilter filter = b2DefaultQueryFilter();

    // Ray right → hits target
    RayTestResult hit1{};
    b2World_CastRay(wid, {100.0f, 100.0f}, {500.0f, 0.0f}, filter, rayCB, &hit1);
    check("ray hits target", hit1.hit);

    // Ray left → miss
    RayTestResult hit2{};
    b2World_CastRay(wid, {100.0f, 100.0f}, {-500.0f, 0.0f}, filter, rayCB, &hit2);
    check("ray misses left", !hit2.hit);

    (void)target.index1;
    b2DestroyWorld(wid);
}

// ── Test 6: CollisionLayer filtering ────────────────────────────────
static void test_layer_filtering() {
    std::printf("\n\xe2\x94\x80\xe2\x94\x80 Test 6: Layer filtering \xe2\x94\x80\xe2\x94\x80\n");

    b2WorldDef wd = b2DefaultWorldDef();
    wd.gravity = {0.0f, 0.0f};
    b2WorldId wid = b2CreateWorld(&wd);

    // Dynamic body with PLAYER category and ENEMY-only mask, moving right
    b2BodyDef pbd = b2DefaultBodyDef();
    pbd.type = b2_dynamicBody;
    pbd.position = {95.0f, 100.0f};
    b2BodyId player = b2CreateBody(wid, &pbd);
    b2Body_SetLinearVelocity(player, {200.0f, 0.0f});
    b2ShapeDef psd = b2DefaultShapeDef();
    psd.density = 1.0f;
    psd.enableContactEvents = true;
    psd.filter.categoryBits = 4;  // PLAYER
    psd.filter.maskBits = 8;      // only ENEMY
    b2Polygon ppoly = b2MakeBox(10.0f, 10.0f);
    b2CreatePolygonShape(player, &psd, &ppoly);

    // Enemy static body — should collide with player (8 & 8 != 0)
    b2BodyDef ebd = b2DefaultBodyDef();
    ebd.type = b2_staticBody;
    ebd.position = {120.0f, 100.0f};
    b2BodyId enemy = b2CreateBody(wid, &ebd);
    b2ShapeDef esd = b2DefaultShapeDef();
    esd.filter.categoryBits = 8;   // ENEMY
    esd.filter.maskBits = UINT64_MAX;
    b2CreatePolygonShape(enemy, &esd, &ppoly);

    // Other PLAYER static body (same position) — should NOT collide (4 & 8 == 0)
    b2BodyDef obd = b2DefaultBodyDef();
    obd.type = b2_staticBody;
    obd.position = {120.0f, 100.0f};
    b2BodyId other = b2CreateBody(wid, &obd);
    b2ShapeDef osd = b2DefaultShapeDef();
    osd.filter.categoryBits = 4;   // PLAYER
    osd.filter.maskBits = UINT64_MAX;
    b2CreatePolygonShape(other, &osd, &ppoly);

    // Step while accumulating begin events; player moves right into static bodies
    int totalBegin = 0;
    for (int i = 0; i < 10; ++i) {
        b2World_Step(wid, 1.0f/60.0f, 4);
        b2ContactEvents ev = b2World_GetContactEvents(wid);
        totalBegin += ev.beginCount;
    }
    check("layer filter: contact events fired (enemy only)", totalBegin > 0);

    (void)other.index1; (void)enemy.index1; (void)player.index1;
    b2DestroyWorld(wid);
}

// ── Test 7: OverlapBox (AABB query) ────────────────────────────────
struct OverlapTestResult { std::vector<b2BodyId> bodies; };
static bool overlapCB(b2ShapeId shapeId, void* ctx) {
    auto* r = static_cast<OverlapTestResult*>(ctx);
    r->bodies.push_back(b2Shape_GetBody(shapeId));
    return true;
}

static void test_overlap_box() {
    std::printf("\n\xe2\x94\x80\xe2\x94\x80 Test 7: OverlapBox \xe2\x94\x80\xe2\x94\x80\n");

    b2WorldDef wd = b2DefaultWorldDef();
    b2WorldId wid = b2CreateWorld(&wd);

    b2BodyDef bd = b2DefaultBodyDef();
    bd.type = b2_staticBody;

    bd.position = {100.0f, 100.0f};
    b2BodyId a = b2CreateBody(wid, &bd);
    b2Polygon pa = b2MakeBox(20.0f, 20.0f);
    b2ShapeDef sd_a = b2DefaultShapeDef();
    b2CreatePolygonShape(a, &sd_a, &pa);

    bd.position = {200.0f, 100.0f};
    b2BodyId b = b2CreateBody(wid, &bd);
    b2Polygon pb = b2MakeBox(20.0f, 20.0f);
    b2ShapeDef sd_b = b2DefaultShapeDef();
    b2CreatePolygonShape(b, &sd_b, &pb);

    bd.position = {300.0f, 100.0f};
    b2BodyId c = b2CreateBody(wid, &bd);
    b2Polygon pc = b2MakeBox(20.0f, 20.0f);
    b2ShapeDef sd_c = b2DefaultShapeDef();
    b2CreatePolygonShape(c, &sd_c, &pc);

    b2AABB aabb;
    aabb.lowerBound = {80.0f, 70.0f};
    aabb.upperBound = {220.0f, 130.0f};

    b2QueryFilter filter = b2DefaultQueryFilter();
    OverlapTestResult result;
    b2World_OverlapAABB(wid, aabb, filter, overlapCB, &result);

    check("overlapBox returns 2 bodies", result.bodies.size() == 2);
    bool hasA = result.bodies[0].index1 == a.index1 || result.bodies[1].index1 == a.index1;
    bool hasB = result.bodies[0].index1 == b.index1 || result.bodies[1].index1 == b.index1;
    check("includes A", hasA);
    check("includes B", hasB);

    (void)c.index1;
    b2DestroyWorld(wid);
}

// ── main ───────────────────────────────────────────────────────────
int main() {
    std::printf("\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\n");
    std::printf("  Box2D Physics Test Suite\n");
    std::printf("\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\n");

    test_rigid_vs_static_box();
    test_rigid_vs_static_circle();
    test_static_vs_static_noop();
    test_rigid_vs_rigid();
    test_raycast();
    test_layer_filtering();
    test_overlap_box();

    std::printf("\n\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\n");
    std::printf("  Results: %d PASS, %d FAIL\n", gPass, gFail);
    std::printf("\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\n");

    return gFail > 0 ? 1 : 0;
}
