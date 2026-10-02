#include "Curve.hpp"
#include "Source2DGenerator.hpp"

#include <numbers>

namespace engine::imagegraph::detail {
	bool SourceCosGradient(NodeContext &context) {
		const int64_t type = context.Integer("type");
		if (type < 0 || type > 3)
			return context.Fail(Status::InvalidValue, "Cos Gradient shape is invalid", "type");
		uint32_t width = 0, height = 0;
		const Image *mask = context.Input("mask");
		if (context.Integer("dimension_unit", 1) == 2) {
			if (!mask) return context.Fail(Status::InvalidValue, "Mask dimensions require a mask", "mask");
			width = mask->Width;
			height = mask->Height;
		} else if (!ResolveDimension(context, "dimension", width, height))
			return false;
		Vector2 dimension{double(width), double(height)};
		if (context.Integer("dimension_unit", 1) != 2) {
			dimension = context.Vec2("dimension", {1, 1});
			if (!context.IsLinked("dimension") && context.Integer("dimension_unit", 1) == 1) {
				dimension.X *= context.Project.SurfaceWidth;
				dimension.Y *= context.Project.SurfaceHeight;
			}
		}
		Vector2 center = context.Vec2("center", {.5, .5});
		if (!context.IsLinked("center") && context.Integer("center_unit", 1) == 1) {
			center.X *= dimension.X;
			center.Y *= dimension.Y;
		}
		const Vector2 shape = context.Vec2("shape", {1, 1});
		const double scale = context.Scalar("scale", 1),
					 radius = context.Scalar("radius", .5) * std::sqrt(2.),
					 angle = context.Scalar("angle") * std::numbers::pi / 180;
		if (dimension.X == 0 || dimension.Y == 0 || scale == 0 ||
			((type == 1 || type == 3) && (radius == 0 || shape.X == 0 || shape.Y == 0)))
			return context.Fail(
				Status::UnsupportedExecution, "Cos Gradient source geometry division is undefined", "scale"
			);
		center.X /= dimension.X;
		center.Y /= dimension.Y;
		const Value *curveValue = context.Find("progress_remap");
		const Curve *curve = curveValue ? std::get_if<Curve>(curveValue) : nullptr;
		if (!curve)
			return context.Fail(
				Status::InvalidValue, "Cos Gradient requires its progress curve", "progress_remap"
			);
		if (curve->Anchors.size() > 9)
			return context.Fail(
				Status::UnsupportedExecution,
				"Cos Gradient GLSL curve uniform has only 64 slots; larger HLSL curves require a backend "
				"observation",
				"progress_remap"
			);
		if (curve->Header[1] == 0)
			return context.Fail(
				Status::UnsupportedExecution,
				"Cos Gradient source curve division is undefined",
				"progress_remap"
			);
		if (uint64_t(width) * height > 64000000 / (1 + curve->Anchors.size()))
			return context.Fail(
				Status::LimitExceeded, "Cos Gradient progress curve exceeds work budget", "dimension"
			);
		const std::array<std::string_view, 4> names{"a", "b", "c", "d"};
		const std::array<Vector3, 4> defaults{{{.5, .5, .5}, {.5, .5, .5}, {.8, .8, .8}, {.21, .54, .88}}};
		std::array<Vector3, 4> coefficients{};
		std::array<const Image *, 4> maps{};
		for (size_t i = 0; i < 4; ++i) {
			coefficients[i] = context.Get<Vector3>(names[i], defaults[i]);
			const std::string name(names[i]);
			if (context.Boolean(name + "_mapped")) {
				maps[i] = context.Input(name + "_map");
				if (!maps[i])
					return context.Fail(
						Status::InvalidValue,
						"Cos Gradient mapped coefficient requires a surface",
						name + "_map"
					);
			}
		}
		const Vector3 mappedMax = context.Get<Vector3>("a_max", {.5, .5, .5});
		const auto format = ResolveProcessorSurfaceFormat(context, nullptr);
		if (!format) return false;
		Image *output = context.NewImage("surface_out", width, height, *format);
		if (!output) return false;
		for (uint32_t y = 0; y < height; ++y)
			for (uint32_t x = 0; x < width; ++x) {
				const double u = (x + .5) / width, v = (y + .5) / height;
				double alpha = 1;
				const Vector2 uv = source2d::GeneratorUv(context, u, v, alpha);
				std::array<Vector3, 4> co = coefficients;
				for (size_t i = 0; i < 4; ++i)
					if (maps[i]) {
						const Rgba sample = SampleNearest(*maps[i], u, v);
						const double amount = (sample[0] + sample[1] + sample[2]) / 3;
						// The pinned shader uses co_a_max for every coefficient's mapped endpoint.
						co[i] = {
							co[i].X + (mappedMax.X - co[i].X) * amount,
							co[i].Y + (mappedMax.Y - co[i].Y) * amount,
							co[i].Z + (mappedMax.Z - co[i].Z) * amount
						};
					}
				Vector2 point{uv.X - center.X, uv.Y - center.Y};
				double progress = 0;
				if (type == 0)
					progress = .5 + point.X * std::cos(angle) - point.Y * std::sin(angle);
				else {
					if (type == 3)
						point = {
							point.X * std::cos(angle) - point.Y * std::sin(angle),
							point.X * std::sin(angle) + point.Y * std::cos(angle)
						};
					if (context.Boolean("uniform_ratio", true)) point.X *= dimension.X / dimension.Y;
					if (type == 2) {
						if (point.X == 0 && point.Y == 0) {
							context.Fail(
								Status::UnsupportedExecution,
								"Cos Gradient source radial angle is undefined at its center",
								"center"
							);
							return false;
						}
						const double a = std::atan2(point.Y, point.X) + angle;
						progress =
							(a - std::floor(a / 6.283185307179586) * 6.283185307179586) / 6.283185307179586;
					} else {
						point.X /= shape.X;
						point.Y /= shape.Y;
						progress = (type == 1 ? std::hypot(point.X, point.Y)
											  : std::abs(point.X) + std::abs(point.Y)) /
								   radius;
					}
				}
				progress = EvalShaderCurve(*curve, (progress + context.Scalar("shift") - .5) / scale + .5);
				const Rgba colour{
					co[0].X + co[1].X * std::cos(6.28318 * (co[2].X * progress + co[3].X)),
					co[0].Y + co[1].Y * std::cos(6.28318 * (co[2].Y * progress + co[3].Y)),
					co[0].Z + co[1].Z * std::cos(6.28318 * (co[2].Z * progress + co[3].Z)),
					alpha
				};
				// The generator submits mask uniforms but never reads them in its source shader.
				if (!WritePixel(*output, x, y, colour))
					return context.Fail(
						Status::InvalidValue, "Cos Gradient sample exceeds surface range", "surface_out"
					);
			}
		return context.FailureCode == Status::Ok;
	}
}
