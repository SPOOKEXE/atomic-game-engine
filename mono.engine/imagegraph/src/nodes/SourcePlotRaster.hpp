#pragma once
#include "../SourceGradient.hpp"
#include "Path.hpp"
#include "Processor.hpp"

#include <numbers>

namespace engine::imagegraph::detail {
	// Pixel-center CPU coverage is an explicit reference profile. Source GPU
	// builtin coverage is requested separately; the source plot's inherited drawing
	// state defaults to alpha1/Normal.
	struct SourcePlotRaster {
		NodeContext &Context;
		Image &Output;
		uint64_t Work = 0;
		static double Cross(Vector2 a, Vector2 b, Vector2 p) {
			return (b.X - a.X) * (p.Y - a.Y) - (b.Y - a.Y) * (p.X - a.X);
		}
		static bool Included(Vector2 a, Vector2 b) {
			return b.Y < a.Y || (b.Y == a.Y && b.X > a.X);
		}
		bool Triangle(Vector2 a, Vector2 b, Vector2 c, Rgba ca, Rgba cb, Rgba cc, bool cut = false) {
			for (double v : {a.X, a.Y, b.X, b.Y, c.X, c.Y})
				if (!std::isfinite(v)) return Context.Fail(Status::InvalidValue, "plot vertex is nonfinite");
			double area = Cross(a, b, c);
			if (!std::isfinite(area))
				return Context.Fail(Status::InvalidValue, "plot triangle area is nonfinite");
			if (area == 0) return true;
			if (area < 0) {
				std::swap(b, c);
				std::swap(cb, cc);
				area = -area;
			}
			const uint32_t left =
				uint32_t(std::clamp(std::floor(std::min({a.X, b.X, c.X})), 0., double(Output.Width)));
			const uint32_t top =
				uint32_t(std::clamp(std::floor(std::min({a.Y, b.Y, c.Y})), 0., double(Output.Height)));
			const uint32_t right =
				uint32_t(std::clamp(std::ceil(std::max({a.X, b.X, c.X})), 0., double(Output.Width)));
			const uint32_t bottom =
				uint32_t(std::clamp(std::ceil(std::max({a.Y, b.Y, c.Y})), 0., double(Output.Height)));
			const uint64_t work = uint64_t(right - left) * (bottom - top);
			if (work > 64000000 - Work)
				return Context.Fail(Status::LimitExceeded, "plot raster exceeds work budget");
			Work += work;
			for (uint32_t y = top; y < bottom; ++y)
				for (uint32_t x = left; x < right; ++x) {
					const Vector2 p{x + .5, y + .5};
					const double ab = Cross(a, b, p), bc = Cross(b, c, p), ce = Cross(c, a, p);
					if (!(ab > 0 || (ab == 0 && Included(a, b))) ||
						!(bc > 0 || (bc == 0 && Included(b, c))) || !(ce > 0 || (ce == 0 && Included(c, a))))
						continue;
					Rgba colour{};
					if (!cut)
						for (size_t channel = 0; channel < 4; ++channel)
							colour[channel] = (ca[channel] * bc + cb[channel] * ce + cc[channel] * ab) / area;
					if (!WritePixel(Output, x, y, colour))
						return Context.Fail(Status::InvalidValue, "plot sample exceeds surface range");
				}
			return true;
		}
		bool Circle(Vector2 center, double radius, Rgba colour, bool cut = false) {
			Vector2 previous{center.X + radius, center.Y};
			for (size_t i = 1; i <= 8; ++i) {
				const double angle = double(i) * std::numbers::pi / 4;
				const Vector2 current{
					center.X + std::cos(angle) * radius, center.Y - std::sin(angle) * radius
				};
				if (!Triangle(center, previous, current, colour, colour, colour, cut)) return false;
				previous = current;
			}
			return true;
		}
		bool Line(Vector2 a, Vector2 b, double width, Rgba ca, Rgba cb, bool rounded, bool cut = false) {
			if (!std::isfinite(width) || std::abs(width) > Limits::MaximumDimension)
				return Context.Fail(Status::LimitExceeded, "plot line width exceeds bounded raster");
			const double dx = b.X - a.X, dy = b.Y - a.Y, length = std::hypot(dx, dy);
			if (length != 0) {
				const Vector2 normal{-dy / length * width / 2, dx / length * width / 2};
				const Vector2 aa{a.X + normal.X, a.Y + normal.Y}, bb{b.X + normal.X, b.Y + normal.Y};
				const Vector2 cc{a.X - normal.X, a.Y - normal.Y}, dd{b.X - normal.X, b.Y - normal.Y};
				if (!Triangle(aa, bb, cc, ca, cb, ca, cut) || !Triangle(bb, cc, dd, cb, ca, cb, cut))
					return false;
			}
			if (rounded && width != 1) {
				if (Context.PixelBuilderDrawing) Context.PixelBuilderDrawing->CirclePrecision = 8;
				return Circle(a, width / 2, ca, cut) && Circle(b, width / 2, cb, cut);
			}
			return true;
		}
	};
	inline double PlotFraction(double x) {
		return x - std::floor(x);
	}
	inline Vector2 PlotOffset(Vector2 p, double length, double angle) {
		const double a = angle * std::numbers::pi / 180;
		return {p.X + std::cos(a) * length, p.Y - std::sin(a) * length};
	}
	inline Rgba PlotColour(const Colour &c) {
		return {c.Red / 255., c.Green / 255., c.Blue / 255., 1};
	}
	inline Colour PlotMultiply(Colour a, Colour b) {
		return {
			uint8_t(uint32_t(a.Red) * b.Red / 255),
			uint8_t(uint32_t(a.Green) * b.Green / 255),
			uint8_t(uint32_t(a.Blue) * b.Blue / 255),
			uint8_t(uint32_t(a.Alpha) * b.Alpha / 255)
		};
	}
	template <class T> T PlotTyped(NodeContext &c, std::string_view port, T fallback = {}) {
		const auto *value = c.Find(port);
		if (!value) return fallback;
		if (const auto *typed = std::get_if<T>(value)) return *typed;
		c.Fail(Status::TypeMismatch, "plot control has an incompatible value type", port);
		return fallback;
	}

	inline std::optional<Colour> PlotGradient(NodeContext &c, std::string_view name, double x) {
		const std::string base(name);
		if (c.Boolean(base + "_mapped")) {
			const auto *image = c.Input(base + "_map");
			if (!image) return Colour{0, 0, 0, 0};
			const auto range = PlotTyped<Vector4>(c, base + "_map_range", {0, 0, 1, 0});
			const double px = (range.X + (range.Z - range.X) * x) * image->Width;
			const double py = (range.Y + (range.W - range.Y) * x) * image->Height;
			if (!std::isfinite(px) || !std::isfinite(py)) {
				c.Fail(
					Status::UnsupportedExecution, "source plot gradient map coordinate is nonfinite", name
				);
				return std::nullopt;
			}
			if (px < 0 || py < 0 || px >= image->Width || py >= image->Height) return Colour{0, 0, 0, 0};
			const auto pixel = ReadPixel(*image, uint32_t(px), uint32_t(py));
			return Colour{
				SourceColorByte(pixel[0] * 255),
				SourceColorByte(pixel[1] * 255),
				SourceColorByte(pixel[2] * 255),
				SourceColorByte(pixel[3] * 255)
			};
		}
		const auto *value = c.Find(name);
		const auto *gradient = value ? std::get_if<Gradient>(value) : nullptr;
		if (!gradient) {
			c.Fail(Status::InvalidValue, "plot gradient control is missing", name);
			return std::nullopt;
		}
		return SourceGradientAt(*gradient, x);
	}
	inline bool SourcePlot(NodeContext &c) {
		if (c.Request.RequireSourceGpuRasterCoverage)
			return c.Fail(
				Status::UnsupportedExecution,
				"plot builtin primitives require captured source GPU "
				"coverage for this request"
			);
		uint32_t width = 0, height = 0;
		if (c.Integer("dimension_unit", 1) == 2) {
			const auto *mask = c.Input("mask");
			if (!mask)
				return c.Fail(Status::InvalidValue, "plot Mask dimensions require a mask", "dimension");
			width = mask->Width;
			height = mask->Height;
		} else if (!ResolveDimension(c, "dimension", width, height))
			return false;
		const auto format = ResolveProcessorSurfaceFormat(c, nullptr);
		if (!format) return false;
		const auto *value = c.Find("data");
		const auto *array = value ? std::get_if<ArrayValue>(value) : nullptr;
		if (!array) return c.Fail(Status::InvalidValue, "plot data requires a numeric array", "data");

		const size_t count = array->Items.empty() ? array->Elements.size() : array->Items.size();
		if (count > Limits::MaximumArrayElements)
			return c.Fail(Status::LimitExceeded, "plot data exceeds sample budget", "data");
		auto charge = c.ReserveWorkspace(2 * Limits::MaximumArrayElements * sizeof(double), "data");
		if (!charge) return false;
		std::vector<double> data;
		data.reserve(count);
		const auto add = [&](const ElementValue &item) {
			if (const auto *v = std::get_if<double>(&item))
				data.push_back(*v);
			else if (const auto *v = std::get_if<int64_t>(&item))
				data.push_back(double(*v));
			else if (const auto *v = std::get_if<bool>(&item))
				data.push_back(double(*v));
			else if (const auto *v = std::get_if<EnumValue>(&item))
				data.push_back(double(v->Value));
			else
				return c.Fail(Status::TypeMismatch, "plot data contains a nonnumeric value", "data");
			if (!std::isfinite(data.back()))
				return c.Fail(Status::InvalidValue, "plot data must be finite", "data");
			return true;
		};
		if (array->Items.empty()) {
			for (const auto &item : array->Elements)
				if (!add(item)) return false;
		} else
			for (const auto &item : array->Items) {
				const auto *leaf = std::get_if<ElementValue>(&item.Data);
				if (!leaf || !add(*leaf))
					return c.Fail(Status::TypeMismatch, "plot data needs numeric leaves", "data");
			}

		const auto range = c.Vec2("range", {0, 1});
		const double start = std::clamp(range.X, 0., 1.) * data.size(),
					 end = std::clamp(range.Y, 0., 1.) * data.size();
		const double sample = std::max(1., c.Scalar("sample_frequency", 1)),
					 offset = std::max(0., c.Scalar("window_offset"));
		if (!std::isfinite(sample) || !std::isfinite(offset) || !std::isfinite(start) || !std::isfinite(end))
			return c.Fail(Status::InvalidValue, "plot trim controls must be finite");
		std::vector<double> values;
		values.reserve(data.size());
		if (c.Integer("trim_mode") == 0) {
			for (double i = start; i < end; i += sample)
				values.push_back(data[size_t(i)]);
		} else {
			const int64_t count = c.Integer("window_size", 8);
			if (count > int64_t(Limits::MaximumArrayElements))
				return c.Fail(Status::LimitExceeded, "plot window exceeds sample budget", "window_size");
			for (int64_t i = 0; i < count; ++i) {
				const double index = offset + double(i) * sample;
				if (index >= data.size()) break;
				const size_t base = size_t(index);
				double y = data[base];
				if (index != std::floor(index) && base + 1 < data.size())
					y += (data[base + 1] - y) * (index - std::trunc(index));
				values.push_back(c.Boolean("flip_value") ? -y : y);
			}
		}
		auto *out = c.NewImage("surface_out", width, height, *format);
		if (!out) return false;
		if (c.Boolean("background")) {
			const auto colour = PlotColour(PlotTyped<Colour>(c, "background_color", {0, 0, 0, 255}));
			for (uint32_t y = 0; y < height; ++y)
				for (uint32_t x = 0; x < width; ++x)
					if (!WritePixel(*out, x, y, colour))
						return c.Fail(Status::InvalidValue, "plot background exceeds surface range");
		}
		SourcePlotRaster raster{c, *out};
		const auto origin = UnitVector(c, "origin", width, height);
		const double scale = c.Scalar("scale", .5) * UnitScale(c, "scale", width),
					 radius = c.Scalar("radius", .5) * UnitScale(c, "radius", width);
		const double valueOffset = c.Scalar("value_offset"), smooth = c.Scalar("smooth"),
					 thickness = c.Scalar("graph_thickness", 1);
		const double barWidth = c.Scalar("bar_width", 4), spacing = c.Scalar("spacing", 1),
					 exploded = c.Scalar("exploded");
		const auto valueRange = c.Vec2("value_range", {0, 1});
		const auto base = PlotTyped<Colour>(c, "base_color", {255, 255, 255, 255});
		const auto colourAt = [&](size_t index, double y) -> std::optional<Rgba> {
			const auto sampleColour = PlotGradient(
				c, "color_over_sample", PlotFraction(double(index) / values.size() + c.Scalar("shift"))
			);
			const double amount = c.Boolean("absolute") ? std::abs(y) : y;
			const double ratio = (amount - valueRange.X) / (valueRange.Y - valueRange.X);
			if (!std::isfinite(ratio)) {
				c.Fail(Status::UnsupportedExecution, "source plot value colour range is nonfinite");
				return std::nullopt;
			}
			const auto valueColour =
				PlotGradient(c, "color_over_value", PlotFraction(ratio + c.Scalar("shift_2")));
			if (!sampleColour || !valueColour) return std::nullopt;
			return PlotColour(PlotMultiply(PlotMultiply(base, *sampleColour), *valueColour));
		};
		const int64_t type = c.Integer("type");
		double angle = c.Scalar("direction");
		if (type < 2) {
			if (type == 1 && c.PixelBuilderDrawing) c.PixelBuilderDrawing->CirclePrecision = 4;
			PathRuntime path;
			const auto *pathValue = c.Find("path");
			const auto *pathData = pathValue ? std::get_if<Path2D>(pathValue) : nullptr;
			const bool usePath =
				pathData && (c.IsLinked("path") || !pathData->Anchors.empty() || pathData->SourceOperation);
			if (usePath && !path.Init(c, *pathData)) return false;
			Vector2 previous{}, first{}, previousPath{};
			Rgba previousColour{}, lastColour{};
			for (size_t i = 0; i < values.size(); ++i) {
				Vector2 point;
				if (usePath) {
					const auto p = path.PointRatio(double(i) / values.size());
					point = {p.X, p.Y};
					if (i == 0) {
						const auto p0 = path.PointRatio(-.001);
						previousPath = {p0.X, p0.Y};
					}
					angle = -std::atan2(point.Y - previousPath.Y, point.X - previousPath.X) * 180 /
							std::numbers::pi;
					previousPath = point;
				} else
					point = PlotOffset(origin, double(i) * (spacing + (type == 1 ? 1 : barWidth)), angle);
				const double y = values[i] + valueOffset;
				const auto colour = colourAt(i, y);
				if (!colour) return false;
				const auto current = PlotOffset(point, scale * y, angle + 90);
				if (type == 0) {
					if (!raster.Line(point, current, barWidth, *colour, *colour, c.Boolean("rounded_bar")))
						return false;
				} else if (double(i) > start) {
					if (smooth > 0) {
						const double a = angle * std::numbers::pi / 180;
						const double distance =
							(current.X - previous.X) * std::cos(a) - (current.Y - previous.Y) * std::sin(a);
						const auto b0 = PlotOffset(previous, distance * smooth, angle),
								   b1 = PlotOffset(current, distance * smooth, angle + 180);
						Vector2 segment = previous;
						for (size_t j = 1; j <= 8; ++j) {
							const double t = double(j) / 8, u = 1 - t;
							const Vector2 next{
								previous.X * u * u * u + 3 * b0.X * u * u * t + 3 * b1.X * u * t * t +
									current.X * t * t * t,
								previous.Y * u * u * u + 3 * b0.Y * u * u * t + 3 * b1.Y * u * t * t +
									current.Y * t * t * t
							};
							if (!raster.Line(segment, next, thickness, *colour, *colour, thickness > 1))
								return false;
							segment = next;
						}
					} else if (!raster.Line(
								   previous, current, thickness, previousColour, *colour, thickness > 1
							   ))
						return false;
				}
				previous = current;
				previousColour = *colour;
				lastColour = *colour;
				if (i == 0) first = current;
			}
			if (type == 1 && c.Boolean("loop") && values.size() > 1)
				if (!raster.Line(first, previous, thickness, lastColour, lastColour, true)) return false;
		} else if (type == 2) {
			if (c.PixelBuilderDrawing) c.PixelBuilderDrawing->CirclePrecision = 64;
			double sum = 0;
			for (double y : values)
				sum += y;
			const double degrees = 360 / sum, dir = c.Boolean("clockwise") ? -1 : 1,
						 donut = c.Scalar("donut_radius"), separation = c.Scalar("donut_separation");
			for (size_t i = 0; i < values.size(); ++i) {
				const double span = values[i] * degrees, begin = angle, endAngle = angle + span * dir;
				angle = endAngle;
				if (!std::isfinite(span))
					return c.Fail(Status::UnsupportedExecution, "source pie total produces nonfinite angles");
				const double rawSteps = std::ceil(span / 3);
				if (rawSteps <= 0) continue;
				if (rawSteps > Limits::MaximumArrayElements)
					return c.Fail(Status::LimitExceeded, "plot pie exceeds arc step budget");
				const size_t steps = size_t(rawSteps);
				const auto colour = colourAt(i, values[i]);
				if (!colour) return false;
				const auto center = PlotOffset(origin, exploded, begin + span * dir / 2);
				Vector2 previous{};
				for (size_t j = 0; j <= steps; ++j) {
					const double a = begin + (endAngle - begin) * double(j) / steps;
					const auto current = PlotOffset(center, radius, a);
					if (j && !raster.Triangle(center, previous, current, *colour, *colour, *colour))
						return false;
					previous = current;
				}
				if (donut > 0)
					for (size_t j = 0; j <= steps; ++j) {
						const double a = begin + (endAngle - begin) * double(j) / steps;
						const auto current = PlotOffset(center, radius * donut, a);
						if (j && !raster.Triangle(
									 center, previous, current, {1, 1, 1, 1}, {1, 1, 1, 1}, {1, 1, 1, 1}, true
								 ))
							return false;
						previous = current;
					}
				if (separation > 0) {
					const Vector2 shifted{center.X - 1, center.Y - 1};
					if (!raster.Line(
							shifted,
							PlotOffset(shifted, radius, begin),
							separation,
							{1, 1, 1, 1},
							{1, 1, 1, 1},
							true,
							true
						) ||
						!raster.Line(
							shifted,
							PlotOffset(shifted, radius, endAngle),
							separation,
							{1, 1, 1, 1},
							{1, 1, 1, 1},
							true,
							true
						))
						return false;
				}
			}
		}
		return c.FailureCode == Status::Ok;
	}
} // namespace engine::imagegraph::detail
