#pragma once
#include "MeshPayload.hpp"

#include <numbers>
namespace engine::imagegraph::detail {
	inline Quaternion SourceQuaternionFromEuler(double x, double y, double z) {
		const double k = -std::numbers::pi / 360;
		x *= k;
		y *= k;
		z *= k;
		const double qx = std::cos(z) * std::sin(x), qy = std::sin(z) * std::sin(x),
					 qz = std::sin(z) * std::cos(x), qw = std::cos(z) * std::cos(x);
		return {
			qx * std::cos(y) - qz * std::sin(y),
			qw * std::sin(y) + qy * std::cos(y),
			qz * std::cos(y) + qx * std::sin(y),
			qw * std::cos(y) - qy * std::sin(y)
		};
	}
	inline Quaternion SourceQuaternionMultiply(Quaternion a, Quaternion b) {
		return {
			a.W * b.X + a.X * b.W + a.Y * b.Z - a.Z * b.Y,
			a.W * b.Y - a.X * b.Z + a.Y * b.W + a.Z * b.X,
			a.W * b.Z + a.X * b.Y - a.Y * b.X + a.Z * b.W,
			a.W * b.W - a.X * b.X - a.Y * b.Y - a.Z * b.Z
		};
	}
	inline Vector3 SourceQuaternionToEuler(Quaternion q) {
		const double k = 180 / std::numbers::pi;
		auto round = [](double n) {
			double f = std::floor(n), r = n - f;
			return (r < .5 ? f : (r > .5 ? f + 1 : (std::fmod(f, 2) == 0 ? f : f + 1))) / 1000;
		};
		return {
			round(std::atan2(2 * (q.W * q.X + q.Y * q.Z), 1 - 2 * (q.X * q.X + q.Y * q.Y)) * k * 1000),
			round(std::asin(std::clamp(2 * (q.W * q.Y - q.Z * q.X), -1.0, 1.0)) * k * 1000),
			round(std::atan2(2 * (q.W * q.Z + q.X * q.Y), 1 - 2 * (q.Y * q.Y + q.Z * q.Z)) * k * 1000)
		};
	}
	inline Quaternion SourceQuaternionLook(Vector3 forward) {
		auto unit = [](Vector3 v) {
			double l = std::hypot(v.X, v.Y, v.Z);
			return l ? Vector3{v.X / l, v.Y / l, v.Z / l} : v;
		};
		forward = unit(forward);
		Vector3 up{0, 0, 1};
		const double dot = forward.Z;
		up = {up.X - forward.X * dot, up.Y - forward.Y * dot, up.Z - forward.Z * dot};
		if (std::hypot(up.X, up.Y, up.Z) == 0) return {};
		up = unit(up);
		Vector3 right{
			up.Y * forward.Z - up.Z * forward.Y,
			up.Z * forward.X - up.X * forward.Z,
			up.X * forward.Y - up.Y * forward.X
		};
		double w = std::sqrt(std::abs(1 + right.X + up.Y + forward.Z)) * .5;
		if (w >= .0001) {
			double r = 1 / (4 * w);
			return {(up.Z - forward.Y) * r, (forward.X - right.Z) * r, (right.Y - up.X) * r, w};
		}
		if (right.X > up.Y && right.X > forward.Z) {
			double x = std::sqrt(std::abs(1 + right.X - up.Y - forward.Z)) * .5, r = 1 / (4 * x);
			return {x, (right.Y + up.X) * r, (forward.X + right.Z) * r, (up.Z - forward.Y) * r};
		}
		if (up.Y > forward.Z) {
			double y = std::sqrt(std::abs(1 + up.Y - right.X - forward.Z)) * .5, r = 1 / (4 * y);
			return {(right.Y + up.X) * r, y, (up.Z + forward.Y) * r, (forward.X - right.Z) * r};
		}
		double z = std::sqrt(std::abs(1 + forward.Z - right.X - up.Y)) * .5, r = 1 / (4 * z);
		return {(forward.X + right.Z) * r, (up.Z + forward.Y) * r, z, (right.Y - up.X) * r};
	}
}
