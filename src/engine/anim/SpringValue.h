#pragma once
#include <cmath>
#include <core/math/Vec2.h>

namespace engine {

	// Phase 5.2: 临界/欠/过阻尼弹簧（半隐式欧拉）
	// 用途：相机跟随、UI 弹入、采集物吸附、命中位移回弹
	struct SpringValue {
		float value = 0.f;
		float velocity = 0.f;
		float target = 0.f;
		float stiffness = 120.f;   // 越大越快收敛
		float damping = 14.f;    // 越大越没"震荡"，~2*sqrt(stiffness) ≈ 临界

		// 推进一帧；返回当前 value
		float update(float dt) {
			// F = -k*(x - target) - c*v
			const float a = -stiffness * (value - target) - damping * velocity;
			velocity += a * dt;
			value += velocity * dt;
			return value;
		}

		// 直接 snap 到目标 (无残余速度)
		void snap(float v) {
			value = target = v;
			velocity = 0.f;
		}

		// 是否已经"足够静止"
		bool atRest(float epsValue = 1e-3f, float epsVel = 1e-3f) const {
			return std::abs(value - target) < epsValue && std::abs(velocity) < epsVel;
		}

		// 临界阻尼系数（给定 stiffness 推荐 damping）
		static float criticalDamping(float k) { return 2.f * std::sqrt(k); }
	};

	// 双轴弹簧：增强点 —— 控制 x、y 两轴，内部由两个 SpringValue 组成
	struct SpringValueVec2 {
		SpringValue x, y;

		// 推进一步，返回当前值
		core::Vec2 update(float dt) {
			return { x.update(dt), y.update(dt) };
		}

		void target(const core::Vec2& p) {
			x.target = p.x;
			y.target = p.y;
		}
		// 直接 snap 到目标位置，速度清零
		void snap(const core::Vec2& v) {
			x.snap(v.x);
			y.snap(v.y);
		}

		// 同时 snap 并设置目标（常用于初始化）
		void snap(const core::Vec2& val, const core::Vec2& target) {
			x.value = val.x;    x.target = target.x;  x.velocity = 0.f;
			y.value = val.y;    y.target = target.y;  y.velocity = 0.f;
		}

		// 是否两轴都足够静止
		bool atRest(float epsValue = 1e-3f, float epsVel = 1e-3f) const {
			return x.atRest(epsValue, epsVel) && y.atRest(epsValue, epsVel);
		}

		// 设置两轴统一的 stiffness / damping（大部分情况两轴动力学相同）
		void setDynamics(float stiffness, float damping) {
			x.stiffness = y.stiffness = stiffness;
			x.damping = y.damping = damping;
		}

		// 便捷设置：用 stiffness 自动匹配临界阻尼
		void setCritical(float stiffness) {
			setDynamics(stiffness, SpringValue::criticalDamping(stiffness));
		}

		// 获取当前位置和目标
		core::Vec2 value()  const { return { x.value,  y.value }; }
		core::Vec2 target() const { return { x.target, y.target }; }
	};

} // namespace engine
