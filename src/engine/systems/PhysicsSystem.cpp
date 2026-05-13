#include "PhysicsSystem.h"
#include "../components/RenderComponents.h"
#include "../components/PhysicsComponents.h"
#include <cmath>
#include <vector>
#include <algorithm>

namespace engine {

	PhysicsSystem::PhysicsSystem(entt::registry& world, entt::dispatcher& dispatcher)
		: world_(world), dispatcher_(dispatcher) {
	}

	void PhysicsSystem::init() {
		transformUpdateConnection_ =
			world_.on_update<Transform>().connect<&PhysicsSystem::onTransformUpdated>(this);

		// 监听 Collider / RigidBody 变化，维护 staticGrid_ 增量更新
		world_.on_construct<Collider>().connect<&PhysicsSystem::onColliderAdded>(this);
		world_.on_destroy<Collider>().connect<&PhysicsSystem::onColliderRemoved>(this);
		world_.on_construct<RigidBody>().connect<&PhysicsSystem::onRigidBodyAdded>(this);
		world_.on_destroy<RigidBody>().connect<&PhysicsSystem::onRigidBodyRemoved>(this);

		// 监听 TileMap 变化，维护 tile 碰撞缓存
		world_.on_construct<TileMap>().connect<&PhysicsSystem::onTileMapAdded>(this);
		world_.on_update<TileMap>().connect<&PhysicsSystem::onTileMapUpdated>(this);
		world_.on_destroy<TileMap>().connect<&PhysicsSystem::onTileMapRemoved>(this);
	}

	void PhysicsSystem::shutdown() {
		transformUpdateConnection_.release();
	}

	/**
	 * 主更新函数 - 使用固定时间步进行物理模拟
	 *
	 * 固定时间步的优势：
	 * 1. 物理行为一致 - 无论帧率如何，物体运动轨迹相同
	 * 2. 避免穿透 - 高帧率不会导致碰撞检测遗漏
	 * 3. 可重现性 - 相同输入产生相同结果，便于调试和网络同步
	 *
	 * 实现原理：
	 * - 累积器收集帧时间
	 * - 当累积时间超过固定步长时，执行一次物理更新
	 * - 剩余时间保留到下一帧
	 */
	void PhysicsSystem::update(float dt) {
		if (variableTimestep_) {
			// 可变时间步：每帧直接用真实 dt 积分，匹配渲染帧率。
			// 可变时间步下不保留插值历史，避免固定步插值带来的那帧延迟。
			snapshotInterpolatedBodiesForStep();
			steppingPhysics_ = true;
			integrateVelocities(dt);
			resolveCollisions();
			steppingPhysics_ = false;
			// 把 previous 追到 current，让渲染层直接显示最新位置
			auto snapView = world_.view<Transform, RigidBody>();
			for (auto [e, tf, rb] : snapView.each()) {
				(void)rb;
				if (auto* interpolation = world_.try_get<TransformInterpolation>(e)) {
					interpolation->previous = tf;
				}
			}
			accumulator_ = 0.f;
			return;
		}

		accumulator_ += dt;
		while (accumulator_ >= fixedTimestep_) {
			snapshotInterpolatedBodiesForStep();
			steppingPhysics_ = true;
			integrateVelocities(fixedTimestep_);
			resolveCollisions();
			steppingPhysics_ = false;
			accumulator_ -= fixedTimestep_;
		}
	}

	void PhysicsSystem::snapshotInterpolatedBodiesForStep() {
		auto view = world_.view<Transform, RigidBody>();
		for (auto [e, tf, rb] : view.each()) {
			auto& interpolation = world_.get_or_emplace<TransformInterpolation>(e);
			interpolation.previous = tf;
			interpolation.initialized = true;
			interpolation.disabled = !rb.interpolate;
		}
	}

	void PhysicsSystem::onTransformUpdated(entt::registry& reg, entt::entity e) {
		if (steppingPhysics_) {
			return;
		}
		if (!reg.all_of<RigidBody, Transform>(e)) {
			return;
		}

		// 物理步之外的 Transform 修改通常意味着 teleport、脚本 snap、关卡重置等。
		// 这些场景不应继续沿用旧的 previous snapshot，否则表现层会把一次瞬移插值成
		// 一段拖影。因此这里立刻把 previous 追到 current，强制下一帧直接显示新位置。
		auto& interpolation = reg.get_or_emplace<TransformInterpolation>(e);
		interpolation.previous = reg.get<Transform>(e);
		interpolation.initialized = true;
	}

	/**
	 * 速度积分 - 根据力和速度更新位置
	 *
	 * 使用半隐式欧拉积分（Symplectic Euler）：
	 * 1. 先更新速度（考虑重力）
	 * 2. 再用新速度更新位置
	 *
	 * 相比显式欧拉积分，半隐式欧拉更稳定，能量守恒更好
	 */
	void PhysicsSystem::integrateVelocities(float dt) {
		auto view = world_.view<Transform, RigidBody>();
		for (auto [e, tf, rb] : view.each()) {
			// 非运动学刚体才受重力影响
			if (!rb.isKinematic) {
				// v += g * gravityScale * dt
				// gravityScale = 0 表示不受重力（如浮空平台）
				// gravityScale = 1 正常重力
				// gravityScale = 2 双倍重力（如重物）
				rb.velocityX += gravityX_ * rb.gravityScale * dt;
				rb.velocityY += gravityY_ * rb.gravityScale * dt;
			}
			// 位置更新：x += v * dt
			tf.x += rb.velocityX * dt;
			tf.y += rb.velocityY * dt;
			if (rb.velocityX != 0.f || rb.velocityY != 0.f) {
				world_.patch<Transform>(e);
			}
		}
	}

	namespace {

		/**
		 * 轴对齐包围盒 - 用于快速碰撞检测
		 * minXY 是左上角，maxXY 是右下角
		 */
		struct AABB { float minX, minY, maxX, maxY; };

		/**
		 * 从 Transform 和 Collider 计算 AABB
		 *
		 * Collider 的 offset 用于将碰撞盒偏移到精灵中心或特定位置
		 * 例如：精灵 32x32，碰撞盒 24x24 居中，offset = (4, 4)
		 */
		AABB makeAABB(const Transform& tf, const Collider& col) {
			float x = tf.x + col.offsetX;
			float y = tf.y + col.offsetY;
			return { x, y, x + col.width, y + col.height };
		}

		/**
		 * 从 ECS 实体计算碰撞 AABB。
		 *
		 * 设计约定：
		 * - 没有 Sprite 的纯物理实体沿用旧语义：Transform 是碰撞盒左上角。
		 * - 有 Sprite 的可见实体使用渲染语义：Transform 是 Sprite pivot 所在点。
		 *   Collider::offsetX/Y 从渲染后的 Sprite 左上角开始偏移。这样默认
		 *   pivot=(0.5,0.5)、Collider=Sprite 大小时，碰撞盒会和显示图像重合。
		 */
		AABB makeEntityAABB(entt::registry& world, entt::entity e) {
			const Transform& tf = world.get<Transform>(e);
			const Collider& col = world.get<Collider>(e);

			if (const Sprite* sprite = world.try_get<Sprite>(e)) {
				const float spriteW = sprite->srcRect.w * std::abs(tf.scaleX);
				const float spriteH = sprite->srcRect.h * std::abs(tf.scaleY);
				const float left = tf.x - sprite->pivotX * spriteW;
				const float top = tf.y - sprite->pivotY * spriteH;
				const float x = left + col.offsetX;
				const float y = top + col.offsetY;
				return { x, y, x + col.width, y + col.height };
			}

			return makeAABB(tf, col);
		}

		/**
		 * AABB 重叠检测 - 分离轴定理（SAT）的简化版
		 *
		 * 两个 AABB 重叠当且仅当：
		 * - X 轴投影重叠 AND Y 轴投影重叠
		 *
		 * 这是 O(1) 的快速检测，用于宽相位过滤
		 */
		bool overlaps(const AABB& a, const AABB& b) {
			return a.minX < b.maxX && a.maxX > b.minX &&
				a.minY < b.maxY && a.maxY > b.minY;
		}

		/**
		 * 计算最小分离向量（Minimum Translation Vector）
		 *
		 * 当两个 AABB 重叠时，返回将它们分离所需的最小位移
		 *
		 * 算法：
		 * 1. 计算 X 和 Y 方向的重叠量
		 * 2. 选择重叠量较小的轴推出（这是视觉上更自然的分离方向）
		 * 3. 根据中心点位置决定推出方向
		 *
		 * outX, outY 是将 A 推离 B 的位移向量
		 */
		void minSeparation(const AABB& a, const AABB& b, float& outX, float& outY) {
			// 计算重叠区域尺寸
			float overlapX = std::min(a.maxX, b.maxX) - std::max(a.minX, b.minX);
			float overlapY = std::min(a.maxY, b.maxY) - std::max(a.minY, b.minY);

			// 选择重叠量较小的轴分离（穿透深度较小的方向）
			if (overlapX < overlapY) {
				// X 轴分离
				float aCx = (a.minX + a.maxX) * 0.5f;
				float bCx = (b.minX + b.maxX) * 0.5f;
				// A 在 B 左边则向左推，否则向右推
				outX = (aCx < bCx) ? -overlapX : overlapX;
				outY = 0.f;
			}
			else {
				// Y 轴分离
				float aCy = (a.minY + a.maxY) * 0.5f;
				float bCy = (b.minY + b.maxY) * 0.5f;
				outX = 0.f;
				outY = (aCy < bCy) ? -overlapY : overlapY;
			}
		}

	} // anonymous namespace

	/**
	 * 碰撞过滤 - 判断两个碰撞体是否应该产生碰撞
	 *
	 * 碰撞层系统（Layer/Mask）：
	 * - layer: 物体所属的层（位掩码，一个物体只能在一个层）
	 * - mask: 物体能碰撞的层（位掩码，可以碰撞多个层）
	 *
	 * 示例配置：
	 * - 玩家：layer = PLAYER(4), mask = ALL
	 * - 敌人：layer = ENEMY(8), mask = PLAYER | STATIC
	 * - 子弹：layer = BULLET(16), mask = ENEMY
	 *
	 * 碰撞条件：(A.layer & B.mask) != 0 AND (B.layer & A.mask) != 0
	 * 这允许非对称碰撞：子弹能碰敌人，敌人不能碰子弹
	 */
	bool PhysicsSystem::canCollide(const Collider& a, const Collider& b) const {
		// 两个都是 Trigger 则不产生物理碰撞
		//if (a.isTrigger && b.isTrigger) return false;

		// 双向检测：A 能碰 B，且 B 能碰 A
		return (a.layer & b.mask) != 0 && (b.layer & a.mask) != 0;
	}

	/**
	 * 碰撞解决 — SpatialHashGrid 宽相位 + 静动分离 + 多次迭代
	 *
	 * 构建两个独立网格：
	 * - dynamicGrid_：有 RigidBody 的实体（含 kinematic）
	 * - staticGrid_：无 RigidBody 的实体
	 *
	 * 外层只遍历动态体，产生两类碰撞对：
	 *   A) 动态-动态：查 dynamicGrid_，entity ID 排序去重，分离逻辑完整
	 *   B) 动态-静态：查 staticGrid_，无需 pair 去重，推开动态/kinematic 体
	 *   静态-静态对：完全跳过（移动实体必须有 RigidBody）
	 *
	 * 多次迭代（默认 3 次）：每次迭代后重建 grid 获取更新后的位置，
	 * 解决多体堆叠/角落穿透问题（如 player 把球推入墙角）。
	 */
	void PhysicsSystem::rebuildGrids() {
		dynamicGrid_.clear();
		staticGrid_.clear();
		auto allView = world_.view<Transform, Collider>();
		for (auto [e, tf, col] : allView.each()) {
			(void)tf; (void)col;
			AABB aabb = makeEntityAABB(world_, e);
			bool hasRb = world_.all_of<RigidBody>(e);
			auto& grid = hasRb ? dynamicGrid_ : staticGrid_;
			grid.insert(e, aabb.minX, aabb.minY,
			            aabb.maxX - aabb.minX, aabb.maxY - aabb.minY);
		}
	}

	void PhysicsSystem::resolveCollisions() {
		const int kMaxIter = 3;

		for (int iter = 0; iter < kMaxIter; ++iter) {
			rebuildGrids();

			auto dynView = world_.view<Transform, Collider, RigidBody>();
			for (auto [e, tf, col, rb] : dynView.each()) {
				(void)tf;
				AABB aabb = makeEntityAABB(world_, e);
				bool isKin = rb.isKinematic;

				// ── B) 动态-静态（先处理，避免动态体被推入静态体）──
				{
					auto candidates = staticGrid_.query(
						aabb.minX, aabb.minY,
						aabb.maxX - aabb.minX,
						aabb.maxY - aabb.minY
					);
					std::sort(candidates.begin(), candidates.end());
					auto last = std::unique(candidates.begin(), candidates.end());

					for (auto it = candidates.begin(); it != last; ++it) {
						auto other = *it;
						if (other == e) continue;

						const Collider& cj = world_.get<Collider>(other);
						if (!canCollide(col, cj)) continue;

						AABB otherAabb = makeEntityAABB(world_, other);
						if (!overlaps(aabb, otherAabb)) continue;

						float sepX = 0.f, sepY = 0.f;
						minSeparation(aabb, otherAabb, sepX, sepY);

						if (iter == 0) {
							dispatcher_.trigger(CollisionInfo{e, other,  sepX,  sepY});
							dispatcher_.trigger(CollisionInfo{other, e, -sepX, -sepY});
						}
						if (col.isTrigger || cj.isTrigger) continue;

						Transform& tfe = world_.get<Transform>(e);
						tfe.x += sepX; tfe.y += sepY;
						world_.patch<Transform>(e);
					}
				}

				// ── A) 动态-动态 ──
				{
					auto candidates = dynamicGrid_.query(
						aabb.minX, aabb.minY,
						aabb.maxX - aabb.minX,
						aabb.maxY - aabb.minY
					);
					std::sort(candidates.begin(), candidates.end());
					auto last = std::unique(candidates.begin(), candidates.end());
					auto eId = entt::to_integral(e);

					for (auto it = candidates.begin(); it != last; ++it) {
						auto other = *it;
						if (other == e) continue;
						if (eId >= entt::to_integral(other)) continue;

						const Collider& cj = world_.get<Collider>(other);
						if (!canCollide(col, cj)) continue;

						AABB otherAabb = makeEntityAABB(world_, other);
						if (!overlaps(aabb, otherAabb)) continue;

						float sepX = 0.f, sepY = 0.f;
						minSeparation(aabb, otherAabb, sepX, sepY);

						if (iter == 0) {
							dispatcher_.trigger(CollisionInfo{e, other,  sepX,  sepY});
							dispatcher_.trigger(CollisionInfo{other, e, -sepX, -sepY});
						}
						if (col.isTrigger || cj.isTrigger) continue;

						bool otherIsKin = world_.get<RigidBody>(other).isKinematic;

						Transform& tfe = world_.get<Transform>(e);
						Transform& tfo = world_.get<Transform>(other);

						if (!isKin && !otherIsKin) {
							tfe.x += sepX * 0.5f; tfe.y += sepY * 0.5f;
							tfo.x -= sepX * 0.5f; tfo.y -= sepY * 0.5f;
							world_.patch<Transform>(e);
							world_.patch<Transform>(other);
						}
						else if (!isKin) {
							tfe.x += sepX; tfe.y += sepY;
							world_.patch<Transform>(e);
						}
						else if (!otherIsKin) {
							tfo.x -= sepX; tfo.y -= sepY;
							world_.patch<Transform>(other);
						}
					}
				}
			}
		}

		resolveTileCollisions();
	}

	void PhysicsSystem::onColliderAdded(entt::registry& reg, entt::entity e) {
		if (!reg.all_of<Transform>(e)) return;
		if (reg.all_of<RigidBody>(e)) return;  // 有 RigidBody → 动态体
		// 无 RigidBody → 静态体，插入 staticGrid_
		AABB aabb = makeEntityAABB(reg, e);
		staticGrid_.insert(e, aabb.minX, aabb.minY,
		                    aabb.maxX - aabb.minX, aabb.maxY - aabb.minY);
	}

	void PhysicsSystem::onColliderRemoved(entt::registry& reg, entt::entity e) {
		(void)reg;
		// 无法从 SpatialHashGrid 移除单实体，标记为无效
		// staticGrid_ 每帧全量重建，冷数据自然消失
		// 后续优化：SpatialHashGrid 增加 remove() 或 versioned entry
	}

	void PhysicsSystem::onRigidBodyAdded(entt::registry& reg, entt::entity e) {
		if (!reg.all_of<Transform, Collider>(e)) return;
		// 从静态变为动态，删不掉 staticGrid_ 条目，同上帧重建策略
		(void)e;
	}

	void PhysicsSystem::onRigidBodyRemoved(entt::registry& reg, entt::entity e) {
		if (!reg.all_of<Transform, Collider>(e)) return;
		// 从动态变为静态，下帧 staticGrid_ 重建时会包含它
		(void)e;
	}

	// ── Tile 碰撞缓存 ──────────────────────────────────────────────────

	void PhysicsSystem::rebuildTileCollisionCache(entt::entity mapEntity, const TileMap& tmap) {
		TileCollisionCache cache;
		cache.width = tmap.width;
		cache.height = tmap.height;
		cache.tileSize = static_cast<float>(tmap.tileSize);
		cache.valid = true;

		cache.grid.resize(tmap.height);
		for (int ty = 0; ty < tmap.height; ++ty) {
			cache.grid[ty].resize(tmap.width);
			for (int tx = 0; tx < tmap.width; ++tx) {
				auto& entry = cache.grid[ty][tx];

				// 取所有 collidable 层中第一个非 None 的碰撞
				TileMap::TileCollision collision;
				collision.shape = TileMap::TileCollisionShape::None;
				for (int layer = 0; layer < static_cast<int>(tmap.layers.size()); ++layer) {
					collision = tmap.collisionAt(layer, tx, ty);
					if (collision.shape != TileMap::TileCollisionShape::None) break;
				}

				entry.shape = static_cast<uint8_t>(collision.shape);
				entry.isTrigger = (collision.shape == TileMap::TileCollisionShape::Trigger);

				if (collision.shape == TileMap::TileCollisionShape::None) {
					entry.localMinX = entry.localMinY = entry.localMaxX = entry.localMaxY = 0.f;
					continue;
				}

				// 以下计算逻辑与 makeTileCollisionAABB 一致，但结果是 tile 本地坐标
				const float ts = cache.tileSize;
				const float tx_f = static_cast<float>(tx);
				const float ty_f = static_cast<float>(ty);

				if (collision.shape == TileMap::TileCollisionShape::Rect &&
					collision.points.size() >= 4) {
					entry.localMinX = tx_f * ts + collision.points[0];
					entry.localMinY = ty_f * ts + collision.points[1];
					entry.localMaxX = entry.localMinX + collision.points[2];
					entry.localMaxY = entry.localMinY + collision.points[3];
				}
				else if ((collision.shape == TileMap::TileCollisionShape::Polygon ||
				          collision.shape == TileMap::TileCollisionShape::OneWay) &&
				          collision.points.size() >= 4) {
					float minX = tx_f * ts + collision.points[0];
					float maxX = minX;
					float minY = ty_f * ts + collision.points[1];
					float maxY = minY;
					for (size_t pi = 2; pi + 1 < collision.points.size(); pi += 2) {
						minX = std::min(minX, tx_f * ts + collision.points[pi]);
						maxX = std::max(maxX, tx_f * ts + collision.points[pi]);
						minY = std::min(minY, ty_f * ts + collision.points[pi + 1]);
						maxY = std::max(maxY, ty_f * ts + collision.points[pi + 1]);
					}
					entry.localMinX = minX;
					entry.localMinY = minY;
					entry.localMaxX = maxX;
					entry.localMaxY = maxY;
				}
				else {
					// Full / Trigger / default
					entry.localMinX = tx_f * ts;
					entry.localMinY = ty_f * ts;
					entry.localMaxX = entry.localMinX + ts;
					entry.localMaxY = entry.localMinY + ts;
				}
			}
		}

		tileCollisionCaches_[mapEntity] = std::move(cache);
	}

	void PhysicsSystem::onTileMapAdded(entt::registry& reg, entt::entity e) {
		if (!reg.all_of<TileMap>(e)) return;
		rebuildTileCollisionCache(e, reg.get<TileMap>(e));
	}

	void PhysicsSystem::onTileMapUpdated(entt::registry& reg, entt::entity e) {
		if (!reg.all_of<TileMap>(e)) return;
		rebuildTileCollisionCache(e, reg.get<TileMap>(e));
	}

	void PhysicsSystem::onTileMapRemoved(entt::registry& reg, entt::entity e) {
		(void)reg;
		tileCollisionCaches_.erase(e);
	}

	/**
	 * TileMap 碰撞 — 使用预计算缓存，跳过 collisionAt() 查找链。
	 *
	 * 每个 tile 的 AABB（本地坐标系）在 TileMap 挂载/更新时预计算。
	 * 每帧直接读取缓存，无需 gid→tileset→collision profile 的线性查找。
	 */
	void PhysicsSystem::resolveTileCollisions() {
		if (tileCollisionCaches_.empty()) return;

		Collider tileCollider{};
		tileCollider.layer = COLLISION_LAYER_STATIC;
		tileCollider.mask = COLLISION_LAYER_ALL;

		auto actors = world_.view<Transform, Collider, RigidBody>();
		for (auto [actor, tf, col, rb] : actors.each()) {
			if (col.isTrigger || rb.isKinematic) continue;
			if (!canCollide(col, tileCollider)) continue;

			AABB actorBox = makeEntityAABB(world_, actor);

			for (auto& [mapEntity, cache] : tileCollisionCaches_) {
				if (!cache.valid) continue;
				const Transform* mapTf = world_.try_get<Transform>(mapEntity);
				if (!mapTf) continue;

				const float ts = cache.tileSize;
				if (ts <= 0.f) continue;

				int minTileX = static_cast<int>(std::floor((actorBox.minX - mapTf->x) / ts));
				int maxTileX = static_cast<int>(std::floor((actorBox.maxX - mapTf->x) / ts));
				int minTileY = static_cast<int>(std::floor((actorBox.minY - mapTf->y) / ts));
				int maxTileY = static_cast<int>(std::floor((actorBox.maxY - mapTf->y) / ts));

				minTileX = std::max(0, minTileX);
				minTileY = std::max(0, minTileY);
				maxTileX = std::min(cache.width - 1, maxTileX);
				maxTileY = std::min(cache.height - 1, maxTileY);
				if (minTileX > maxTileX || minTileY > maxTileY) continue;

				for (int ty = minTileY; ty <= maxTileY; ++ty) {
					for (int tx = minTileX; tx <= maxTileX; ++tx) {
						const auto& entry = cache.grid[ty][tx];
						if (entry.shape == static_cast<uint8_t>(TileMap::TileCollisionShape::None))
							continue;

						// 本地 AABB → 世界 AABB
						AABB tileBox{
							mapTf->x + entry.localMinX,
							mapTf->y + entry.localMinY,
							mapTf->x + entry.localMaxX,
							mapTf->y + entry.localMaxY
						};

						if (!overlaps(actorBox, tileBox)) continue;

						float sepX = 0.f, sepY = 0.f;
						minSeparation(actorBox, tileBox, sepX, sepY);

						dispatcher_.trigger(CollisionInfo{actor, mapEntity, sepX, sepY});
						dispatcher_.trigger(CollisionInfo{mapEntity, actor, -sepX, -sepY});

						if (entry.isTrigger) continue;

						tf.x += sepX;
						tf.y += sepY;
						world_.patch<Transform>(actor);

						if (sepX != 0.f && rb.velocityX * sepX < 0.f) rb.velocityX = 0.f;
						if (sepY != 0.f && rb.velocityY * sepY < 0.f) rb.velocityY = 0.f;

						actorBox.minX += sepX; actorBox.maxX += sepX;
						actorBox.minY += sepY; actorBox.maxY += sepY;
					}
				}
			}
		}
	}

	/**
	 * 射线检测 — DDA grid traversal + Slab 窄相位
	 *
	 * 从射线起点沿方向遍历穿过的 grid cell，只检测这些 cell 内的实体。
	 * 结合 dynamicGrid_ 和 staticGrid_ 覆盖所有碰撞体（含无 RigidBody 的 Trigger）。
	 */
	RaycastHit PhysicsSystem::raycast(float startX, float startY, float dirX, float dirY,
		float maxDist, CollisionLayer layerMask, CollisionLayer ignoreLayer,
		entt::entity ignoreEntity) {
		RaycastHit result{};
		result.hit = false;
		result.distance = maxDist;

		float len = std::sqrt(dirX * dirX + dirY * dirY);
		if (len < 0.0001f) return result;
		dirX /= len; dirY /= len;

		rebuildGrids();

		const float cellSize = dynamicGrid_.cellSize();
		const float invCellSize = 1.0f / cellSize;

		// 起始 cell（与 SpatialHashGrid 坐标约定一致）
		int cx = static_cast<int>(startX * invCellSize);
		int cy = static_cast<int>(startY * invCellSize);

		int stepX = (dirX > 0) ? 1 : -1;
		int stepY = (dirY > 0) ? 1 : -1;

		// tDelta = 沿射线穿过一个 cell 所需的距离
		float tDeltaX = (std::abs(dirX) > 0.0001f) ? cellSize / std::abs(dirX) : 1e10f;
		float tDeltaY = (std::abs(dirY) > 0.0001f) ? cellSize / std::abs(dirY) : 1e10f;

		// tMax = 沿射线到下一个 cell 边界的距离
		auto calcTMax = [&](float origin, int cell, float dir, float cellSize) -> float {
			if (std::abs(dir) < 0.0001f) return 1e10f;
			if (dir > 0) return ((cell + 1) * cellSize - origin) / std::abs(dir);
			return (origin - cell * cellSize) / std::abs(dir);
		};
		float tMaxX = calcTMax(startX, cx, dirX, cellSize);
		float tMaxY = calcTMax(startY, cy, dirY, cellSize);

		// 确保不超出 maxDist
		if (tMaxX > maxDist) tMaxX = maxDist + 1.f;
		if (tMaxY > maxDist) tMaxY = maxDist + 1.f;

		entt::sparse_set visited;
		float t = 0.f;

		auto checkCellEntities = [&](const SpatialHashGrid<entt::entity>& grid) {
			auto* cell = grid.queryCell(cx, cy);
			if (!cell) return;
			for (auto e : *cell) {
				if (visited.contains(e)) continue;
				visited.push(e);

				if (e == ignoreEntity) continue;

				const Collider& col = world_.get<Collider>(e);
				if ((col.layer & layerMask) == 0) continue;
				if (ignoreLayer != 0 && (col.layer & ignoreLayer) != 0) continue;

				const AABB box = makeEntityAABB(world_, e);

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
					result.entity = e;
					result.distance = tMin;
					result.hitX = startX + dirX * tMin;
					result.hitY = startY + dirY * tMin;

					float boxCx = (box.minX + box.maxX) * 0.5f;
					float boxCy = (box.minY + box.maxY) * 0.5f;
					result.normalX = boxCx - result.hitX;
					result.normalY = boxCy - result.hitY;
					float nLen = std::sqrt(result.normalX * result.normalX + result.normalY * result.normalY);
					if (nLen > 0.0001f) {
						result.normalX /= nLen;
						result.normalY /= nLen;
					}
				}
			}
		};

		while (t < maxDist) {
			checkCellEntities(dynamicGrid_);
			checkCellEntities(staticGrid_);

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

	/**
	 * 盒形区域查询 — 通过 grid 只查覆盖 cell 内的实体
	 */
	std::vector<OverlapResult> PhysicsSystem::overlapBox(float centerX, float centerY,
		float halfW, float halfH,
		CollisionLayer layerMask) {
		std::vector<OverlapResult> results;
		rebuildGrids();

		AABB query{ centerX - halfW, centerY - halfH, centerX + halfW, centerY + halfH };
		float qw = query.maxX - query.minX;
		float qh = query.maxY - query.minY;

		// 合并两个网格的候选集
		auto candidates = dynamicGrid_.query(query.minX, query.minY, qw, qh);
		auto staticCandidates = staticGrid_.query(query.minX, query.minY, qw, qh);
		candidates.insert(candidates.end(), staticCandidates.begin(), staticCandidates.end());

		std::sort(candidates.begin(), candidates.end());
		auto last = std::unique(candidates.begin(), candidates.end());

		for (auto it = candidates.begin(); it != last; ++it) {
			auto e = *it;
			const Collider& col = world_.get<Collider>(e);
			if ((col.layer & layerMask) == 0) continue;

			AABB box = makeEntityAABB(world_, e);
			if (overlaps(query, box)) {
				OverlapResult r;
				r.entity = e;
				r.overlapX = std::min(query.maxX, box.maxX) - std::max(query.minX, box.minX);
				r.overlapY = std::min(query.maxY, box.maxY) - std::max(query.minY, box.minY);
				results.push_back(r);
			}
		}

		return results;
	}

	/**
	 * 圆形区域查询 — 通过 grid 只查覆盖 cell 内的实体
	 */
	std::vector<entt::entity> PhysicsSystem::overlapCircle(float centerX, float centerY, float radius,
		CollisionLayer layerMask) {
		std::vector<entt::entity> results;
		rebuildGrids();

		float r2 = radius * radius;
		AABB queryBox{ centerX - radius, centerY - radius, centerX + radius, centerY + radius };
		float qw = queryBox.maxX - queryBox.minX;
		float qh = queryBox.maxY - queryBox.minY;

		auto candidates = dynamicGrid_.query(queryBox.minX, queryBox.minY, qw, qh);
		auto staticCandidates = staticGrid_.query(queryBox.minX, queryBox.minY, qw, qh);
		candidates.insert(candidates.end(), staticCandidates.begin(), staticCandidates.end());

		std::sort(candidates.begin(), candidates.end());
		auto last = std::unique(candidates.begin(), candidates.end());

		for (auto it = candidates.begin(); it != last; ++it) {
			auto e = *it;
			const Collider& col = world_.get<Collider>(e);
			if ((col.layer & layerMask) == 0) continue;

			AABB box = makeEntityAABB(world_, e);

			float closestX = std::max(box.minX, std::min(centerX, box.maxX));
			float closestY = std::max(box.minY, std::min(centerY, box.maxY));
			float dx = closestX - centerX;
			float dy = closestY - centerY;

			if (dx * dx + dy * dy <= r2) {
				results.push_back(e);
			}
		}

		return results;
	}

} // namespace engine