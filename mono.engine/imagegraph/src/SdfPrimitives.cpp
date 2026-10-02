#include "SdfMath.hpp"
#include "SdfPayload.hpp"

// Pixel Composer sh_rm_primitive distance expressions, b69eca232217360cf1502ef0223523d818606652.
// Copyright (c) 2023 Tanasart. MIT notice: docs/pixel-composer-m0/PixelComposer-LICENSE.txt.
namespace engine::imagegraph::detail::sdf_math {
	float sdRegularPolygon(V2 p, float r, int n) {

		float an = 3.141593f / float(n);
		V2 acs = V2(cos(an), sin(an));

		float bn = mod(atan(p.x, p.y), 2.0f * an) - an;
		p = length(p) * V2(cos(bn), abs(sin(bn)));

		p -= r * acs;
		p.y += clamp(-p.y, 0.0f, r * acs.y);
		return length(p) * sign(p.x);
	}

	float sdPie(V2 p, float angle, float r) {
		V2 c = V2(sin(angle), cos(angle));

		p.x = abs(p.x);
		float l = length(p) - r;
		float m = length(p - c * clamp(dot(p, c), 0.0f, r));
		return max(l, m * sign(c.y * p.x - c.x * p.y));
	}

	float sdPlane(V3 p, V3 n, float h) {
		return dot(p, n) + h;
	}

	float sdBox(V3 p, V3 b) {
		V3 q = abs(p) - b;
		return length(max(q, 0.0f)) + min(max(q.x, max(q.y, q.z)), 0.0f);
	}

	float sdBoxFrame(V3 p, V3 b, float e) {
		p = abs(p) - b;
		V3 q = abs(p + e) - e;
		return min(
			min(length(max(V3(p.x, q.y, q.z), 0.0f)) + min(max(p.x, max(q.y, q.z)), 0.0f),
				length(max(V3(q.x, p.y, q.z), 0.0f)) + min(max(q.x, max(p.y, q.z)), 0.0f)),
			length(max(V3(q.x, q.y, p.z), 0.0f)) + min(max(q.x, max(q.y, p.z)), 0.0f)
		);
	}

	float sdSphere(V3 p, float radius) {
		return length(p) - radius;
	}

	float sdEllipsoid(V3 p, V3 r) {
		float k0 = length(p / r);
		float k1 = length(p / (r * r));
		return k0 * (k0 - 1.0f) / k1;
	}

	float sdTorus(V3 p, V2 t) {
		V2 q = V2(length(p.xz()) - t.x, p.y);
		return length(q) - t.y;
	}

	float sdCutSphere(V3 p, float r, float h) {

		float w = sqrt(r * r - h * h);

		V2 q = V2(length(p.xz()), p.y);
		float s = max((h - r) * q.x * q.x + w * w * (h + r - 2.0f * q.y), h * q.x - w * q.y);
		return (s < 0.0f) ? length(q) - r : (q.x < w) ? h - q.y : length(q - V2(w, h));
	}

	float sdCutHollowSphere(V3 p, float r, float h, float t) {

		float w = sqrt(r * r - h * h);

		V2 q = V2(length(p.xz()), p.y);
		return ((h * q.x < w * q.y) ? length(q - V2(w, h)) : abs(length(q) - r)) - t;
	}

	float sdCappedTorus(V3 p, float an, float ra, float rb) {
		V2 sc = V2(sin(an), cos(an));

		p.x = abs(p.x);
		float k = (sc.y * p.x > sc.x * p.y) ? dot(p.xy(), sc) : length(p.xy());
		return sqrt(dot(p, p) + ra * ra - 2.0f * ra * k) - rb;
	}

	float sdCylinder(V3 p, V3 c) {
		return length(p.xz() - c.xy()) - c.z;
	}

	float sdCappedCylinder(V3 p, float h, float r) {
		V2 d = abs(V2(length(p.xz()), p.y)) - V2(r, h);
		return min(max(d.x, d.y), 0.0f) + length(max(d, 0.0f));
	}

	float sdCapsule(V3 p, V3 a, V3 b, float r) {
		V3 pa = p - a, ba = b - a;
		float h = clamp(dot(pa, ba) / dot(ba, ba), 0.0f, 1.0f);
		return length(pa - ba * h) - r;
	}

	float sdCone(V3 p, float an, float h) {
		V2 c = V2(sin(an), cos(an));

		V2 q = h * V2(c.x / c.y, -1.0f);

		V2 w = V2(length(p.xz()), p.y);
		V2 a = w - q * clamp(dot(w, q) / dot(q, q), 0.0f, 1.0f);
		V2 b = w - q * V2(clamp(w.x / q.x, 0.0f, 1.0f), 1.0f);
		float k = sign(q.y);
		float d = min(dot(a, a), dot(b, b));
		float s = max(k * (w.x * q.y - w.y * q.x), k * (w.y - q.y));
		return sqrt(d) * sign(s);
	}

	float sdCappedCone(V3 p, float h, float r1, float r2) {
		V2 q = V2(length(p.xz()), p.y);
		V2 k1 = V2(r2, h);
		V2 k2 = V2(r2 - r1, 2.0f * h);
		V2 ca = V2(q.x - min(q.x, (q.y < 0.0f) ? r1 : r2), abs(q.y) - h);
		V2 cb = q - k1 + k2 * clamp(dot(k1 - q, k2) / dot2(k2), 0.0f, 1.0f);
		float s = (cb.x < 0.0f && ca.y < 0.0f) ? -1.0f : 1.0f;
		return s * sqrt(min(dot2(ca), dot2(cb)));
	}

	float sdRoundCone(V3 p, float h, float r1, float r2) {

		float b = (r1 - r2) / h;
		float a = sqrt(1.0f - b * b);

		V2 q = V2(length(p.xz()), p.y);
		float k = dot(q, V2(-b, a));
		if (k < 0.0f) return length(q) - r1;
		if (k > a * h) return length(q - V2(0.0f, h)) - r2;
		return dot(q, V2(a, b)) - r1;
	}

	float sdSolidAngle(V3 p, float an, float ra) {
		V2 c = V2(sin(an), cos(an));
		V2 q = V2(length(p.xz()), p.y);
		float l = length(q) - ra;
		float m = length(q - c * clamp(dot(q, c), 0.0f, ra));
		return max(l, m * sign(c.y * q.x - c.x * q.y));
	}

	float sdOctahedron(V3 p, float s) {
		p = abs(p);
		float m = p.x + p.y + p.z - s;
		V3 q;
		if (3.0f * p.x < m)
			q = p;
		else if (3.0f * p.y < m)
			q = p.yzx();
		else if (3.0f * p.z < m)
			q = p.zxy();
		else
			return m * 0.57735027f;

		float k = clamp(0.5f * (q.z - q.y + s), 0.0f, s);
		return length(V3(q.x, q.y - s + k, q.z - k));
	}

	float sdPyramid(V3 p, float h) {
		float m2 = h * h + 0.25f;

		p.x = abs(p.x);
		p.z = abs(p.z);
		if (p.z > p.x) std::swap(p.x, p.z);
		p.x -= .5f;
		p.z -= .5f;

		V3 q = V3(p.z, h * p.y - 0.5f * p.x, h * p.x + 0.5f * p.y);

		float s = max(-q.x, 0.0f);
		float t = clamp((q.y - 0.5f * p.z) / (m2 + 0.25f), 0.0f, 1.0f);

		float a = m2 * (q.x + s) * (q.x + s) + q.y * q.y;
		float b = m2 * (q.x + 0.5f * t) * (q.x + 0.5f * t) + (q.y - m2 * t) * (q.y - m2 * t);

		float d2 = min(q.y, -q.x * m2 - q.y * 0.5f) > 0.0f ? 0.0f : min(a, b);

		return sqrt((d2 + q.z * q.z) / m2) * sign(max(q.z, -p.y));
	}

}

namespace engine::imagegraph::detail::sdf_math {
	float Primitive(int32_t shape, V3 p, const SourceSdfShape &data) {
		const float radius = static_cast<float>(data.Radius), thickness = static_cast<float>(data.Thickness);
		const float crop = static_cast<float>(data.Crop), angle = static_cast<float>(data.Angle),
					height = static_cast<float>(data.Height);
		const V3 size{
			static_cast<float>(data.Size.X), static_cast<float>(data.Size.Y), static_cast<float>(data.Size.Z)
		};
		switch (shape) {
		case 100:
			return -p.y;
		case 101:
			return sdBox(p, size / 2.f);
		case 102:
			return sdBoxFrame(p, size / 2.f, thickness);
		case 103: {
			const float corner = static_cast<float>(
				p.x > 0 ? (p.y > 0 ? data.Corner.X : data.Corner.Y)
						: (p.y > 0 ? data.Corner.Z : data.Corner.W)
			);
			const V2 q = abs(p.xy()) -
						 V2{static_cast<float>(data.Size2D.X), static_cast<float>(data.Size2D.Y)} + corner;
			const float rounded = min(max(q.x, q.y), 0.f) + length(max(q, 0.f)) - corner;
			const V2 w{rounded, abs(p.z) - thickness};
			return min(max(w.x, w.y), 0.f) + length(max(w, 0.f));
		}
		case 200:
			return sdSphere(p, radius);
		case 201:
			return sdEllipsoid(p, size / 2.f);
		case 202:
			return sdCutSphere(p, radius, crop);
		case 203:
			return sdCutHollowSphere(p, radius, crop, thickness);
		case 204:
			return sdTorus(p, V2{radius, thickness});
		case 205:
			return sdCappedTorus(p, angle, radius, thickness);
		case 300:
			return sdCappedCylinder(p, height, radius);
		case 301:
			return sdCapsule(p, V3{-height, 0, 0}, V3{height, 0, 0}, radius);
		case 302:
			return sdCone(p, angle, height);
		case 303:
			return sdCappedCone(
				p, height, static_cast<float>(data.RadiusRange.X), static_cast<float>(data.RadiusRange.Y)
			);
		case 304:
			return sdRoundCone(
				p, height, static_cast<float>(data.RadiusRange.X), static_cast<float>(data.RadiusRange.Y)
			);
		case 305:
			return sdSolidAngle(p, angle, radius);
		case 306: {
			const V2 w{sdRegularPolygon(p.xy(), .5f, data.Sides), abs(p.z) - thickness};
			return min(max(w.x, w.y), 0.f) + length(max(w, 0.f));
		}
		case 307: {
			const V2 w{sdPie(p.xy(), angle, radius), abs(p.z) - thickness};
			return min(max(w.x, w.y), 0.f) + length(max(w, 0.f));
		}
		case 400:
			return sdOctahedron(p, static_cast<float>(data.UniformSize));
		case 401:
			return sdPyramid(p, static_cast<float>(data.UniformSize));
		default:
			return std::numeric_limits<float>::quiet_NaN();
		}
	}
}
