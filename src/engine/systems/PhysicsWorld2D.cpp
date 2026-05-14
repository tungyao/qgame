#include "PhysicsWorld2D.h"
#include <algorithm>
#include <cmath>

namespace engine {

// ── AABB utilities ──────────────────────────────────────────────────────────

PhysicsWorld2D::AABB PhysicsWorld2D::computeAABB(
    float bodyX, float bodyY,
    const ShapeDef& shape,
    const float* spriteSrcW, const float* spriteSrcH,
    float scaleX, float scaleY,
    float pivotX, float pivotY)
{
    float ox = bodyX;
    float oy = bodyY;
    bool centered = true;  // bodyX/bodyY is the center

    if (spriteSrcW && spriteSrcH) {
        float sw = *spriteSrcW * std::abs(scaleX);
        float sh = *spriteSrcH * std::abs(scaleY);
        // Convert center → top-left (ox,oy is now top-left of sprite rect)
        ox = bodyX - pivotX * sw;
        oy = bodyY - pivotY * sh;
        centered = false;
    }

    ox += shape.offsetX;
    oy += shape.offsetY;

    switch (shape.shapeType) {
    case ShapeType::Box:
        if (centered)
            return { ox - shape.width * 0.5f, oy - shape.height * 0.5f,
                     ox + shape.width * 0.5f, oy + shape.height * 0.5f };
        return { ox, oy, ox + shape.width, oy + shape.height };
    case ShapeType::Circle: {
        float r = shape.radius;
        if (centered)
            return { ox - r, oy - r, ox + r, oy + r };
        float cx = ox + r;
        float cy = oy + r;
        return { cx - r, cy - r, cx + r, cy + r };
    }
    case ShapeType::Capsule: {
        float r = shape.radius;
        float len = shape.capsuleLength;
        if (centered) {
            float hh = r + len * 0.5f;
            return { ox - r, oy - hh, ox + r, oy + hh };
        }
        float hw = r;
        float hh = r + len * 0.5f;
        float cx = ox + r;
        float cy = oy + r;
        return { cx - hw, cy - hh, cx + hw, cy + hh };
    }
    }
    if (centered)
        return { ox - shape.width * 0.5f, oy - shape.height * 0.5f,
                 ox + shape.width * 0.5f, oy + shape.height * 0.5f };
    return { ox, oy, ox + shape.width, oy + shape.height };
}

bool PhysicsWorld2D::overlaps(const AABB& a, const AABB& b) {
    return a.minX < b.maxX && a.maxX > b.minX &&
           a.minY < b.maxY && a.maxY > b.minY;
}

void PhysicsWorld2D::minSeparation(const AABB& a, const AABB& b,
                                     float& outX, float& outY) {
    float overlapX = std::min(a.maxX, b.maxX) - std::max(a.minX, b.minX);
    float overlapY = std::min(a.maxY, b.maxY) - std::max(a.minY, b.minY);

    if (overlapX < overlapY) {
        float aCx = (a.minX + a.maxX) * 0.5f;
        float bCx = (b.minX + b.maxX) * 0.5f;
        outX = (aCx < bCx) ? -overlapX : overlapX;
        outY = 0.f;
    } else {
        float aCy = (a.minY + a.maxY) * 0.5f;
        float bCy = (b.minY + b.maxY) * 0.5f;
        outX = 0.f;
        outY = (aCy < bCy) ? -overlapY : overlapY;
    }
}

bool PhysicsWorld2D::sweptAABBvsAABB_(const AABB& start, float dx, float dy,
                                      const AABB& target, float& outTime) {
    float mw = (start.maxX - start.minX) * 0.5f;
    float mh = (start.maxY - start.minY) * 0.5f;
    float cx = (start.minX + start.maxX) * 0.5f;
    float cy = (start.minY + start.maxY) * 0.5f;

    float eMinX = target.minX - mw, eMaxX = target.maxX + mw;
    float eMinY = target.minY - mh, eMaxY = target.maxY + mh;

    float tMin = 0.f, tMax = 1.f;

    auto slab = [&](float p, float d, float lo, float hi) -> bool {
        if (std::abs(d) < 0.0001f) return (p >= lo && p <= hi);
        float t1 = (lo - p) / d, t2 = (hi - p) / d;
        if (t1 > t2) std::swap(t1, t2);
        tMin = std::max(tMin, t1);
        tMax = std::min(tMax, t2);
        return tMin <= tMax;
    };

    if (!slab(cx, dx, eMinX, eMaxX)) return false;
    if (!slab(cy, dy, eMinY, eMaxY)) return false;

    outTime = tMin;
    return (tMin >= 0.f && tMin <= 1.f);
}

// ── Narrow phase ────────────────────────────────────────────────────────────

bool PhysicsWorld2D::boxVsBox_(const AABB& a, const AABB& b,
                                float& nx, float& ny, float& overlap) {
    if (!overlaps(a, b)) return false;

    float overlapX = std::min(a.maxX, b.maxX) - std::max(a.minX, b.minX);
    float overlapY = std::min(a.maxY, b.maxY) - std::max(a.minY, b.minY);

    if (overlapX < overlapY) {
        overlap = overlapX;
        float aCx = (a.minX + a.maxX) * 0.5f;
        float bCx = (b.minX + b.maxX) * 0.5f;
        nx = (aCx < bCx) ? 1.f : -1.f;
        ny = 0.f;
    } else {
        overlap = overlapY;
        float aCy = (a.minY + a.maxY) * 0.5f;
        float bCy = (b.minY + b.maxY) * 0.5f;
        nx = 0.f;
        ny = (aCy < bCy) ? 1.f : -1.f;
    }
    return true;
}

bool PhysicsWorld2D::boxVsCircle_(const AABB& box,
                                   float cx, float cy, float r,
                                   float& nx, float& ny, float& overlap) {
    // 找到圆上距离盒子最近的点
    float closestX = std::max(box.minX, std::min(cx, box.maxX));
    float closestY = std::max(box.minY, std::min(cy, box.maxY));
    float dx = cx - closestX;
    float dy = cy - closestY;
    float distSq = dx * dx + dy * dy;

    if (distSq >= r * r) return false;

    float dist = std::sqrt(distSq);
    if (dist < 0.0001f) {
        // 圆心在盒子内部，沿最短轴推出
        float toLeft = cx - box.minX;
        float toRight = box.maxX - cx;
        float toTop = cy - box.minY;
        float toBottom = box.maxY - cy;
        float minDist = std::min({toLeft, toRight, toTop, toBottom});
        if (minDist == toLeft)      { nx =  1.f; ny = 0.f; overlap = r + toLeft; }
        else if (minDist == toRight){ nx = -1.f; ny = 0.f; overlap = r + toRight; }
        else if (minDist == toTop)  { nx = 0.f; ny =  1.f; overlap = r + toTop; }
        else                        { nx = 0.f; ny = -1.f; overlap = r + toBottom; }
        return true;
    }

    overlap = r - dist;
    nx = dx / dist;
    ny = dy / dist;
    return true;
}

bool PhysicsWorld2D::circleVsCircle_(float ax, float ay, float ar,
                                      float bx, float by, float br,
                                      float& nx, float& ny, float& overlap) {
    float dx = bx - ax;
    float dy = by - ay;
    float distSq = dx * dx + dy * dy;
    float rSum = ar + br;

    if (distSq >= rSum * rSum) return false;

    float dist = std::sqrt(distSq);
    if (dist < 0.0001f) {
        nx = 0.f; ny = 1.f;
        overlap = rSum;
        return true;
    }

    overlap = rSum - dist;
    nx = dx / dist;
    ny = dy / dist;
    return true;
}

// ── Lifecycle ───────────────────────────────────────────────────────────────

PhysicsWorld2D::PhysicsWorld2D()  = default;
PhysicsWorld2D::~PhysicsWorld2D() = default;

// ── World config ────────────────────────────────────────────────────────────

void PhysicsWorld2D::setGravity(float x, float y)       { gravityX_ = x; gravityY_ = y; }
void PhysicsWorld2D::setIterationCount(int count)       { iterationCount_ = std::max(1, count); }
void PhysicsWorld2D::setContactMargin(float margin)     { contactMargin_ = std::max(0.f, margin); }
void PhysicsWorld2D::setBroadphaseCellSize(float size)  { broadphase_ = SpatialHashGrid<BodyId>(size); }

// ── Body management ─────────────────────────────────────────────────────────

PhysicsWorld2D::BodyId PhysicsWorld2D::createBody(const BodyDef& def) {
    BodyId id = nextBodyId_++;
    Body body;
    body.type         = def.type;
    body.x            = def.x;
    body.y            = def.y;
    body.velocityX    = def.velocityX;
    body.velocityY    = def.velocityY;
    body.gravityScale = def.gravityScale;
    body.mass         = def.mass;
    body.invMass      = (def.type == BodyType::Rigid && def.mass > 0.f) ? (1.f / def.mass) : 0.f;
    body.bounciness   = def.bounciness;
    body.friction     = def.friction;
    body.ccdEnabled   = def.ccdEnabled;
    body.contactMargin = def.contactMargin;
    bodies_[id] = std::move(body);
    return id;
}

void PhysicsWorld2D::destroyBody(BodyId id) {
    bodies_.erase(id);
}

void PhysicsWorld2D::clearAllBodies() {
    bodies_.clear();
    nextBodyId_ = 1;
    prevFrameContacts_.clear();
    lastContactNormals_.clear();
    ccdBuffer_.clear();
}

void PhysicsWorld2D::setBodyTransform(BodyId id, float x, float y) {
    auto it = bodies_.find(id);
    if (it != bodies_.end()) { it->second.x = x; it->second.y = y; }
}

void PhysicsWorld2D::setBodyVelocity(BodyId id, float vx, float vy) {
    auto it = bodies_.find(id);
    if (it != bodies_.end()) { it->second.velocityX = vx; it->second.velocityY = vy; }
}

void PhysicsWorld2D::setBodyType(BodyId id, BodyType type) {
    auto it = bodies_.find(id);
    if (it != bodies_.end()) {
        it->second.type = type;
        it->second.invMass = (type == BodyType::Rigid && it->second.mass > 0.f)
                             ? (1.f / it->second.mass) : 0.f;
    }
}

void PhysicsWorld2D::setBodyGravityScale(BodyId id, float scale) {
    auto it = bodies_.find(id);
    if (it != bodies_.end()) it->second.gravityScale = scale;
}

void PhysicsWorld2D::setBodyMass(BodyId id, float mass) {
    auto it = bodies_.find(id);
    if (it != bodies_.end()) {
        it->second.mass = mass;
        it->second.invMass = (it->second.type == BodyType::Rigid && mass > 0.f)
                             ? (1.f / mass) : 0.f;
    }
}

BodyType PhysicsWorld2D::bodyType(BodyId id) const {
    auto it = bodies_.find(id);
    return it != bodies_.end() ? it->second.type : BodyType::Static;
}

bool PhysicsWorld2D::isBodyValid(BodyId id) const {
    return bodies_.find(id) != bodies_.end();
}

// ── Shape management ────────────────────────────────────────────────────────

void PhysicsWorld2D::addShape(BodyId bodyId, const ShapeDef& def) {
    auto it = bodies_.find(bodyId);
    if (it != bodies_.end()) it->second.shapes.push_back(def);
}

void PhysicsWorld2D::clearShapes(BodyId bodyId) {
    auto it = bodies_.find(bodyId);
    if (it != bodies_.end()) it->second.shapes.clear();
}

// ── State readback ──────────────────────────────────────────────────────────

void PhysicsWorld2D::getBodyState(BodyId id, float& x, float& y,
                                    float& vx, float& vy) const {
    auto it = bodies_.find(id);
    if (it != bodies_.end()) {
        const auto& b = it->second;
        x = b.x; y = b.y;
        vx = b.velocityX; vy = b.velocityY;
    }
}

// ── Internal helpers ────────────────────────────────────────────────────────

PhysicsWorld2D::AABB PhysicsWorld2D::computeBodyAABB_(
    const Body& body, const ShapeDef& shape) const
{
    return computeAABB(body.x, body.y, shape);
}

bool PhysicsWorld2D::canCollide_(const ShapeDef& a, const ShapeDef& b) const {
    return (a.layer & b.mask) != 0 && (b.layer & a.mask) != 0;
}

// ── Integration ─────────────────────────────────────────────────────────────

// (integrateVelocities is the public entry point — see Simulation pipeline section)

// ── Broad phase ─────────────────────────────────────────────────────────────

void PhysicsWorld2D::rebuildBroadphase_() const {
    broadphase_.clear();
    for (auto& [id, body] : bodies_) {
        for (auto& shape : body.shapes) {
            AABB aabb = computeBodyAABB_(body, shape);
            broadphase_.insert(id, aabb.minX, aabb.minY,
                               aabb.maxX - aabb.minX,
                               aabb.maxY - aabb.minY);
        }
    }
}

void PhysicsWorld2D::broadPhase_(
    std::vector<std::pair<BodyId, BodyId>>& outPairs)
{
    outPairs.clear();
    // Only iterate dynamic bodies (Rigid + Kinematic)
    for (auto& [id, body] : bodies_) {
        if (body.type == BodyType::Static) continue;
        if (body.shapes.empty()) continue;

        AABB aabb = computeBodyAABB_(body, body.shapes[0]);

        // Query against all bodies (static + dynamic) in same cells
        auto candidates = broadphase_.query(
            aabb.minX, aabb.minY,
            aabb.maxX - aabb.minX,
            aabb.maxY - aabb.minY);

        std::sort(candidates.begin(), candidates.end());
        auto last = std::unique(candidates.begin(), candidates.end());

        for (auto it = candidates.begin(); it != last; ++it) {
            BodyId other = *it;
            if (other == id) continue;
            if (!isBodyValid(other)) continue;

            // Deduplicate: only keep pairs where id < other.
            // Static bodies never iterate, so dynamic must always accept
            // pairs with statics regardless of ID ordering.
            auto otherIt = bodies_.find(other);
            bool otherIsStatic = (otherIt != bodies_.end() && otherIt->second.type == BodyType::Static);
            if (!otherIsStatic && id > other) continue;

            outPairs.push_back({id, other});
        }
    }
}

// ── Narrow phase dispatch ───────────────────────────────────────────────────

bool PhysicsWorld2D::narrowPhase_(
    const AABB& aabbA, const ShapeDef& shapeA,
    const AABB& aabbB, const ShapeDef& shapeB,
    float& outNX, float& outNY, float& outOverlap)
{
    ShapeType tA = shapeA.shapeType;
    ShapeType tB = shapeB.shapeType;

    // Reorder so tA <= tB for dispatch table
    if (static_cast<uint8_t>(tA) > static_cast<uint8_t>(tB)) {
        bool hit = narrowPhase_(aabbB, shapeB, aabbA, shapeA,
                                outNX, outNY, outOverlap);
        outNX = -outNX;
        outNY = -outNY;
        return hit;
    }

    // Dispatch table
    if (tA == ShapeType::Box && tB == ShapeType::Box) {
        return boxVsBox_(aabbA, aabbB, outNX, outNY, outOverlap);
    }
    if (tA == ShapeType::Box && tB == ShapeType::Circle) {
        float cx = (aabbB.minX + aabbB.maxX) * 0.5f;
        float cy = (aabbB.minY + aabbB.maxY) * 0.5f;
        return boxVsCircle_(aabbA, cx, cy, shapeB.radius,
                            outNX, outNY, outOverlap);
    }
    if (tA == ShapeType::Circle && tB == ShapeType::Circle) {
        float ax = (aabbA.minX + aabbA.maxX) * 0.5f;
        float ay = (aabbA.minY + aabbA.maxY) * 0.5f;
        float bx = (aabbB.minX + aabbB.maxX) * 0.5f;
        float by = (aabbB.minY + aabbB.maxY) * 0.5f;
        return circleVsCircle_(ax, ay, shapeA.radius,
                               bx, by, shapeB.radius,
                               outNX, outNY, outOverlap);
    }
    // Capsule variants stub
    return capsuleVsAny_(aabbA, shapeA, aabbB, shapeB, outNX, outNY, outOverlap);
}

// ── Contact solver ──────────────────────────────────────────────────────────

void PhysicsWorld2D::solveContacts_(const std::vector<CollisionPair>& pairs) {
    for (auto& pair : pairs) {
        if (pair.isTriggerPair) continue;

        auto itA = bodies_.find(pair.bodyA);
        auto itB = bodies_.find(pair.bodyB);
        if (itA == bodies_.end() || itB == bodies_.end()) continue;

        Body& bodyA = itA->second;
        Body& bodyB = itB->second;

        float sepX = pair.normalX * pair.overlap;
        float sepY = pair.normalY * pair.overlap;

        float invMassA = bodyA.invMass;
        float invMassB = bodyB.invMass;
        float totalInvMass = invMassA + invMassB;
        if (totalInvMass < 0.0001f) {
            // Both infinite mass. Still resolve: push kinematic bodies apart.
            bool aKinematic = (bodyA.type == BodyType::Kinematic);
            bool bKinematic = (bodyB.type == BodyType::Kinematic);
            if (aKinematic && bKinematic) {
                bodyA.x -= sepX * 0.5f; bodyA.y -= sepY * 0.5f;
                bodyB.x += sepX * 0.5f; bodyB.y += sepY * 0.5f;
            } else if (aKinematic) {
                bodyA.x -= sepX; bodyA.y -= sepY;
            } else if (bKinematic) {
                bodyB.x += sepX; bodyB.y += sepY;
            }
            // Static vs Static — nothing to push.
            continue;
        }

        float ratioA = invMassA / totalInvMass;
        float ratioB = invMassB / totalInvMass;

        bodyA.x -= sepX * ratioA;
        bodyA.y -= sepY * ratioA;
        bodyB.x += sepX * ratioB;
        bodyB.y += sepY * ratioB;

        // Zero velocity in separation direction
        float nLen = std::sqrt(pair.normalX * pair.normalX + pair.normalY * pair.normalY);
        if (nLen < 0.0001f) continue;
        float nx = pair.normalX / nLen;
        float ny = pair.normalY / nLen;

        auto zeroVelocityAlongNormal = [&](Body& body, float sign) {
            float vn = body.velocityX * nx + body.velocityY * ny;
            if (vn * sign < 0.f) {
                body.velocityX -= vn * nx;
                body.velocityY -= vn * ny;
            }
        };
        zeroVelocityAlongNormal(bodyA, -1.f);  // Zero vn when moving toward B (+normal, vn>0)
        zeroVelocityAlongNormal(bodyB, 1.f);   // Zero vn when moving toward A (-normal, vn<0)
    }
}

// ── CCD post-pass ───────────────────────────────────────────────────────────

void PhysicsWorld2D::ccdPostPass_() {
    if (ccdBuffer_.empty()) return;

    rebuildBroadphase_();

    for (auto& entry : ccdBuffer_) {
        auto it = bodies_.find(entry.id);
        if (it == bodies_.end()) continue;
        Body& body = it->second;

        float dx = body.x - entry.oldX;
        float dy = body.y - entry.oldY;
        AABB oldBox = entry.oldBox;

        // Build sweep region
        AABB sweepRegion = oldBox;
        sweepRegion.minX = std::min(sweepRegion.minX, oldBox.minX + dx);
        sweepRegion.maxX = std::max(sweepRegion.maxX, oldBox.maxX + dx);
        sweepRegion.minY = std::min(sweepRegion.minY, oldBox.minY + dy);
        sweepRegion.maxY = std::max(sweepRegion.maxY, oldBox.maxY + dy);

        auto candidates = broadphase_.query(
            sweepRegion.minX, sweepRegion.minY,
            sweepRegion.maxX - sweepRegion.minX,
            sweepRegion.maxY - sweepRegion.minY);
        std::sort(candidates.begin(), candidates.end());
        candidates.erase(std::unique(candidates.begin(), candidates.end()),
                         candidates.end());

        if (body.shapes.empty()) continue;
        const ShapeDef& shape = body.shapes[0];

        // Step 1: Swept AABB
        {
            float bestT = 1.f;
            for (auto se : candidates) {
                if (se == entry.id) continue;
                auto sit = bodies_.find(se);
                if (sit == bodies_.end() || sit->second.shapes.empty()) continue;
                if (!canCollide_(shape, sit->second.shapes[0])) continue;
                AABB targetBox = computeBodyAABB_(sit->second, sit->second.shapes[0]);
                float t = 1.f;
                if (sweptAABBvsAABB_(oldBox, dx, dy, targetBox, t) && t < bestT) {
                    bestT = t;
                }
            }
            if (bestT < 1.f) {
                float contact = std::max(0.f, bestT - 0.001f);
                body.x = entry.oldX + dx * contact;
                body.y = entry.oldY + dy * contact;
                dx = body.x - entry.oldX;
                dy = body.y - entry.oldY;
                oldBox = computeBodyAABB_(body, shape);
            }
        }

        // Step 2: Overlap resolve (iterative — pushing out of one overlap
        // may push into another, so loop until stable or max iterations)
        {
            constexpr int kMaxOverlapIter = 4;
            for (int oi = 0; oi < kMaxOverlapIter; ++oi) {
                bool anyResolved = false;
                AABB curBox = computeBodyAABB_(body, shape);
                for (auto se : candidates) {
                    if (se == entry.id) continue;
                    auto sit = bodies_.find(se);
                    if (sit == bodies_.end() || sit->second.shapes.empty()) continue;
                    if (!canCollide_(shape, sit->second.shapes[0])) continue;
                    AABB targetBox = computeBodyAABB_(sit->second, sit->second.shapes[0]);
                    if (!overlaps(curBox, targetBox)) continue;
                    float sx = 0.f, sy = 0.f;
                    minSeparation(curBox, targetBox, sx, sy);
                    body.x += sx; body.y += sy;
                    curBox.minX += sx; curBox.maxX += sx;
                    curBox.minY += sy; curBox.maxY += sy;
                    anyResolved = true;
                }
                if (!anyResolved) break;
            }
        }
    }
}

// ── Contact processing ──────────────────────────────────────────────────────

void PhysicsWorld2D::processContacts_(const std::vector<CollisionPair>& pairs) {
    std::unordered_set<ContactKey, ContactKeyHash> thisFrame;

    for (auto& pair : pairs) {
        ContactKey key{std::min(pair.bodyA, pair.bodyB),
                        std::max(pair.bodyA, pair.bodyB)};
        thisFrame.insert(key);

        CollisionPair cp = pair;
        if (prevFrameContacts_.count(key)) {
            cp.state = ContactState::Persist;
        } else {
            cp.state = ContactState::Begin;
        }

        // Save normal for End events
        lastContactNormals_[key] = cp;

        if (contactCb_) contactCb_(cp);
    }

    // Fire End for contacts that existed last frame but not this frame
    for (const auto& key : prevFrameContacts_) {
        if (!thisFrame.count(key)) {
            auto it = lastContactNormals_.find(key);
            if (it != lastContactNormals_.end()) {
                CollisionPair endPair = it->second;
                endPair.state = ContactState::End;
                endPair.overlap = 0.f;
                if (contactCb_) contactCb_(endPair);
            }
        }
    }

    prevFrameContacts_ = std::move(thisFrame);
}

// ── Simulation pipeline ─────────────────────────────────────────────────────

void PhysicsWorld2D::integrateVelocities(float dt) {
    for (auto& [id, body] : bodies_) {
        if (body.type == BodyType::Static) continue;
        if (body.type == BodyType::Rigid) {
            body.velocityX += gravityX_ * body.gravityScale * dt;
            body.velocityY += gravityY_ * body.gravityScale * dt;
        }
        body.x += body.velocityX * dt;
        body.y += body.velocityY * dt;
    }
}

void PhysicsWorld2D::resolveCollisions() {
    for (int iter = 0; iter < iterationCount_; ++iter) {
        rebuildBroadphase_();

        std::vector<std::pair<BodyId, BodyId>> pairs;
        broadPhase_(pairs);

        std::vector<CollisionPair> contacts;

        for (auto [idA, idB] : pairs) {
            auto itA = bodies_.find(idA);
            auto itB = bodies_.find(idB);
            if (itA == bodies_.end() || itB == bodies_.end()) continue;
            if (itA->second.shapes.empty() || itB->second.shapes.empty()) continue;

            const ShapeDef& shapeA = itA->second.shapes[0];
            const ShapeDef& shapeB = itB->second.shapes[0];

            if (!canCollide_(shapeA, shapeB)) continue;

            AABB aabbA = computeBodyAABB_(itA->second, shapeA);
            AABB aabbB = computeBodyAABB_(itB->second, shapeB);

            if (!overlaps(aabbA, aabbB)) continue;

            float nx, ny, overlap;
            if (!narrowPhase_(aabbA, shapeA, aabbB, shapeB, nx, ny, overlap))
                continue;

            CollisionPair pair;
            pair.bodyA = idA;
            pair.bodyB = idB;
            pair.normalX = nx;
            pair.normalY = ny;
            pair.overlap = overlap;
            pair.isTriggerPair = shapeA.isTrigger || shapeB.isTrigger;

            contacts.push_back(pair);
        }

        if (iter == 0) {
            processContacts_(contacts);
        }
        solveContacts_(contacts);
    }
}

void PhysicsWorld2D::step(float dt) {
    ccdBuffer_.clear();
    for (auto& [id, body] : bodies_) {
        if (body.ccdEnabled && !body.shapes.empty()) {
            ccdBuffer_.push_back({id, computeBodyAABB_(body, body.shapes[0]),
                                  body.x, body.y});
        }
    }

    integrateVelocities(dt);
    resolveCollisions();
    ccdPostPass_();
}

// ── Spatial queries ─────────────────────────────────────────────────────────

PhysicsWorld2D::RaycastResult PhysicsWorld2D::raycast(
    float startX, float startY,
    float dirX, float dirY,
    float maxDist,
    CollisionLayer layerMask,
    CollisionLayer ignoreLayer,
    BodyId ignoreBody) const
{
    RaycastResult result{};
    result.distance = maxDist;

    float len = std::sqrt(dirX * dirX + dirY * dirY);
    if (len < 0.0001f) return result;
    dirX /= len; dirY /= len;

    rebuildBroadphase_();

    const float cellSize = broadphase_.cellSize();
    const float invCellSize = 1.0f / cellSize;

    int cx = static_cast<int>(startX * invCellSize);
    int cy = static_cast<int>(startY * invCellSize);

    int stepX = (dirX > 0) ? 1 : -1;
    int stepY = (dirY > 0) ? 1 : -1;

    float tDeltaX = (std::abs(dirX) > 0.0001f) ? cellSize / std::abs(dirX) : 1e10f;
    float tDeltaY = (std::abs(dirY) > 0.0001f) ? cellSize / std::abs(dirY) : 1e10f;

    auto calcTMax = [&](float origin, int cell, float dir, float cs) -> float {
        if (std::abs(dir) < 0.0001f) return 1e10f;
        if (dir > 0) return ((cell + 1) * cs - origin) / std::abs(dir);
        return (origin - cell * cs) / std::abs(dir);
    };
    float tMaxX = calcTMax(startX, cx, dirX, cellSize);
    float tMaxY = calcTMax(startY, cy, dirY, cellSize);

    if (tMaxX > maxDist) tMaxX = maxDist + 1.f;
    if (tMaxY > maxDist) tMaxY = maxDist + 1.f;

    std::unordered_set<BodyId> visited;

    auto checkCell = [&]() {
        auto* cellPtr = broadphase_.queryCell(cx, cy);
        if (!cellPtr) return;
        for (auto id : *cellPtr) {
            if (id == ignoreBody) continue;
            if (!visited.insert(id).second) continue;

            auto it = bodies_.find(id);
            if (it == bodies_.end() || it->second.shapes.empty()) continue;

            const ShapeDef& shape = it->second.shapes[0];
            if ((shape.layer & layerMask) == 0) continue;
            if (ignoreLayer != 0 && (shape.layer & ignoreLayer) != 0) continue;

            AABB box = computeBodyAABB_(it->second, shape);

            float tMin = 0.f;
            float tMaxLocal = maxDist;
            bool valid = true;

            for (int axis = 0; axis < 2; ++axis) {
                float p = (axis == 0) ? startX : startY;
                float d = (axis == 0) ? dirX : dirY;
                float minB = (axis == 0) ? box.minX : box.minY;
                float maxB = (axis == 0) ? box.maxX : box.maxY;

                if (std::abs(d) < 0.0001f) {
                    if (p < minB || p > maxB) { valid = false; break; }
                } else {
                    float t1 = (minB - p) / d;
                    float t2 = (maxB - p) / d;
                    if (t1 > t2) std::swap(t1, t2);
                    tMin = std::max(tMin, t1);
                    tMaxLocal = std::min(tMaxLocal, t2);
                }
            }

            if (valid && tMin <= tMaxLocal && tMin >= 0.f && tMin < result.distance) {
                result.hit = true;
                result.bodyId = id;
                result.distance = tMin;
                result.hitX = startX + dirX * tMin;
                result.hitY = startY + dirY * tMin;

                float boxCx = (box.minX + box.maxX) * 0.5f;
                float boxCy = (box.minY + box.maxY) * 0.5f;
                result.normalX = boxCx - result.hitX;
                result.normalY = boxCy - result.hitY;
                float nLen = std::sqrt(result.normalX * result.normalX +
                                       result.normalY * result.normalY);
                if (nLen > 0.0001f) {
                    result.normalX /= nLen;
                    result.normalY /= nLen;
                }
            }
        }
    };

    float t = 0.f;
    while (t < maxDist) {
        checkCell();
        if (tMaxX < tMaxY) {
            t = tMaxX;
            tMaxX += tDeltaX;
            cx += stepX;
        } else {
            t = tMaxY;
            tMaxY += tDeltaY;
            cy += stepY;
        }
    }

    return result;
}

std::vector<PhysicsWorld2D::OverlapResult> PhysicsWorld2D::overlapRect(
    float centerX, float centerY,
    float halfW, float halfH,
    CollisionLayer layerMask) const
{
    std::vector<OverlapResult> results;
    rebuildBroadphase_();

    AABB query{centerX - halfW, centerY - halfH,
               centerX + halfW, centerY + halfH};
    float qw = query.maxX - query.minX;
    float qh = query.maxY - query.minY;

    auto candidates = broadphase_.query(query.minX, query.minY, qw, qh);
    std::sort(candidates.begin(), candidates.end());
    auto last = std::unique(candidates.begin(), candidates.end());

    for (auto it = candidates.begin(); it != last; ++it) {
        auto id = *it;
        auto bit = bodies_.find(id);
        if (bit == bodies_.end() || bit->second.shapes.empty()) continue;

        const ShapeDef& shape = bit->second.shapes[0];
        if ((shape.layer & layerMask) == 0) continue;

        AABB box = computeBodyAABB_(bit->second, shape);
        if (overlaps(query, box)) {
            OverlapResult r;
            r.bodyId = id;
            r.overlapX = std::min(query.maxX, box.maxX) - std::max(query.minX, box.minX);
            r.overlapY = std::min(query.maxY, box.maxY) - std::max(query.minY, box.minY);
            results.push_back(r);
        }
    }

    return results;
}

std::vector<PhysicsWorld2D::BodyId> PhysicsWorld2D::overlapCircle(
    float centerX, float centerY, float radius,
    CollisionLayer layerMask) const
{
    std::vector<BodyId> results;
    rebuildBroadphase_();

    float r2 = radius * radius;
    AABB queryBox{centerX - radius, centerY - radius,
                  centerX + radius, centerY + radius};
    float qw = queryBox.maxX - queryBox.minX;
    float qh = queryBox.maxY - queryBox.minY;

    auto candidates = broadphase_.query(queryBox.minX, queryBox.minY, qw, qh);
    std::sort(candidates.begin(), candidates.end());
    auto last = std::unique(candidates.begin(), candidates.end());

    for (auto it = candidates.begin(); it != last; ++it) {
        auto id = *it;
        auto bit = bodies_.find(id);
        if (bit == bodies_.end() || bit->second.shapes.empty()) continue;

        const ShapeDef& shape = bit->second.shapes[0];
        if ((shape.layer & layerMask) == 0) continue;

        AABB box = computeBodyAABB_(bit->second, shape);

        float closestX = std::max(box.minX, std::min(centerX, box.maxX));
        float closestY = std::max(box.minY, std::min(centerY, box.maxY));
        float dx = closestX - centerX;
        float dy = closestY - centerY;

        if (dx * dx + dy * dy <= r2) {
            results.push_back(id);
        }
    }

    return results;
}

} // namespace engine
