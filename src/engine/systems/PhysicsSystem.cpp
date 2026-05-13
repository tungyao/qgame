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

		/**
		 * 把 TileCollision 转成世界空间 AABB。
		 *
		 * 当前阶段的实现目标是先把 v2 collision profile 接到 PhysicsSystem：
		 * - Full: 整格 AABB
		 * - Rect: 使用局部 [x, y, w, h]
		 * - Polygon/OneWay: 暂时退化成点集包围盒，至少保证 authoring 数据不会失效
		 * - Trigger: 仍然给出体积，但后续逻辑只派发事件、不做分离
		 *
		 * 这里故意不读取 visual/animation，因为碰撞必须和显示帧解耦。
		 */
		AABB makeTileCollisionAABB(const Transform& mapTf,
			const TileMap& tmap,
			int tileX,
			int tileY,
			const TileMap::TileCollision& collision) {
			const float tileSize = static_cast<float>(tmap.tileSize);
			const float tileWorldX = mapTf.x + static_cast<float>(tileX) * tileSize;
			const float tileWorldY = mapTf.y + static_cast<float>(tileY) * tileSize;

			if (collision.shape == TileMap::TileCollisionShape::Rect &&
				collision.points.size() >= 4) {
				const float x = collision.points[0];
				const float y = collision.points[1];
				const float w = collision.points[2];
				const float h = collision.points[3];
				return { tileWorldX + x, tileWorldY + y,
						tileWorldX + x + w, tileWorldY + y + h };
			}

			if ((collision.shape == TileMap::TileCollisionShape::Polygon ||
				collision.shape == TileMap::TileCollisionShape::OneWay) &&
				collision.points.size() >= 4) {
				float minX = collision.points[0];
				float maxX = collision.points[0];
				float minY = collision.points[1];
				float maxY = collision.points[1];
				for (size_t i = 2; i + 1 < collision.points.size(); i += 2) {
					minX = std::min(minX, collision.points[i]);
					maxX = std::max(maxX, collision.points[i]);
					minY = std::min(minY, collision.points[i + 1]);
					maxY = std::max(maxY, collision.points[i + 1]);
				}
				return { tileWorldX + minX, tileWorldY + minY,
						tileWorldX + maxX, tileWorldY + maxY };
			}

			return {
				tileWorldX,
				tileWorldY,
				tileWorldX + tileSize,
				tileWorldY + tileSize
			};
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
	 * 碰撞解决 - 使用 SpatialHashGrid 宽相位检测并处理所有碰撞对
	 *
	 * 替换 SAP (Sweep and Prune) 为空间哈希网格：
	 * - 每帧重建 grid：multi-cell insert (用 AABB)
	 * - 对每个实体，查询覆盖 cell 内的候选实体
	 * - sort + unique 去重（cell 级），entity ID 排序去重（pair 级）
	 * - 静态-静态对跳过（两方都无 RigidBody 不产生碰撞事件）
	 *
	 * 复杂度：O(n * cells_per_entity + pairs * candidates)
	 * 较原 SAP 的平均 O(n log n + n*k) 在静态密集场景更有优势。
	 */
	void PhysicsSystem::resolveCollisions() {
		// Step 1: 构建空间哈希网格
		entityGrid_.clear();
		auto view = world_.view<Transform, Collider>();
		for (auto [e, tf, col] : view.each()) {
			(void)tf; (void)col;
			AABB aabb = makeEntityAABB(world_, e);
			entityGrid_.insert(e, aabb.minX, aabb.minY,
			                   aabb.maxX - aabb.minX, aabb.maxY - aabb.minY);
		}

		// Step 2: 碰撞检测
		// 遍历所有实体（含静态），用 entity ID 排序保证每对只处理一次
		for (auto [e, tf, col] : view.each()) {
			(void)tf;
			AABB aabb = makeEntityAABB(world_, e);
			bool hasRb = world_.all_of<RigidBody>(e);
			bool isKin = hasRb && world_.get<RigidBody>(e).isKinematic;

			// 查询覆盖 cell 内的所有候选实体
			auto candidates = entityGrid_.query(
				aabb.minX, aabb.minY,
				aabb.maxX - aabb.minX,
				aabb.maxY - aabb.minY
			);

			// Cell 级去重（跨 cell 重复） + pair 级去重（entity ID 排序）
			std::sort(candidates.begin(), candidates.end());
			auto last = std::unique(candidates.begin(), candidates.end());
			auto eId = entt::to_integral(e);

			for (auto it = candidates.begin(); it != last; ++it) {
				auto other = *it;
				auto oId = entt::to_integral(other);
				if (eId >= oId) continue;  // 每对只由 ID 较小者处理

				const Collider& ci = col;
				const Collider& cj = world_.get<Collider>(other);
				if (!canCollide(ci, cj)) continue;

				AABB otherAabb = makeEntityAABB(world_, other);
				if (!overlaps(aabb, otherAabb)) continue;

				float sepX = 0.f, sepY = 0.f;
				minSeparation(aabb, otherAabb, sepX, sepY);

				// 碰撞事件始终派发（无论 RigidBody 状态 — Trigger-only 实体也需事件）
				dispatcher_.trigger(CollisionInfo{e, other,  sepX,  sepY});
				dispatcher_.trigger(CollisionInfo{other, e, -sepX, -sepY});

				// Trigger 不产生物理分离
				if (ci.isTrigger || cj.isTrigger) continue;

				bool otherHasRb = world_.all_of<RigidBody>(other);
				bool otherIsKin = otherHasRb && world_.get<RigidBody>(other).isKinematic;

				// 静态-静态对不做分离
				if (!hasRb && !otherHasRb) continue;

				Transform& tfe = world_.get<Transform>(e);
				Transform& tfo = world_.get<Transform>(other);

				if (hasRb && !isKin && otherHasRb && !otherIsKin) {
					tfe.x += sepX * 0.5f; tfe.y += sepY * 0.5f;
					tfo.x -= sepX * 0.5f; tfo.y -= sepY * 0.5f;
					world_.patch<Transform>(e);
					world_.patch<Transform>(other);
				}
				else if (hasRb && !isKin) {
					tfe.x += sepX; tfe.y += sepY;
					world_.patch<Transform>(e);
				}
				else if (otherHasRb && !otherIsKin) {
					tfo.x -= sepX; tfo.y -= sepY;
					world_.patch<Transform>(other);
				}
			}
		}

		resolveTileCollisions();
	}

	/**
	 * TileMap 静态碰撞 - 只检查动态刚体当前 AABB 覆盖到的 tile 范围。
	 *
	 * TileMap v2 改为读取 TileCollision profile：
	 * - Layer::collidable=false 整层不参与
	 * - Trigger 只派发事件，不阻挡
	 * - Full/Rect/Polygon/OneWay 当前都会生成体积；其中 Polygon/OneWay
	 *   先退化成包围盒，便于后续继续细化形状求交
	 *
	 * 这样 visual/animation 的变化不会再隐式改变 physics 结果。
	 */
	void PhysicsSystem::resolveTileCollisions() {
		auto tilemaps = world_.view<Transform, TileMap>();
		if (tilemaps.begin() == tilemaps.end()) return;

		Collider tileCollider{};
		tileCollider.layer = COLLISION_LAYER_STATIC;
		tileCollider.mask = COLLISION_LAYER_ALL;

		auto actors = world_.view<Transform, Collider, RigidBody>();
		for (auto [actor, tf, col, rb] : actors.each()) {
			if (col.isTrigger || rb.isKinematic) continue;
			if (!canCollide(col, tileCollider)) continue;

			AABB actorBox = makeEntityAABB(world_, actor);

			for (auto [mapEntity, mapTf, tmap] : tilemaps.each()) {
				if (tmap.tileSize <= 0 || tmap.width <= 0 || tmap.height <= 0 ||
					tmap.tilesets.empty() || tmap.layers.empty()) {
					continue;
				}

				const float tileSize = static_cast<float>(tmap.tileSize);
				int minTileX = static_cast<int>(std::floor((actorBox.minX - mapTf.x) / tileSize));
				int maxTileX = static_cast<int>(std::floor((actorBox.maxX - mapTf.x) / tileSize));
				int minTileY = static_cast<int>(std::floor((actorBox.minY - mapTf.y) / tileSize));
				int maxTileY = static_cast<int>(std::floor((actorBox.maxY - mapTf.y) / tileSize));

				minTileX = std::max(0, minTileX);
				minTileY = std::max(0, minTileY);
				maxTileX = std::min(tmap.width - 1, maxTileX);
				maxTileY = std::min(tmap.height - 1, maxTileY);
				if (minTileX > maxTileX || minTileY > maxTileY) continue;

				for (int ty = minTileY; ty <= maxTileY; ++ty) {
					for (int tx = minTileX; tx <= maxTileX; ++tx) {
						for (int layer = 0; layer < static_cast<int>(tmap.layers.size()); ++layer) {
							const TileMap::TileCollision collision = tmap.collisionAt(layer, tx, ty);
							if (collision.shape == TileMap::TileCollisionShape::None) continue;

							const AABB tileBox = makeTileCollisionAABB(mapTf, tmap, tx, ty, collision);
							if (!overlaps(actorBox, tileBox)) continue;

							float sepX = 0.f;
							float sepY = 0.f;
							minSeparation(actorBox, tileBox, sepX, sepY);

							dispatcher_.trigger(CollisionInfo{ actor, mapEntity, sepX, sepY });
							dispatcher_.trigger(CollisionInfo{ mapEntity, actor, -sepX, -sepY });

							if (collision.shape == TileMap::TileCollisionShape::Trigger) {
								continue;
							}

							tf.x += sepX;
							tf.y += sepY;
							world_.patch<Transform>(actor);

							if (sepX != 0.f && rb.velocityX * sepX < 0.f) rb.velocityX = 0.f;
							if (sepY != 0.f && rb.velocityY * sepY < 0.f) rb.velocityY = 0.f;

							// [优化] 分离后直接偏移 AABB，避免重新查询 ECS 和 Sprite 组件。
							// actorBox 是栈上的本地副本，直接加减 sep 等价于重建。
							actorBox.minX += sepX; actorBox.maxX += sepX;
							actorBox.minY += sepY; actorBox.maxY += sepY;
						}
					}
				}
			}
		}
	}

	/**
	 * 射线检测 - 从起点沿方向发射射线，返回第一个碰撞体
	 *
	 * 使用 Slab 算法（Ray-AABB 交叉检测）
	 *
	 * 参数：
	 * - startX, startY: 射线起点
	 * - dirX, dirY: 射线方向（会自动归一化）
	 * - maxDist: 最大检测距离
	 * - layerMask: 只检测指定层的物体
	 *
	 * 返回：
	 * - RaycastHit.hit: 是否命中
	 * - RaycastHit.entity: 命中的实体
	 * - RaycastHit.hitX, hitY: 命中点坐标
	 * - RaycastHit.normalX, normalY: 命中点法线（指向碰撞体中心）
	 * - RaycastHit.distance: 起点到命中点的距离
	 *
	 * 用途：
	 * - 子弹/激光检测
	 * - 视线检测（AI 能否看到玩家）
	 * - 地面检测（角色是否站在地面上）
	 */
	RaycastHit PhysicsSystem::raycast(float startX, float startY, float dirX, float dirY,
		float maxDist, CollisionLayer layerMask) {
		RaycastHit result{};
		result.hit = false;
		result.distance = maxDist;

		// 归一化方向向量
		float len = std::sqrt(dirX * dirX + dirY * dirY);
		if (len < 0.0001f) return result;  // 零方向，无效射线
		dirX /= len; dirY /= len;

		// 遍历所有碰撞体
		auto view = world_.view<Transform, Collider>();
		for (auto [e, tf, col] : view.each()) {
			// 层过滤
			if ((col.layer & layerMask) == 0) continue;

			(void)tf;
			(void)col;
			// [优化] 每个实体只计算一次 AABB，同时复用于命中判断和法线计算
			const AABB box = makeEntityAABB(world_, e);

			// Slab 算法：计算射线与 AABB 的交点
			// tMin: 射线进入 AABB 的时间
			// tMax: 射线离开 AABB 的时间
			float tMin = 0.f;
			float tMax = maxDist;

			// 对每个轴分别计算
			for (int axis = 0; axis < 2; ++axis) {
				float p = (axis == 0) ? startX : startY;
				float d = (axis == 0) ? dirX : dirY;
				float minB = (axis == 0) ? box.minX : box.minY;
				float maxB = (axis == 0) ? box.maxX : box.maxY;

				if (std::abs(d) < 0.0001f) {
					// 射线与该轴平行，检查起点是否在 slab 内
					if (p < minB || p > maxB) {
						tMin = maxDist + 1.f;  // 标记为不相交
						break;
					}
				}
				else {
					// 计算射线与 slab 两个平面的交点
					float t1 = (minB - p) / d;
					float t2 = (maxB - p) / d;
					if (t1 > t2) std::swap(t1, t2);  // 确保 t1 <= t2

					// 更新相交区间
					tMin = std::max(tMin, t1);
					tMax = std::min(tMax, t2);
				}
			}

			// 检查是否相交且在有效范围内
			if (tMin <= tMax && tMin >= 0.f && tMin < result.distance) {
				result.hit = true;
				result.entity = e;
				result.distance = tMin;
				result.hitX = startX + dirX * tMin;
				result.hitY = startY + dirY * tMin;

				// [优化] 直接复用上方已算好的 box，原代码此处重复调用了 makeEntityAABB
				float cx = (box.minX + box.maxX) * 0.5f;
				float cy = (box.minY + box.maxY) * 0.5f;
				result.normalX = cx - result.hitX;
				result.normalY = cy - result.hitY;
				float nLen = std::sqrt(result.normalX * result.normalX + result.normalY * result.normalY);
				if (nLen > 0.0001f) {
					result.normalX /= nLen;
					result.normalY /= nLen;
				}
			}
		}

		return result;
	}

	/**
	 * 盒形区域查询 - 检测指定矩形区域内所有碰撞体
	 *
	 * 参数：
	 * - centerX, centerY: 查询区域中心
	 * - halfW, halfH: 查询区域半宽/半高
	 * - layerMask: 只查询指定层的物体
	 *
	 * 返回：
	 * - OverlapResult.entity: 碰撞的实体
	 * - OverlapResult.overlapX, overlapY: 重叠区域尺寸
	 *
	 * 用途：
	 * - 技能范围检测（横扫攻击）
	 * - 触发区域检测
	 * - AI 感知范围
	 */
	std::vector<OverlapResult> PhysicsSystem::overlapBox(float centerX, float centerY,
		float halfW, float halfH,
		CollisionLayer layerMask) {
		std::vector<OverlapResult> results;
		// 构造查询区域的 AABB
		AABB query{ centerX - halfW, centerY - halfH, centerX + halfW, centerY + halfH };

		auto view = world_.view<Transform, Collider>();
		for (auto [e, tf, col] : view.each()) {
			// 层过滤
			if ((col.layer & layerMask) == 0) continue;

			(void)tf;
			(void)col;
			AABB box = makeEntityAABB(world_, e);
			if (overlaps(query, box)) {
				OverlapResult r;
				r.entity = e;
				// 计算重叠区域尺寸
				r.overlapX = std::min(query.maxX, box.maxX) - std::max(query.minX, box.minX);
				r.overlapY = std::min(query.maxY, box.maxY) - std::max(query.minY, box.minY);
				results.push_back(r);
			}
		}

		return results;
	}

	/**
	 * 圆形区域查询 - 检测指定圆形区域内所有碰撞体
	 *
	 * 参数：
	 * - centerX, centerY: 圆心
	 * - radius: 半径
	 * - layerMask: 只查询指定层的物体
	 *
	 * 算法：
	 * 1. 先用 AABB 快速排除明显不相交的碰撞体
	 * 2. 计算 AABB 上离圆心最近的点
	 * 3. 检查该点到圆心的距离是否小于半径
	 *
	 * 用途：
	 * - 爆炸伤害范围
	 * - 吸引/排斥效果范围
	 * - NPC 交互范围
	 */
	std::vector<entt::entity> PhysicsSystem::overlapCircle(float centerX, float centerY, float radius,
		CollisionLayer layerMask) {
		std::vector<entt::entity> results;
		float r2 = radius * radius;  // 使用距离平方避免开方

		auto view = world_.view<Transform, Collider>();
		for (auto [e, tf, col] : view.each()) {
			// 层过滤
			if ((col.layer & layerMask) == 0) continue;

			(void)tf;
			(void)col;
			AABB box = makeEntityAABB(world_, e);

			// 找到 AABB 上离圆心最近的点
			// 将圆心 clamp 到 AABB 内部
			float closestX = std::max(box.minX, std::min(centerX, box.maxX));
			float closestY = std::max(box.minY, std::min(centerY, box.maxY));

			// 计算距离平方
			float dx = closestX - centerX;
			float dy = closestY - centerY;

			// 如果最近点在圆内，则 AABB 与圆相交
			if (dx * dx + dy * dy <= r2) {
				results.push_back(e);
			}
		}

		return results;
	}

} // namespace engine