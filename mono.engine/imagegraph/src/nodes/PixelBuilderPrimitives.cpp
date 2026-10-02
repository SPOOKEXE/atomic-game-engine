#include "PixelBuilderPrimitives.hpp"

#include <algorithm>
#include <cmath>

namespace engine::imagegraph::detail {
	namespace {
		double Cross(Vector2 a, Vector2 b, Vector2 p) {
			return (b.X - a.X) * (p.Y - a.Y) - (b.Y - a.Y) * (p.X - a.X);
		}
		bool IncludedEdge(Vector2 a, Vector2 b) {
			return b.Y < a.Y || (b.Y == a.Y && b.X > a.X);
		}
		struct PrimitiveRaster {
			NodeContext &Context;
			Image &Shape;
			uint64_t Work = 0;
			bool Failed = false;
			std::array<double, 4> Clip;
			void Triangle(Vector2 a, Vector2 b, Vector2 c) {
				if (Failed) return;
				for (double value : {a.X, a.Y, b.X, b.Y, c.X, c.Y})
					if (!std::isfinite(value)) {
						Failed = !Context.Fail(Status::InvalidValue, "PB primitive vertices must be finite");
						return;
					}
				const double area = Cross(a, b, c);
				if (!std::isfinite(area)) {
					Failed = !Context.Fail(Status::InvalidValue, "PB primitive area is nonfinite");
					return;
				}
				if (area == 0) return;
				if (area < 0) std::swap(b, c);
				const uint32_t left =
					uint32_t(std::clamp(std::floor(std::min({a.X, b.X, c.X})), 0.0, double(Shape.Width)));
				const uint32_t top =
					uint32_t(std::clamp(std::floor(std::min({a.Y, b.Y, c.Y})), 0.0, double(Shape.Height)));
				const uint32_t right =
					uint32_t(std::clamp(std::ceil(std::max({a.X, b.X, c.X})), 0.0, double(Shape.Width)));
				const uint32_t bottom =
					uint32_t(std::clamp(std::ceil(std::max({a.Y, b.Y, c.Y})), 0.0, double(Shape.Height)));
				const uint64_t work = uint64_t(right - left) * (bottom - top);
				if (work > 64000000 - Work) {
					Failed = !Context.Fail(Status::LimitExceeded, "PB primitive raster exceeds work budget");
					return;
				}
				Work += work;
				for (uint32_t y = top; y < bottom; ++y)
					for (uint32_t x = left; x < right; ++x) {
						const Vector2 pixel{x + .5, y + .5};
						if (pixel.X < Clip[0] || pixel.Y < Clip[1] || pixel.X >= Clip[2] ||
							pixel.Y >= Clip[3])
							continue;
						const double ab = Cross(a, b, pixel), bc = Cross(b, c, pixel),
									 ca = Cross(c, a, pixel);
						if ((ab > 0 || (ab == 0 && IncludedEdge(a, b))) &&
							(bc > 0 || (bc == 0 && IncludedEdge(b, c))) &&
							(ca > 0 || (ca == 0 && IncludedEdge(c, a))))
							WritePixel(Shape, x, y, {1, 1, 1, 1});
					}
			}
			void Quad(Vector2 a, Vector2 b, Vector2 c, Vector2 d) {
				Triangle(a, b, c);
				Triangle(b, c, d);
			}
			void Line(Vector2 a, Vector2 b, double width) {
				if (!std::isfinite(width) || std::abs(width) > Limits::MaximumDimension) {
					Failed = !Context.Fail(Status::LimitExceeded, "PB line width exceeds bounded canvas");
					return;
				}
				const double length = std::hypot(b.X - a.X, b.Y - a.Y);
				if (length == 0) {
					const double radius = width / 2;
					Quad(
						{a.X - radius, a.Y - radius},
						{a.X + radius, a.Y - radius},
						{a.X - radius, a.Y + radius},
						{a.X + radius, a.Y + radius}
					);
					return;
				}
				const Vector2 normal{-(b.Y - a.Y) * width / (2 * length), (b.X - a.X) * width / (2 * length)};
				Quad(
					{a.X + normal.X, a.Y + normal.Y},
					{b.X + normal.X, b.Y + normal.Y},
					{a.X - normal.X, a.Y - normal.Y},
					{b.X - normal.X, b.Y - normal.Y}
				);
			}
			void Arc(Vector2 center, double rx, double ry, double from, double to, uint32_t precision) {
				Vector2 previous{};
				for (uint32_t index = 0; index <= precision; ++index) {
					const double angle = (from + (to - from) * index / precision) * std::acos(-1.0) / 180;
					const Vector2 current{center.X + std::cos(angle) * rx, center.Y - std::sin(angle) * ry};
					if (index) Triangle(center, previous, current);
					previous = current;
				}
			}
		};
		Vector2 Radial(Vector2 center, double rx, double ry, double degrees) {
			const double radians = degrees * std::acos(-1.0) / 180;
			return {center.X + rx * std::cos(radians), center.Y - ry * std::sin(radians)};
		}
	}
	bool
	RasterPixelBuilderPrimitive(NodeContext &context, const std::array<double, 4> &bounds, Image &shape) {
		if (context.Request.RequireSourceGpuRasterCoverage)
			return context.Fail(
				Status::UnsupportedExecution,
				"PB primitive requires captured GameMaker GPU coverage for this request"
			);
		PrimitiveRaster raster{context, shape, 0, false, {0, 0, double(shape.Width), double(shape.Height)}};
		PixelBuilderDrawState isolated{context.Request.PixelBuilderCirclePrecision};
		PixelBuilderDrawState &state = context.PixelBuilderDrawing ? *context.PixelBuilderDrawing : isolated;
		if (state.CirclePrecision < 4 || state.CirclePrecision > 64 || state.CirclePrecision % 4 != 0)
			return context.Fail(Status::InvalidValue, "PB circle precision must be 4..64 and divisible by 4");
		const double x0 = bounds[0] - 1, y0 = bounds[1] - 1, x1 = bounds[2] - 1, y1 = bounds[3] - 1;
		const double w = x1 - x0, h = y1 - y0;
		if (w == 0 || h == 0) return true;
		const Vector2 center{(x0 + x1) / 2, (y0 + y1) / 2};
		const auto type = std::string_view(context.Authored.Type);
		if (type == "pc.pb_draw_ellipse") {
			if (w <= 1 || h <= 1)
				raster.Line({x0, y0}, {x1, y1}, 1);
			else if (w <= 2 || h <= 2)
				raster.Quad({x0 + 1, y0 + 1}, {x1 + 1, y0 + 1}, {x0 + 1, y1 + 1}, {x1 + 1, y1 + 1});
			else
				raster.Arc(center, w / 2, h / 2, 0, 360, state.CirclePrecision);
		} else if (type == "pc.pb_draw_pie") {
			const int64_t corner = context.Integer("corner");
			const Vector2 origin{corner == 1 || corner == 3 ? x1 : x0, corner >= 2 ? y1 : y0};
			raster.Clip = {x0 + 1, y0 + 1, x1 + 1, y1 + 1};
			raster.Arc(origin, w, h, 0, 360, state.CirclePrecision);
		} else if (type == "pc.pb_draw_polygon" || type == "pc.pb_draw_star") {
			const int64_t sides = context.Integer("sides", 3);
			if (sides < 1 || sides > 4096)
				return context.Fail(Status::LimitExceeded, "PB polygon sides must be 1..4096", "sides");
			const double angle = context.Scalar("angle"), step = 360.0 / sides;
			const bool star = type == "pc.pb_draw_star";
			const int64_t mode = star ? context.Integer("mode") : 0;
			if (mode == 0) {
				const double inner = context.Scalar("inner_radius", .5);
				for (int64_t index = 0; index < sides; ++index) {
					const Vector2 first = Radial(center, w / 2, h / 2, angle + index * step),
								  last = Radial(center, w / 2, h / 2, angle + (index + 1) * step);
					if (star) {
						const auto middle =
							Radial(center, w * inner / 2, h * inner / 2, angle + (index + .5) * step);
						raster.Triangle(center, first, middle);
						raster.Triangle(center, middle, last);
					} else
						raster.Triangle(center, first, last);
				}
			} else {
				const double thickness = context.Scalar("thickness", 1);
				for (int64_t start = 0; start < (sides % 2 == 0 ? 2 : 1); ++start) {
					int64_t point = start;
					Vector2 previous{};
					for (int64_t index = 0; index <= sides; ++index) {
						const auto current = Radial(center, w / 2, h / 2, angle + point * step);
						point = (point + 2) % sides;
						if (index) raster.Line(previous, current, thickness);
						previous = current;
					}
				}
			}
		} else if (type == "pc.pb_draw_quadrilateral") {
			const auto point = [&](std::string_view port, Vector2 fallback) {
				const auto value = context.Vec2(port, fallback);
				return Vector2{x0 + w * value.X, y0 + h * value.Y};
			};
			raster.Quad(
				point("top_left", {0, 0}),
				point("top_right", {1, 0}),
				point("bottom_left", {0, 1}),
				point("bottom_right", {1, 1})
			);
		} else if (type == "pc.pb_draw_trapezoid") {
			const int64_t axis = context.Integer("axis");
			const double side = context.Scalar("side", .5) / 2;
			raster.Quad(
				{bounds[0] + (axis == 0) * side * w, bounds[1] + (axis == 3) * side * h},
				{bounds[2] - (axis == 0) * side * w, bounds[1] + (axis == 2) * side * h},
				{bounds[0] + (axis == 1) * side * w, bounds[3] - (axis == 3) * side * h},
				{bounds[2] - (axis == 1) * side * w, bounds[3] - (axis == 2) * side * h}
			);
		} else if (type == "pc.pb_draw_triangle") {
			const int64_t mode = context.Integer("mode"), base = context.Integer("base"),
						  corner = context.Integer("apex_corner");
			const double apex = context.Scalar("apex_ratio", .5);
			Vector2 a{x0, y0}, b{x1, y1}, c{x0, y0};
			if (mode == 0) {
				switch (base) {
				case 0:
					a = {x0, y1};
					b = {x1, y1};
					c = {x0 + w * apex, y0};
					break;
				case 1:
					a = {x0, y0};
					b = {x0, y1};
					c = {x1, y0 + h * apex};
					break;
				case 2:
					a = {x0, y0};
					b = {x1, y0};
					c = {x0 + w * apex, y1};
					break;
				case 3:
					a = {x1, y0};
					b = {x1, y1};
					c = {x0, y0 + h * apex};
					break;
				}
			} else {
				switch (corner) {
				case 0:
					a = {x1, y1 - h * apex};
					b = {x1 - w * apex, y1};
					c = {x0, y0};
					break;
				case 1:
					a = {x0, y1 - h * apex};
					b = {x0 + w * apex, y1};
					c = {x1, y0};
					break;
				case 2:
					a = {x0, y0 + h * apex};
					b = {x0 + w * apex, y0};
					c = {x1, y1};
					break;
				case 3:
					a = {x1, y0 + h * apex};
					b = {x1 - w * apex, y0};
					c = {x0, y1};
					break;
				}
			}
			raster.Triangle(a, b, c);
		} else if (type == "pc.pb_draw_line" || type == "pc.pb_draw_curve") {
			const bool curve = type == "pc.pb_draw_curve";
			const int64_t kinds = context.Integer("type"), thickness = context.Integer("thickness", 1),
						  corners = context.Integer("corner");
			if (std::abs(double(thickness)) > Limits::MaximumDimension)
				return context.Fail(
					Status::LimitExceeded, "PB line thickness exceeds bounded canvas", "thickness"
				);
			const double half = std::floor(thickness / 2.0);
			if (!context.Boolean("overflow", curve)) raster.Clip = {x0 + 1, y0 + 1, x1 + 1, y1 + 1};
			for (int64_t kind = 0; kind < 6; ++kind) {
				if (curve ? kinds != kind : !(kinds & (int64_t{1} << kind))) continue;
				Vector2 a{x0, y0}, b{x1, y1};
				double direction = 0, bend = context.Scalar("bend", .5);
				switch (kind) {
				case 0:
					a = {x0, y1 - half};
					b = {x1, y1 - half};
					direction = 90;
					bend *= h;
					break;
				case 1:
					a = {x0, y0 + thickness % 2 + half};
					b = {x1, y0 + thickness % 2 + half};
					direction = -90;
					bend *= h;
					break;
				case 2:
					a = {x0 + thickness % 2 + half, y0};
					b = {x0 + thickness % 2 + half, y1};
					direction = 0;
					bend *= w;
					break;
				case 3:
					a = {x1 - half, y0};
					b = {x1 - half, y1};
					direction = 180;
					bend *= w;
					break;
				case 4:
					a = {x1, y0};
					b = {x0, y1};
					direction = 135;
					bend *= std::hypot(w, h) / 2;
					break;
				case 5:
					a = {x0, y0};
					b = {x1, y1};
					direction = 45;
					bend *= std::hypot(w, h) / 2;
					break;
				}
				if (!curve) {
					if (kind >= 4) {
						a.X += half * bool(corners & 01);
						a.Y += half * bool(corners & 01);
						b.X -= half * bool(corners & 10);
						b.Y -= half * bool(corners & 10);
					}
					raster.Line(a, b, double(thickness));
				} else {
					const int64_t segments = context.Integer("segments", 8);
					if (segments < 1 || segments > 4096)
						return context.Fail(
							Status::LimitExceeded, "PB curve segments must be 1..4096", "segments"
						);
					Vector2 previous{};
					for (int64_t index = 0; index <= segments; ++index) {
						const double ratio = double(index) / segments,
									 offset =
										 std::sqrt(std::max(0.0, 1 - std::pow((ratio - .5) * 2, 2))) * bend;
						const Vector2 current = Radial(
							{a.X + (b.X - a.X) * ratio, a.Y + (b.Y - a.Y) * ratio}, offset, offset, direction
						);
						if (index) {
							raster.Line(previous, current, double(thickness));
							if (thickness != 1) {
								state.CirclePrecision = 8;
								raster.Arc(previous, thickness / 2.0, thickness / 2.0, 0, 360, 8);
								raster.Arc(current, thickness / 2.0, thickness / 2.0, 0, 360, 8);
							}
						}
						previous = current;
					}
				}
			}
		} else if (type == "pc.pb_draw_round_rectangle") {
			const auto radii = context.Get<Vector4>("corner_radius", {1, 1, 1, 1});
			std::array<double, 4> radius{
				std::floor(radii.X), std::floor(radii.Y), std::floor(radii.Z), std::floor(radii.W)
			};
			if (context.Boolean("clamp"))
				for (auto &value : radius)
					value = std::min({value, w / 2 + 1, h / 2 + 1});
			const bool sharp = context.Integer("profile") == 1;
			const double tl = radius[0], tr = radius[1], bl = radius[2], br = radius[3];
			const std::array<Vector2, 4> corners{
				Vector2{x0 + tl, y0 + tl},
				Vector2{x1 - tr, y0 + tr},
				Vector2{x0 + bl, y1 - bl},
				Vector2{x1 - br, y1 - br}
			};
			for (size_t corner = 0; corner < 4; ++corner) {
				if (radius[corner] <= 1) continue;
				const auto p = corners[corner];
				if (!sharp)
					raster.Arc(
						{p.X + 1, p.Y + 1},
						radius[corner],
						radius[corner],
						corner == 0	  ? 90
						: corner == 1 ? 0
						: corner == 2 ? 180
									  : 270,
						corner == 0	  ? 180
						: corner == 1 ? 90
						: corner == 2 ? 270
									  : 360,
						32
					);
				else {
					switch (corner) {
					case 0:
						raster.Triangle({p.X, y0 + 1}, p, {x0 + 1, p.Y});
						break;
					case 1:
						raster.Triangle({p.X, y0}, p, {x1, p.Y});
						break;
					case 2:
						raster.Triangle({p.X, y1 - 1}, p, {x0 + 1, p.Y});
						break;
					case 3:
						raster.Triangle({p.X, y1}, p, {x1, p.Y});
						break;
					}
				}
			}
			raster.Quad(corners[0], corners[1], corners[2], corners[3]);
			raster.Quad({corners[0].X, y0}, {corners[1].X, y0}, corners[0], corners[1]);
			raster.Quad(corners[2], corners[3], {corners[2].X, y1}, {corners[3].X, y1});
			raster.Quad({x0, corners[0].Y}, corners[0], {x0, corners[2].Y}, corners[2]);
			raster.Quad(corners[1], {x1, corners[1].Y}, corners[3], {x1, corners[3].Y});
		} else
			return context.Fail(Status::UnknownNode, "Unknown PB primitive");
		return !raster.Failed && context.FailureCode == Status::Ok;
	}
}
