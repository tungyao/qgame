/**
 * PhysicsWorld2D 碰撞类型综合测试
 *
 * 测试覆盖：
 *   1.  Rigid → Static (Box)      2.  Rigid → Static (Circle)
 *   3.  Kinematic → Static         4.  Rigid ↔ Rigid
 *   5.  Kinematic → Rigid          6.  Trigger (Kinematic)
 *   7.  Trigger (Rigid)            8.  Static-Static no-op
 *   9.  CCD tunneling              10. Raycast
 *   11. OverlapBox                 12. OverlapCircle
 *   13. AABB centering             14. Contact Begin/Persist/End
 *   15. CollisionLayer filtering
 */

#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <vector>

#include <engine/systems/PhysicsWorld2D.h>

using namespace engine;

static int gPass = 0;
static int gFail = 0;

static void check(const char* name, bool cond, const char* detail = nullptr) {
    if (cond) {
        std::printf("  PASS: %s\n", name);
        gPass++;
    } else {
        std::printf("  FAIL: %s", name);
        if (detail) std::printf("  — %s", detail);
        std::printf("\n");
        gFail++;
    }
}

// ── Body 工厂 ──────────────────────────────────────────────────────────

static PhysicsWorld2D::BodyId makeStaticBox(PhysicsWorld2D& world,
    float x, float y, float w, float h,
    CollisionLayer layer = COLLISION_LAYER_STATIC,
    CollisionLayer mask = COLLISION_LAYER_ALL)
{
    PhysicsWorld2D::BodyDef bd;
    bd.type = BodyType::Static; bd.x = x; bd.y = y;
    auto id = world.createBody(bd);
    PhysicsWorld2D::ShapeDef sd;
    sd.shapeType = ShapeType::Box; sd.width = w; sd.height = h;
    sd.layer = layer; sd.mask = mask;
    world.addShape(id, sd);
    return id;
}

static PhysicsWorld2D::BodyId makeRigidBox(PhysicsWorld2D& world,
    float x, float y, float w, float h,
    float vx = 0.f, float vy = 0.f,
    CollisionLayer layer = COLLISION_LAYER_DEFAULT,
    CollisionLayer mask = COLLISION_LAYER_ALL,
    bool ccd = false)
{
    PhysicsWorld2D::BodyDef bd;
    bd.type = BodyType::Rigid; bd.x = x; bd.y = y;
    bd.velocityX = vx; bd.velocityY = vy;
    bd.mass = 1.f; bd.gravityScale = 0.f; bd.ccdEnabled = ccd;
    auto id = world.createBody(bd);
    PhysicsWorld2D::ShapeDef sd;
    sd.shapeType = ShapeType::Box; sd.width = w; sd.height = h;
    sd.layer = layer; sd.mask = mask;
    world.addShape(id, sd);
    return id;
}

static PhysicsWorld2D::BodyId makeKinematic(PhysicsWorld2D& world,
    float x, float y, float w, float h,
    CollisionLayer layer = COLLISION_LAYER_PLAYER,
    CollisionLayer mask = COLLISION_LAYER_ALL)
{
    PhysicsWorld2D::BodyDef bd;
    bd.type = BodyType::Kinematic; bd.x = x; bd.y = y; bd.mass = 1.f;
    auto id = world.createBody(bd);
    PhysicsWorld2D::ShapeDef sd;
    sd.shapeType = ShapeType::Box; sd.width = w; sd.height = h;
    sd.layer = layer; sd.mask = mask;
    world.addShape(id, sd);
    return id;
}

static PhysicsWorld2D::BodyId makeTrigger(PhysicsWorld2D& world,
    float x, float y, float w, float h,
    CollisionLayer layer = COLLISION_LAYER_ENEMY,
    CollisionLayer mask = COLLISION_LAYER_ALL)
{
    PhysicsWorld2D::BodyDef bd;
    bd.type = BodyType::Static; bd.x = x; bd.y = y;
    auto id = world.createBody(bd);
    PhysicsWorld2D::ShapeDef sd;
    sd.shapeType = ShapeType::Box; sd.width = w; sd.height = h;
    sd.isTrigger = true; sd.layer = layer; sd.mask = mask;
    world.addShape(id, sd);
    return id;
}

static PhysicsWorld2D::BodyId makeRigidCircle(PhysicsWorld2D& world,
    float x, float y, float r, float vx = 0.f, float vy = 0.f)
{
    PhysicsWorld2D::BodyDef bd;
    bd.type = BodyType::Rigid; bd.x = x; bd.y = y;
    bd.velocityX = vx; bd.velocityY = vy;
    bd.mass = 1.f; bd.gravityScale = 0.f;
    auto id = world.createBody(bd);
    PhysicsWorld2D::ShapeDef sd;
    sd.shapeType = ShapeType::Circle; sd.radius = r;
    sd.layer = COLLISION_LAYER_DEFAULT; sd.mask = COLLISION_LAYER_ALL;
    world.addShape(id, sd);
    return id;
}

// ── 辅助：多步模拟 ────────────────────────────────────────────────────

static void stepMany(PhysicsWorld2D& world, int n, float dt = 1.f/60.f) {
    for (int i = 0; i < n; ++i) world.step(dt);
}

// ═══════════════════════════════════════════════════════════════════════════
// Test 1: Rigid → Static (Box)
// ═══════════════════════════════════════════════════════════════════════════
static void test_rigid_vs_static_box() {
    std::printf("\n── Test 1: Rigid → Static (Box) ──\n");

    PhysicsWorld2D world;
    world.setGravity(0.f, 0.f);

    // Wall centered at (200,100), 20x200 → AABB {190, 0, 210, 200}
    makeStaticBox(world, 200.f, 100.f, 20.f, 200.f);

    // Ball at x=100, vx=300. At 60fps, moves 5px/frame. Needs ~18 frames to reach wall.
    auto ball = makeRigidBox(world, 100.f, 100.f, 20.f, 20.f, 300.f, 0.f);

    // Step enough times for ball to reach the wall and be stopped
    stepMany(world, 40);

    float x, y, vx, vy;
    world.getBodyState(ball, x, y, vx, vy);

    // Ball AABB = {x-10, y-10, x+10, y+10}. Wall AABB = {190, 0, 210, 200}
    // After collision, ball right edge (x+10) must be <= wall left edge (190)
    check("ball right edge at wall surface", x + 10.f <= 190.5f);
    check("ball NOT tunneled (x+10 >= 180)", x + 10.f >= 179.f);
    check("ball velocity zeroed (vx ~ 0)", std::abs(vx) < 1.f);

    world.clearAllBodies();
}

// ═══════════════════════════════════════════════════════════════════════════
// Test 2: Rigid → Static (Circle vs Box)
// ═══════════════════════════════════════════════════════════════════════════
static void test_rigid_vs_static_circle() {
    std::printf("\n── Test 2: Rigid → Static (Circle vs Box) ──\n");

    PhysicsWorld2D world;
    world.setGravity(0.f, 0.f);

    makeStaticBox(world, 200.f, 100.f, 20.f, 200.f);

    // Circle radius=10, moving right
    auto ball = makeRigidCircle(world, 100.f, 100.f, 10.f, 300.f, 0.f);

    stepMany(world, 40);

    float x, y, vx, vy;
    world.getBodyState(ball, x, y, vx, vy);

    check("circle at wall surface (x+10 <= 190.5)", x + 10.f <= 190.5f);
    check("circle NOT tunneled", x + 10.f >= 179.f);
    check("circle velocity zeroed", std::abs(vx) < 1.f);

    world.clearAllBodies();
}

// ═══════════════════════════════════════════════════════════════════════════
// Test 3: Kinematic → Static
// ═══════════════════════════════════════════════════════════════════════════
static void test_kinematic_vs_static() {
    std::printf("\n── Test 3: Kinematic → Static ──\n");

    PhysicsWorld2D world;
    world.setGravity(0.f, 0.f);

    // Wall at x=200, 20x200 → AABB {190, 0, 210, 200}
    makeStaticBox(world, 200.f, 100.f, 20.f, 200.f);

    // Kinematic player moving right toward wall
    auto player = makeKinematic(world, 100.f, 100.f, 28.f, 28.f);
    world.setBodyVelocity(player, 200.f, 0.f);

    int contactCount = 0;
    world.setContactCallback([&](const PhysicsWorld2D::CollisionPair&) {
        contactCount++;
    });

    stepMany(world, 40);

    float x, y, vx, vy;
    world.getBodyState(player, x, y, vx, vy);

    // Kinematic must be pushed out: right edge (x+14) <= wall left (190)
    check("kinematic right edge at wall", x + 14.f <= 190.5f);
    check("kinematic NOT tunneled", x + 14.f >= 180.f);
    check("contact callback fired", contactCount > 0);

    world.clearAllBodies();
}

// ═══════════════════════════════════════════════════════════════════════════
// Test 4: Rigid ↔ Rigid
// ═══════════════════════════════════════════════════════════════════════════
static void test_rigid_vs_rigid() {
    std::printf("\n── Test 4: Rigid ↔ Rigid ──\n");

    PhysicsWorld2D world;
    world.setGravity(0.f, 0.f);

    // Two 20x20 balls approaching each other
    auto a = makeRigidBox(world, 100.f, 100.f, 20.f, 20.f, 150.f, 0.f);
    auto b = makeRigidBox(world, 150.f, 100.f, 20.f, 20.f, -150.f, 0.f);

    stepMany(world, 30);

    float ax, ay, avx, avy, bx, by, bvx, bvy;
    world.getBodyState(a, ax, ay, avx, avy);
    world.getBodyState(b, bx, by, bvx, bvy);

    // After collision: A moved left, B moved right. Not overlapping.
    // Solver is inelastic: velocity along normal is zeroed, not reflected.
    check("balls not overlapping", ax + 10.f <= bx - 10.f + 0.5f);
    check("A velocity zeroed (avx ~ 0)", std::abs(avx) < 1.f);
    check("B velocity zeroed (bvx ~ 0)", std::abs(bvx) < 1.f);

    world.clearAllBodies();
}

// ═══════════════════════════════════════════════════════════════════════════
// Test 5: Kinematic → Rigid (push)
// ═══════════════════════════════════════════════════════════════════════════
static void test_kinematic_vs_rigid() {
    std::printf("\n── Test 5: Kinematic → Rigid ──\n");

    PhysicsWorld2D world;
    world.setGravity(0.f, 0.f);

    // Kinematic player moving right
    auto player = makeKinematic(world, 100.f, 100.f, 28.f, 28.f);
    world.setBodyVelocity(player, 200.f, 0.f);

    // Rigid ball at rest, in front of player
    auto ball = makeRigidBox(world, 140.f, 100.f, 20.f, 20.f, 0.f, 0.f);

    stepMany(world, 30);

    float px, py, pvx, pvy, bx, by, bvx, bvy;
    world.getBodyState(player, px, py, pvx, pvy);
    world.getBodyState(ball, bx, by, bvx, bvy);

    // Ball must be pushed right (position separation) by kinematic player.
    // Solver pushes rigid by position only; no momentum transfer to velocity.
    check("ball pushed right from start (bx > 140)", bx > 140.f);
    check("player and ball not overlapping", px + 14.f <= bx - 10.f + 0.5f);

    world.clearAllBodies();
}

// ═══════════════════════════════════════════════════════════════════════════
// Test 6: Trigger (Kinematic enters)
// ═══════════════════════════════════════════════════════════════════════════
static void test_trigger_kinematic() {
    std::printf("\n── Test 6: Trigger (Kinematic enters) ──\n");

    PhysicsWorld2D world;
    world.setGravity(0.f, 0.f);

    // Pickup trigger at (200, 100), 20x20 → AABB {190, 90, 210, 110}
    auto pickup = makeTrigger(world, 200.f, 100.f, 20.f, 20.f);

    // Kinematic player starting left, moving right through trigger
    auto player = makeKinematic(world, 100.f, 100.f, 28.f, 28.f);
    world.setBodyVelocity(player, 200.f, 0.f);

    int contactCount = 0;
    bool isTriggerPair = false;
    world.setContactCallback([&](const PhysicsWorld2D::CollisionPair& pair) {
        contactCount++;
        if (pair.isTriggerPair) isTriggerPair = true;
    });

    stepMany(world, 50);

    check("trigger contact fired (kinematic)", contactCount > 0);
    check("pair marked as trigger", isTriggerPair);

    // Kinematic should pass through trigger (not pushed away)
    float px, py, pvx, pvy;
    world.getBodyState(player, px, py, pvx, pvy);
    check("kinematic passes through trigger (px > 210)", px > 210.f);

    world.clearAllBodies();
}

// ═══════════════════════════════════════════════════════════════════════════
// Test 7: Trigger (Rigid enters)
// ═══════════════════════════════════════════════════════════════════════════
static void test_trigger_rigid() {
    std::printf("\n── Test 7: Trigger (Rigid enters) ──\n");

    PhysicsWorld2D world;
    world.setGravity(0.f, 0.f);

    auto pickup = makeTrigger(world, 200.f, 100.f, 20.f, 20.f);

    auto ball = makeRigidBox(world, 100.f, 100.f, 20.f, 20.f, 300.f, 0.f);

    int contactCount = 0;
    bool isTriggerPair = false;
    world.setContactCallback([&](const PhysicsWorld2D::CollisionPair& pair) {
        contactCount++;
        if (pair.isTriggerPair) isTriggerPair = true;
    });

    stepMany(world, 40);

    check("trigger contact fired (rigid)", contactCount > 0);
    check("pair marked as trigger", isTriggerPair);

    // Rigid should pass through trigger
    float bx, by, bvx, bvy;
    world.getBodyState(ball, bx, by, bvx, bvy);
    check("rigid passes through trigger (bx > 215)", bx > 215.f);

    world.clearAllBodies();
}

// ═══════════════════════════════════════════════════════════════════════════
// Test 8: Static-Static — no pairs
// ═══════════════════════════════════════════════════════════════════════════
static void test_static_vs_static_noop() {
    std::printf("\n── Test 8: Static-Static (no pairs) ──\n");

    PhysicsWorld2D world;

    makeStaticBox(world, 100.f, 100.f, 40.f, 40.f);
    makeStaticBox(world, 110.f, 110.f, 40.f, 40.f);

    int contactCount = 0;
    world.setContactCallback([&](const PhysicsWorld2D::CollisionPair&) {
        contactCount++;
    });

    world.step(1.f / 60.f);

    check("no contacts between static bodies", contactCount == 0);

    world.clearAllBodies();
}

// ═══════════════════════════════════════════════════════════════════════════
// Test 9: CCD tunneling prevention
// ═══════════════════════════════════════════════════════════════════════════
static void test_ccd_tunneling() {
    std::printf("\n── Test 9: CCD tunneling prevention ──\n");

    PhysicsWorld2D world;
    world.setGravity(0.f, 0.f);

    // Thin wall (10px) at x=300 → AABB {295, 0, 305, 200}
    makeStaticBox(world, 300.f, 100.f, 10.f, 200.f);

    // Fast ball: vx=15000, at 1/60s dt moves 250px/frame.
    // Starts at x=100 (AABB {90,90,110,110}), would land at x=350 without CCD.
    auto ball = makeRigidBox(world, 100.f, 100.f, 20.f, 20.f, 15000.f, 0.f,
                             COLLISION_LAYER_DEFAULT, COLLISION_LAYER_ALL, true);

    world.step(1.f / 60.f);

    float x, y, vx, vy;
    world.getBodyState(ball, x, y, vx, vy);

    // CCD must stop ball at wall surface
    check("CCD: ball right edge at wall surface (x+10 <= 296)", x + 10.f <= 296.f);
    check("CCD: ball not bounced too far left (x+10 >= 280)", x + 10.f >= 280.f);

    world.clearAllBodies();
}

// ═══════════════════════════════════════════════════════════════════════════
// Test 10: Raycast
// ═══════════════════════════════════════════════════════════════════════════
static void test_raycast() {
    std::printf("\n── Test 10: Raycast ──\n");

    PhysicsWorld2D world;

    auto target = makeStaticBox(world, 300.f, 100.f, 20.f, 20.f,
                                COLLISION_LAYER_ENEMY, COLLISION_LAYER_ALL);

    // Ray right → hits target at ~290 (box left edge = 300-10 = 290)
    auto hit = world.raycast(100.f, 100.f, 1.f, 0.f, 500.f);
    check("ray hits target", hit.hit && hit.bodyId == target);
    check("ray distance ~190", std::abs(hit.distance - 190.f) < 5.f);

    // Ray ignores ENEMY layer → miss
    auto hit2 = world.raycast(100.f, 100.f, 1.f, 0.f, 500.f,
                              COLLISION_LAYER_ALL, COLLISION_LAYER_ENEMY);
    check("ray ignores ENEMY layer", !hit2.hit);

    // Ray left → miss
    auto hit3 = world.raycast(100.f, 100.f, -1.f, 0.f, 500.f);
    check("ray misses left", !hit3.hit);

    // Ray ignores specific body → miss
    auto hit4 = world.raycast(100.f, 100.f, 1.f, 0.f, 500.f,
                              COLLISION_LAYER_ALL, 0, target);
    check("ray ignores specific body", !hit4.hit);

    world.clearAllBodies();
}

// ═══════════════════════════════════════════════════════════════════════════
// Test 11: OverlapBox
// ═══════════════════════════════════════════════════════════════════════════
static void test_overlap_box() {
    std::printf("\n── Test 11: OverlapBox ──\n");

    PhysicsWorld2D world;

    auto a = makeStaticBox(world, 100.f, 100.f, 40.f, 40.f);
    auto b = makeStaticBox(world, 200.f, 100.f, 40.f, 40.f);
    auto c = makeStaticBox(world, 300.f, 100.f, 40.f, 40.f);
    (void)c;

    // Query rect center (150,100), halfW=70, halfH=30 → x=80..220, y=70..130
    // A AABB={80,80,120,120} ✓  B AABB={180,80,220,120} ✓  C AABB={280,80,320,120} ✗
    auto results = world.overlapRect(150.f, 100.f, 70.f, 30.f);

    check("overlapBox returns 2 bodies", results.size() == 2);

    bool hasA = false, hasB = false, hasC = false;
    for (auto& r : results) {
        if (r.bodyId == a) hasA = true;
        if (r.bodyId == b) hasB = true;
        if (r.bodyId == c) hasC = true;
    }
    check("overlapBox includes A", hasA);
    check("overlapBox includes B", hasB);
    check("overlapBox excludes C", !hasC);

    world.clearAllBodies();
}

// ═══════════════════════════════════════════════════════════════════════════
// Test 12: OverlapCircle
// ═══════════════════════════════════════════════════════════════════════════
static void test_overlap_circle() {
    std::printf("\n── Test 12: OverlapCircle ──\n");

    PhysicsWorld2D world;

    auto a = makeStaticBox(world, 100.f, 100.f, 20.f, 20.f);
    auto b = makeStaticBox(world, 150.f, 100.f, 20.f, 20.f);
    auto c = makeStaticBox(world, 300.f, 100.f, 20.f, 20.f);
    (void)c;

    // Circle at (125, 100), r=40 → covers a (dist=25) and b (dist=25), not c
    auto results = world.overlapCircle(125.f, 100.f, 40.f);

    check("overlapCircle returns 2 bodies", results.size() == 2);

    bool hasA = false, hasB = false, hasC = false;
    for (auto id : results) {
        if (id == a) hasA = true;
        if (id == b) hasB = true;
        if (id == c) hasC = true;
    }
    check("overlapCircle includes A", hasA);
    check("overlapCircle includes B", hasB);
    check("overlapCircle excludes C", !hasC);

    world.clearAllBodies();
}

// ═══════════════════════════════════════════════════════════════════════════
// Test 13: AABB centering
// ═══════════════════════════════════════════════════════════════════════════
static void test_aabb_centering() {
    std::printf("\n── Test 13: AABB centering ──\n");

    PhysicsWorld2D world;

    // Box at (200, 150), size 40x30 → centered AABB = {180, 135, 220, 165}
    {
        PhysicsWorld2D::BodyDef bd;
        bd.type = BodyType::Static; bd.x = 200.f; bd.y = 150.f;
        auto id = world.createBody(bd);
        PhysicsWorld2D::ShapeDef sd;
        sd.shapeType = ShapeType::Box; sd.width = 40.f; sd.height = 30.f;
        world.addShape(id, sd);

        world.step(0.f);

        auto hit = world.raycast(100.f, 150.f, 1.f, 0.f, 500.f);
        // Ray starts at x=100, should hit box left edge at x=180 → dist=80
        check("Box AABB centered: left edge at ~180",
              hit.hit && std::abs(hit.distance - 80.f) < 5.f);
    }

    world.clearAllBodies();

    // Circle at (200, 150), r=20 → centered AABB = {180, 130, 220, 170}
    {
        PhysicsWorld2D::BodyDef bd;
        bd.type = BodyType::Static; bd.x = 200.f; bd.y = 150.f;
        auto id = world.createBody(bd);
        PhysicsWorld2D::ShapeDef sd;
        sd.shapeType = ShapeType::Circle; sd.radius = 20.f;
        world.addShape(id, sd);

        world.step(0.f);

        auto hit = world.raycast(100.f, 150.f, 1.f, 0.f, 500.f);
        check("Circle AABB centered: left edge at ~180",
              hit.hit && std::abs(hit.distance - 80.f) < 5.f);
    }

    world.clearAllBodies();
}

// ═══════════════════════════════════════════════════════════════════════════
// Test 14: Contact lifecycle (Begin → Persist → End)
// ═══════════════════════════════════════════════════════════════════════════
static void test_contact_lifecycle() {
    std::printf("\n── Test 14: Contact lifecycle ──\n");

    PhysicsWorld2D world;
    // Gravity pushes ball down onto floor, creating continuous overlap → Persist
    world.setGravity(0.f, 500.f);

    // Floor at y=200, 200x20 → AABB {100, 190, 300, 210}
    makeStaticBox(world, 200.f, 200.f, 200.f, 20.f);

    struct Event { ContactState state; };
    std::vector<Event> events;
    world.setContactCallback([&](const PhysicsWorld2D::CollisionPair& pair) {
        events.push_back({pair.state});
    });

    // Ball above floor, falls under gravity
    auto ball = makeRigidBox(world, 160.f, 100.f, 20.f, 20.f, 0.f, 0.f);
    world.setBodyGravityScale(ball, 1.f);

    // Step until ball lands on floor → Begin
    stepMany(world, 40);
    bool hasBegin = false;
    for (auto& e : events) { if (e.state == ContactState::Begin) hasBegin = true; }

    // Continue stepping while resting on floor → Persist (gravity re-overlaps each step)
    events.clear();
    stepMany(world, 10);
    bool hasPersist = false;
    for (auto& e : events) { if (e.state == ContactState::Persist) hasPersist = true; }

    // Remove gravity and move ball up → End
    world.setGravity(0.f, 0.f);
    world.setBodyVelocity(ball, 0.f, -300.f);
    events.clear();
    stepMany(world, 15);

    bool hasEnd = false;
    for (auto& e : events) { if (e.state == ContactState::End) hasEnd = true; }

    check("Begin event fired", hasBegin);
    check("Persist event fired", hasPersist);
    check("End event fired", hasEnd);

    world.clearAllBodies();
}

// ═══════════════════════════════════════════════════════════════════════════
// Test 15: CollisionLayer filtering
// ═══════════════════════════════════════════════════════════════════════════
static void test_layer_filtering() {
    std::printf("\n── Test 15: CollisionLayer filtering ──\n");

    PhysicsWorld2D world;
    world.setGravity(0.f, 0.f);

    // Kinematic player, mask=ENEMY only
    auto player = makeKinematic(world, 100.f, 100.f, 20.f, 20.f,
                                COLLISION_LAYER_PLAYER, COLLISION_LAYER_ENEMY);

    // Enemy static at same position — should collide (PLAYER layer & ENEMY mask)
    auto enemy = makeStaticBox(world, 100.f, 100.f, 20.f, 20.f,
                               COLLISION_LAYER_ENEMY, COLLISION_LAYER_ALL);

    // Other player static at same position — should NOT collide
    // (PLAYER layer & PLAYER mask → PLAYER & ENEMY mask = 0)
    auto otherPlayer = makeStaticBox(world, 100.f, 100.f, 20.f, 20.f,
                                     COLLISION_LAYER_PLAYER, COLLISION_LAYER_ALL);

    int contactCount = 0;
    world.setContactCallback([&](const PhysicsWorld2D::CollisionPair&) {
        contactCount++;
    });

    world.step(1.f / 60.f);

    // Only the enemy (COLLISION_LAYER_ENEMY=8) should collide.
    // Player's mask = ENEMY(8), so player-layer (4) body should NOT collide.
    check("layer filter: exactly 1 contact (enemy only)", contactCount == 1);

    (void)otherPlayer; (void)enemy; (void)player;
    world.clearAllBodies();
}

// ═══════════════════════════════════════════════════════════════════════════
// main
// ═══════════════════════════════════════════════════════════════════════════
int main() {
    std::printf("══════════════════════════════════════════\n");
    std::printf("  PhysicsWorld2D Collision Test Suite\n");
    std::printf("══════════════════════════════════════════\n");

    test_rigid_vs_static_box();
    test_rigid_vs_static_circle();
    test_kinematic_vs_static();
    test_rigid_vs_rigid();
    test_kinematic_vs_rigid();
    test_trigger_kinematic();
    test_trigger_rigid();
    test_static_vs_static_noop();
    test_ccd_tunneling();
    test_raycast();
    test_overlap_box();
    test_overlap_circle();
    test_aabb_centering();
    test_contact_lifecycle();
    test_layer_filtering();

    std::printf("\n══════════════════════════════════════════\n");
    std::printf("  Results: %d PASS, %d FAIL\n", gPass, gFail);
    std::printf("══════════════════════════════════════════\n");

    return gFail > 0 ? 1 : 0;
}
