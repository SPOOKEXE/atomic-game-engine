#include "Source2DComplexGenerator.hpp"
#include "SourceInterpret.hpp"

#include <numbers>
namespace engine::imagegraph::detail {
	namespace {
		constexpr uint64_t CUBE_PIXEL_WORK = 512 + 256 * 96;
		struct CubeMatrix {
			double Row[3][3]{};
		};
		CubeMatrix CubeMultiply(const CubeMatrix &a, const CubeMatrix &b) {
			CubeMatrix out{};
			for (size_t i = 0; i < 3; ++i)
				for (size_t j = 0; j < 3; ++j)
					out.Row[i][j] =
						a.Row[i][0] * b.Row[0][j] + a.Row[i][1] * b.Row[1][j] + a.Row[i][2] * b.Row[2][j];
			return out;
		}
		Vector3 CubeTransform(const CubeMatrix &m, Vector3 p) {
			return {
				m.Row[0][0] * p.X + m.Row[0][1] * p.Y + m.Row[0][2] * p.Z,
				m.Row[1][0] * p.X + m.Row[1][1] * p.Y + m.Row[1][2] * p.Z,
				m.Row[2][0] * p.X + m.Row[2][1] * p.Y + m.Row[2][2] * p.Z
			};
		}
		CubeMatrix CubeRotation(Vector3 angle) {
			const double x = angle.X * std::numbers::pi / 180, y = angle.Y * std::numbers::pi / 180,
						 z = angle.Z * std::numbers::pi / 180;
			const double cx = std::cos(x), sx = std::sin(x), cy = std::cos(y), sy = std::sin(y),
						 cz = std::cos(z), sz = std::sin(z);
			const CubeMatrix rx{{{1, 0, 0}, {0, cx, sx}, {0, -sx, cx}}},
				ry{{{cy, 0, -sy}, {0, 1, 0}, {sy, 0, cy}}}, rz{{{cz, sz, 0}, {-sz, cz, 0}, {0, 0, 1}}};
			return CubeMultiply(CubeMultiply(rx, ry), rz);
		}
		bool CubeInverse(NodeContext &c, const CubeMatrix &m, CubeMatrix &out) {
			// Names follow GLSL column indexing; row storage keeps the source cofactor formula explicit.
			const double a00 = m.Row[0][0], a01 = m.Row[1][0], a02 = m.Row[2][0], a10 = m.Row[0][1],
						 a11 = m.Row[1][1], a12 = m.Row[2][1], a20 = m.Row[0][2], a21 = m.Row[1][2],
						 a22 = m.Row[2][2];
			const double b01 = a22 * a11 - a12 * a21, b11 = -a22 * a10 + a12 * a20,
						 b21 = a21 * a10 - a11 * a20;
			const double det = a00 * b01 + a01 * b11 + a02 * b21;
			if (det == 0 || !std::isfinite(det))
				return c.Fail(
					Status::UnsupportedExecution, "Gradient Cube camera inverse is undefined", "rotation"
				);
			out = {
				{{b01, b11, b21},
				 {-a22 * a01 + a02 * a21, a22 * a00 - a02 * a20, -a21 * a00 + a01 * a20},
				 {a12 * a01 - a02 * a11, -a12 * a00 + a02 * a10, a11 * a00 - a01 * a10}}
			};
			for (auto &row : out.Row)
				for (auto &value : row) {
					value /= det;
					if (!std::isfinite(value))
						return c.Fail(
							Status::UnsupportedExecution,
							"Gradient Cube camera inverse is nonfinite",
							"rotation"
						);
				}
			return true;
		}
		bool CubeVec3(NodeContext &c, std::string_view port, Vector3 fallback, Vector3 &out) {
			const auto *value = c.Find(port);
			out = fallback;
			if (!value) return true;
			if (const auto *p = std::get_if<Vector3>(value))
				out = *p;
			else if (const auto *n = std::get_if<double>(value))
				out = {*n, *n, *n};
			else if (const auto *n = std::get_if<int64_t>(value))
				out = {double(*n), double(*n), double(*n)};
			else
				return c.Fail(
					Status::UnsupportedExecution, "Gradient Cube needs a resolved three-component tuple", port
				);
			return (std::isfinite(out.X) && std::isfinite(out.Y) && std::isfinite(out.Z)) ||
				   c.Fail(Status::InvalidValue, "Gradient Cube tuple must be finite", port);
		}
		struct CubePalette {
			std::array<Rgba, 8> Corner{};
		};
		template <class T> std::optional<Colour> CubeColor(const T &raw) {
			auto color = InterpretPackedColour(raw);
			// colToVec4 preserves packed alpha only for int64 source colors.
			if constexpr (!std::is_same_v<T, Colour> && !std::is_same_v<T, int64_t>)
				if (color) color->Alpha = 255;
			return color;
		}
		bool CubeColors(NodeContext &c, CubePalette &palette) {
			const auto *value = c.Find("colors");
			const auto *a = value ? std::get_if<ArrayValue>(value) : nullptr;
			size_t count = 1;
			if (a) {
				if (!a->Nested.empty())
					return c.Fail(
						Status::UnsupportedExecution,
						"Gradient Cube palette row nesting is not resolved",
						"colors"
					);
				count = a->Items.empty() ? a->Elements.size() : a->Items.size();
			}
			if (!count)
				return c.Fail(
					Status::UnsupportedExecution,
					"Gradient Cube empty palette leaves ambient shader uniforms",
					"colors"
				);
			if (count > 256)
				return c.Fail(
					Status::UnsupportedExecution,
					"Gradient Cube palette upload exceeds the named GLSL uniform profile",
					"colors"
				);
			for (size_t i = 0; i < 8; ++i) {
				std::optional<Colour> color;
				if (!value)
					color = i % 2 ? Colour{255, 255, 255, 255} : Colour{0, 0, 0, 255};
				else if (!a)
					color = std::visit([](const auto &raw) { return CubeColor(raw); }, *value);
				else {
					const ElementValue *leaf = a->Items.empty()
												   ? &a->Elements[i % count]
												   : std::get_if<ElementValue>(&a->Items[i % count].Data);
					if (leaf) color = std::visit([](const auto &raw) { return CubeColor(raw); }, *leaf);
				}
				if (!color)
					return c.Fail(
						Status::UnsupportedExecution,
						"Gradient Cube palette needs integral packed or typed colors",
						"colors"
					);
				palette.Corner[i] = {
					color->Red / 255., color->Green / 255., color->Blue / 255., color->Alpha / 255.
				};
			}
			return true;
		}
		Rgba CubeMix(Rgba a, Rgba b, double f) {
			for (size_t i = 0; i < 4; ++i)
				a[i] = a[i] + (b[i] - a[i]) * f;
			return a;
		}
		Rgba CubePaletteAt(const CubePalette &p, Vector3 point) {
			const auto &v = p.Corner;
			return CubeMix(
				CubeMix(CubeMix(v[0], v[1], point.Z), CubeMix(v[2], v[3], point.Z), point.Y),
				CubeMix(CubeMix(v[4], v[5], point.Z), CubeMix(v[6], v[7], point.Z), point.Y),
				point.X
			);
		}
		bool CubeMarch(NodeContext &c, Vector3 eye, Vector3 dir, int64_t shape, double &depth) {
			depth = 0;
			for (size_t i = 0; i < 256; ++i) {
				const Vector3 p{eye.X + depth * dir.X, eye.Y + depth * dir.Y, eye.Z + depth * dir.Z};
				double distance = 0;
				if (shape == 0) {
					const Vector3 q{std::abs(p.X) - .5, std::abs(p.Y) - .5, std::abs(p.Z) - .5},
						r{std::max(q.X, 0.), std::max(q.Y, 0.), std::max(q.Z, 0.)};
					distance = std::sqrt(r.X * r.X + r.Y * r.Y + r.Z * r.Z) +
							   std::min(std::max(q.X, std::max(q.Y, q.Z)), 0.);
				}
				if (shape == 1) distance = std::sqrt(p.X * p.X + p.Y * p.Y + p.Z * p.Z) - .5;
				if (!std::isfinite(distance))
					return c.Fail(
						Status::UnsupportedExecution,
						"Gradient Cube march distance exceeds native arithmetic range",
						"scale"
					);
				if (distance < 1e-5) return true;
				depth += distance;
				if (depth >= 10) {
					depth = 10;
					return true;
				}
			}
			depth = 10;
			return true;
		}
	}
	bool SourceGradientCube(NodeContext &c) {
		ENGINE_PROFILE("imagegraph.source.gradient_cube");
		if (!source2d::ComplexBatchAdmission(c, CUBE_PIXEL_WORK, "surface_out", "dimension", 2)) return false;
		const auto format = ResolveProcessorSurfaceFormat(c, nullptr);
		if (!format) return false;
		uint32_t width, height;
		if (!source2d::ResolveGeneratorDimensions(c, nullptr, width, height)) return false;
		const Vector2 raw = c.Vec2("dimension", {1, 1});
		if (raw.X <= 0 || raw.Y <= 0)
			return c.Fail(
				Status::UnsupportedExecution,
				"Gradient Cube nonpositive source quad coverage is unobserved",
				"dimension"
			);
		Vector3 rotation, objectRotation, objectScale;
		if (!CubeVec3(c, "rotation", {30, 45, 0}, rotation) ||
			!CubeVec3(c, "rotation_2", {}, objectRotation) || !CubeVec3(c, "scale_2", {1, 1, 1}, objectScale))
			return false;
		const double scale = c.Scalar("scale", 1), crossPosition = c.Scalar("position");
		const int64_t shape = c.Integer("shape"), axis = c.Integer("axis"), crossAxis = c.Integer("axis_2");
		if (c.FailureCode != Status::Ok) return false;
		CubePalette colors;
		if (!CubeColors(c, colors)) return false;
		CubeMatrix inverse;
		const auto camera = CubeRotation(rotation), object = CubeRotation(objectRotation);
		if (!CubeInverse(c, camera, inverse)) return false;
		const Vector3 direction = CubeTransform(inverse, {0, 0, -1});
		Image *main = c.NewImage("surface_out", width, height, *format);
		if (!main) return false;
		Image *cross = c.NewImage("cross_section", width, height, *format);
		if (!cross) return false;
		for (uint32_t y = 0; y < height; ++y)
			for (uint32_t x = 0; x < width; ++x) {
				const double u = (x + .5) / width, v = (y + .5) / height;
				const Vector3 eye = CubeTransform(inverse, {(u - .5) * 2 * scale, (v - .5) * 2 * scale, 5});
				double depth;
				if (!CubeMarch(c, eye, direction, shape, depth)) return false;
				Vector3 hit{
					eye.X + direction.X * depth + .5,
					eye.Y + direction.Y * depth + .5,
					eye.Z + direction.Z * depth + .5
				};
				if (axis == 1) hit = {hit.Y, hit.Z, hit.X};
				if (axis == 2) hit = {hit.Z, hit.X, hit.Y};
				hit = CubeTransform(object, hit);
				hit.X *= objectScale.X;
				hit.Y *= objectScale.Y;
				hit.Z *= objectScale.Z;
				Rgba col = CubePaletteAt(colors, hit);
				col[3] = depth < 10 ? 1 : 0;
				Vector3 slice{};
				if (crossAxis == 0) slice = {crossPosition, u, v};
				if (crossAxis == 1) slice = {u, crossPosition, v};
				if (crossAxis == 2) slice = {u, v, crossPosition};
				slice = CubeTransform(object, slice);
				slice.X *= objectScale.X;
				slice.Y *= objectScale.Y;
				slice.Z *= objectScale.Z;
				if (!source2d::StoreComplexPixel(c, *main, x, y, col, "surface_out") ||
					!source2d::StoreComplexPixel(
						c, *cross, x, y, CubePaletteAt(colors, slice), "cross_section"
					))
					return false;
			}
		return c.FailureCode == Status::Ok;
	}
}
