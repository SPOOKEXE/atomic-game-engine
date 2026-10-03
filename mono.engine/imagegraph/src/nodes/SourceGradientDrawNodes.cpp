#include "../SourceMappedInputs.hpp"
#include "../SourceSafeDraw.hpp"
#include "Curve.hpp"
#include "Gradient.hpp"
#include "Source2DGenerator.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <type_traits>

namespace engine::imagegraph::detail {
	namespace {
		constexpr uint64_t DRAW_GRADIENT_WORK_LIMIT = 64000000;
		template <class Variant> std::optional<double> DrawGradientNumber(const Variant &value) {
			if (const auto *v = std::get_if<double>(&value)) return *v;
			if (const auto *v = std::get_if<int64_t>(&value)) return double(*v);
			if (const auto *v = std::get_if<EnumValue>(&value)) return double(v->Value);
			return std::nullopt;
		}
		template <class VectorFn, class NumberFn>
		void DrawGradientItems(
			const std::vector<SourceArrayItem> &items, bool vectors, VectorFn &vector, NumberFn &number
		);
		template <class Variant, class VectorFn, class NumberFn>
		void DrawGradientVisit(const Variant &value, bool vectors, VectorFn &vector, NumberFn &number) {
			if (const auto n = DrawGradientNumber(value)) {
				number(*n);
			} else if (const auto *v = std::get_if<Vector2>(&value)) {
				vector(*v);
			} else if constexpr (std::is_same_v<Variant, Value>) {
				if (const auto *a = std::get_if<ArrayValue>(&value)) {
					const auto row = [&](const std::vector<ElementValue> &elements) {
						if (vectors && !elements.empty()) {
							const auto x = DrawGradientNumber(elements[0]);
							const auto y = elements.size() > 1 ? DrawGradientNumber(elements[1])
															   : std::optional<double>{0};
							if (x && y) {
								vector(Vector2{*x, *y});
								return;
							}
						}
						for (const auto &e : elements)
							DrawGradientVisit(e, vectors, vector, number);
					};
					row(a->Elements);
					for (const auto &elements : a->Nested)
						row(elements);
					DrawGradientItems(a->Items, vectors, vector, number);
				}
			}
		}
		template <class VectorFn, class NumberFn>
		void DrawGradientItems(
			const std::vector<SourceArrayItem> &items, bool vectors, VectorFn &vector, NumberFn &number
		) {
			if (vectors && !items.empty()) {
				const auto *x = std::get_if<ElementValue>(&items[0].Data);
				const auto *y = items.size() > 1 ? std::get_if<ElementValue>(&items[1].Data) : nullptr;
				const auto a = x ? DrawGradientNumber(*x) : std::nullopt;
				const auto b = y ? DrawGradientNumber(*y) : std::optional<double>{0};
				if (a && b) {
					vector(Vector2{*a, *b});
					return;
				}
			}
			for (const auto &item : items) {
				if (const auto *e = std::get_if<ElementValue>(&item.Data))
					DrawGradientVisit(*e, vectors, vector, number);
				else if (const auto *children = std::get_if<std::vector<SourceArrayItem>>(&item.Data))
					DrawGradientItems(*children, vectors, vector, number);
				else if (vectors) {
					const auto &image = std::get<Image>(item.Data);
					vector(Vector2{double(image.Width), double(image.Height)});
				}
			}
		}
		const Value *DrawGradientOriginal(const NodeContext &c, std::string_view port) {
			for (auto input = c.ProcessorOriginalValues.rbegin(); input != c.ProcessorOriginalValues.rend();
				 ++input)
				if (input->first == port) return input->second;
			for (const auto &[id, value] : c.Values)
				if (id == port) return &value;
			return c.Find(port);
		}
		template <class V> void DrawGradientCounts(const V &v, size_t &curve, size_t &gradient);
		void
		DrawGradientItemCounts(const std::vector<SourceArrayItem> &items, size_t &curve, size_t &gradient) {
			for (const auto &item : items) {
				if (const auto *v = std::get_if<ElementValue>(&item.Data))
					DrawGradientCounts(*v, curve, gradient);
				else if (const auto *children = std::get_if<std::vector<SourceArrayItem>>(&item.Data))
					DrawGradientItemCounts(*children, curve, gradient);
			}
		}
		template <class V> void DrawGradientCounts(const V &v, size_t &curve, size_t &gradient) {
			if (const auto *c = std::get_if<Curve>(&v))
				curve = std::max(curve, c->Anchors.size());
			else if (const auto *g = std::get_if<Gradient>(&v))
				gradient = std::max(gradient, g->Keys.size());
			else if constexpr (std::is_same_v<V, Value>) {
				if (const auto *a = std::get_if<ArrayValue>(&v)) {
					for (const auto &e : a->Elements)
						DrawGradientCounts(e, curve, gradient);
					for (const auto &row : a->Nested)
						for (const auto &e : row)
							DrawGradientCounts(e, curve, gradient);
					DrawGradientItemCounts(a->Items, curve, gradient);
				}
			}
		}
		bool DrawGradientBatchWork(NodeContext &c) {
			if (c.ProcessorRow != 0) return true;
			const auto unit = c.Integer("dimension_unit", 1);
			double maskWidth = 1, maskHeight = 1;
			if (const auto *m = c.Input("mask")) {
				maskWidth = m->Width;
				maskHeight = m->Height;
			}
			for (const auto &[port, frames] : c.ImageArrays)
				if (port == "mask" && frames)
					for (const auto &m : frames->Images) {
						maskWidth = std::max(maskWidth, double(m.Width));
						maskHeight = std::max(maskHeight, double(m.Height));
					}
			uint64_t width = 1, height = 1;
			bool valid = true;
			const auto dimension = [&](Vector2 p) {
				if (!c.IsLinked("dimension")) {
					if (unit == 1) {
						p.X *= c.Project.SurfaceWidth;
						p.Y *= c.Project.SurfaceHeight;
					} else if (unit == 2) {
						p.X *= maskWidth;
						p.Y *= maskHeight;
					}
				}
				if (!std::isfinite(p.X) || !std::isfinite(p.Y) || p.X <= 0 || p.Y <= 0 ||
					p.X > Limits::MaximumDimension + .5 || p.Y > Limits::MaximumDimension + .5) {
					valid = false;
					return;
				}
				const auto side = [](double n) {
					const auto f = std::floor(n), part = n - f;
					return uint64_t(std::max(1., f + (part > .5 || (part == .5 && std::fmod(f, 2.) != 0))));
				};
				width = std::max(width, side(p.X));
				height = std::max(height, side(p.Y));
			};
			auto d = dimension;
			auto number = [&](double n) { dimension({n, n}); };
			if (const auto *v = DrawGradientOriginal(c, "dimension"))
				DrawGradientVisit(*v, true, d, number);
			else
				dimension(c.Vec2("dimension", {1, 1}));
			for (const auto &[port, frames] : c.ImageArrays)
				if (port == "dimension" && frames)
					for (const auto &image : frames->Images)
						dimension({double(image.Width), double(image.Height)});
			if (!valid || width > Limits::MaximumDimension || height > Limits::MaximumDimension)
				return c.Fail(
					Status::LimitExceeded, "Draw Gradient batch dimensions exceed native limits", "dimension"
				);
			size_t curves = 0, keys = 0;
			for (const auto port : {"progress_remap", "inverse_curve", "curve"}) {
				size_t anchors = 0, unused = 0;
				if (const auto *v = DrawGradientOriginal(c, port)) DrawGradientCounts(*v, anchors, unused);
				curves += anchors;
			}
			size_t unused = 0;
			if (const auto *v = DrawGradientOriginal(c, "gradient")) DrawGradientCounts(*v, unused, keys);
			// Bound shape/map/read/write stages, all three curve scans and eight Newton steps per curve.
			const uint64_t work = 48 + uint64_t(curves) * 9 + uint64_t(keys);
			if (width * height > DRAW_GRADIENT_WORK_LIMIT / std::max(size_t{1}, c.ProcessorCount) / work)
				return c.Fail(
					Status::LimitExceeded,
					"Draw Gradient exceeds entire processor batch work budget",
					"dimension"
				);
			return true;
		}
		bool DrawGradientDimensions(NodeContext &c, uint32_t &width, uint32_t &height, Vector2 &raw) {
			raw = c.Vec2("dimension", {1, 1});
			const auto unit = c.Integer("dimension_unit", 1);
			if (unit < 0 || unit > 2)
				return c.Fail(
					Status::InvalidValue, "Draw Gradient dimension unit is invalid", "dimension_unit"
				);
			if (!c.IsLinked("dimension")) {
				if (unit == 1) {
					raw.X *= c.Project.SurfaceWidth;
					raw.Y *= c.Project.SurfaceHeight;
				} else if (unit == 2) {
					const auto *mask = c.Input("mask");
					if (!mask) return c.Fail(Status::InvalidValue, "Mask dimensions require a mask", "mask");
					raw.X *= mask->Width;
					raw.Y *= mask->Height;
				}
			}
			if (!std::isfinite(raw.X) || !std::isfinite(raw.Y) || raw.X <= 0 || raw.Y <= 0)
				return c.Fail(
					Status::UnsupportedExecution,
					"Draw Gradient raw shader dimensions must be positive",
					"dimension"
				);
			const auto side = [](double n) {
				const auto f = std::floor(n), part = n - f;
				return std::max(1., f + (part > .5 || (part == .5 && std::fmod(f, 2.) != 0)));
			};
			const auto w = side(raw.X), h = side(raw.Y);
			if (w > Limits::MaximumDimension || h > Limits::MaximumDimension)
				return c.Fail(
					Status::LimitExceeded, "Draw Gradient dimensions exceed native limits", "dimension"
				);
			width = uint32_t(w);
			height = uint32_t(h);
			return c.FailureCode == Status::Ok;
		}
		struct DrawGradientMapped {
			Vector2 Range{};
			const Image *Map = nullptr;
		};
		bool DrawGradientControl(NodeContext &c, std::string_view port, DrawGradientMapped &out) {
			if (SourceRangeMapped(c, port)) {
				if (!ReadSourceMappedRange(c, port, out.Range)) return false;
				out.Map = c.Input(std::string(port) + "_map");
			} else {
				const auto value = c.Scalar(port);
				out.Range = {value, value};
			}
			return c.FailureCode == Status::Ok;
		}
		double DrawGradientMappedSample(const DrawGradientMapped &control, double u, double v) {
			if (!control.Map) return control.Range.X;
			const auto p = SampleNearest(*control.Map, u, v);
			return control.Range.X + (control.Range.Y - control.Range.X) * (p[0] + p[1] + p[2]) / 3;
		}
		const Curve *DrawGradientCurve(NodeContext &c, std::string_view port) {
			const auto *v = c.Find(port);
			const auto *curve = v ? std::get_if<Curve>(v) : nullptr;
			if (!curve || curve->Anchors.empty()) {
				c.Fail(Status::UnsupportedExecution, "Draw Gradient curve upload is absent or empty", port);
				return nullptr;
			}
			if (curve->Anchors.size() > 9) {
				c.Fail(
					Status::UnsupportedExecution,
					"Draw Gradient GLSL curve has only 64 uniform slots; HLSL requires a backend observation",
					port
				);
				return nullptr;
			}
			if (curve->Header[1] == 0) {
				c.Fail(Status::UnsupportedExecution, "Draw Gradient curve divides by zero scale", port);
				return nullptr;
			}
			return curve;
		}
	}
	bool SourceDrawGradient(NodeContext &c) {
		if (!DrawGradientBatchWork(c)) return false;
		uint32_t width = 0, height = 0;
		Vector2 raw{};
		if (!DrawGradientDimensions(c, width, height, raw)) return false;
		const auto format = ResolveProcessorSurfaceFormat(c, nullptr);
		if (!format) return false;
		const auto *mask = c.Input("mask");
		const bool safeMask =
			mask && (mask->Format == SurfaceFormat::R8Unorm || mask->Format == SurfaceFormat::R16Float ||
					 mask->Format == SurfaceFormat::R32Float);
		if (safeMask) {
			auto *out = c.NewImage("surface_out", width, height, *format);
			if (!out) return false;
			// __channel_pre replaces sh_gradient when the base mask is a single-channel surface.
			for (uint32_t y = 0; y < height; ++y)
				for (uint32_t x = 0; x < width; ++x) {
					const auto p = SampleNearest(*mask, (x + .5) / width, (y + .5) / height);
					if (!WritePixel(*out, x, y, {p[0], p[0], p[0], 1}))
						return c.Fail(
							Status::InvalidValue,
							"Draw Gradient safe mask sample exceeds surface range",
							"surface_out"
						);
				}
			return c.FailureCode == Status::Ok;
		}
		const auto type = c.Integer("type"), loop = c.Integer("loop");
		if (type < 0 || type > 3 || loop < 0 || loop > 2)
			return c.Fail(
				Status::InvalidValue,
				"Draw Gradient shape or loop is invalid",
				type < 0 || type > 3 ? "type" : "loop"
			);
		auto center = c.Vec2("center", {.5, .5});
		if (!c.IsLinked("center") && c.Integer("center_unit", 1) == 1) {
			center.X *= raw.X;
			center.Y *= raw.Y;
		}
		center.X /= raw.X;
		center.Y /= raw.Y;
		const auto shape = c.Vec2("shape", {1, 1}), levelIn = c.Vec2("level_in", {0, 1}),
				   levelOut = c.Vec2("level_out", {0, 1});
		const bool uniform = c.Boolean("uniform_ratio", true);
		const auto inverse = c.Scalar("inverse_axis");
		if (levelIn.X == levelIn.Y)
			return c.Fail(
				Status::UnsupportedExecution, "Draw Gradient level range divides by zero", "level_in"
			);
		DrawGradientMapped angle, radius, shift, scale;
		if (!DrawGradientControl(c, "angle", angle) || !DrawGradientControl(c, "radius", radius) ||
			!DrawGradientControl(c, "shift", shift) || !DrawGradientControl(c, "scale", scale))
			return false;
		const auto *progress = DrawGradientCurve(c, "progress_remap"),
				   *outputCurve = DrawGradientCurve(c, "curve");
		const auto *inverseCurve = inverse != 0 ? DrawGradientCurve(c, "inverse_curve") : nullptr;
		if (!progress || !outputCurve || (inverse != 0 && !inverseCurve)) return false;
		const auto *gv = c.Find("gradient");
		const auto *gradient = gv ? std::get_if<Gradient>(gv) : nullptr;
		if (!gradient)
			return c.Fail(Status::InvalidValue, "Draw Gradient requires gradient data", "gradient");
		const auto sampler = ReadGradient(c, "gradient", *gradient);
		if (!sampler.Map && (gradient->Keys.empty() || gradient->Keys.size() > 64))
			return c.Fail(
				Status::UnsupportedExecution,
				"Draw Gradient GLSL key uniform requires 1 through 64 keys",
				"gradient"
			);
		auto *out = c.NewImage("surface_out", width, height, *format);
		if (!out) return false;
		constexpr double tau = 6.283185307179586;
		for (uint32_t y = 0; y < height; ++y)
			for (uint32_t x = 0; x < width; ++x) {
				const double u = (x + .5) / width, v = (y + .5) / height;
				const auto ang = DrawGradientMappedSample(angle, u, v) * tau / 360,
						   rad = DrawGradientMappedSample(radius, u, v) * std::sqrt(2.),
						   shf = DrawGradientMappedSample(shift, u, v),
						   sca = DrawGradientMappedSample(scale, u, v);
				double alpha = 1;
				const auto uv = source2d::GeneratorUv(c, u, v, alpha);
				if (!std::isfinite(uv.X) || !std::isfinite(uv.Y) || !std::isfinite(alpha))
					return c.Fail(
						Status::UnsupportedExecution, "Draw Gradient UV sample is nonfinite", "uv_map"
					);
				const double px = uv.X - center.X, py = uv.Y - center.Y, ax = uniform ? raw.X / raw.Y : 1,
							 ay = 1, a = std::atan2(py, px) + ang;
				const auto circular = [&] { return std::hypot(px * ax / shape.X, py * ay / shape.Y) / rad; };
				const auto radial = [&] { return (a - std::floor(a / tau) * tau) / tau; };
				double p = 0, inv = 0;
				if (type == 0) {
					p = .5 + px * std::cos(ang) - py * std::sin(ang);
					inv = .5 + px * std::cos(ang + tau / 4) - py * std::sin(ang + tau / 4);
				} else if (type == 1) {
					p = circular();
					inv = radial();
				} else if (type == 2) {
					p = radial();
					if (inverse != 0) inv = circular();
				} else {
					p = (std::abs((px * std::cos(ang) - py * std::sin(ang)) * ax / shape.X) +
						 std::abs((px * std::sin(ang) + py * std::cos(ang)) * ay / shape.Y)) /
						rad;
					inv = std::max(std::abs(px), std::abs(py));
				}
				if (inverse != 0) p += EvalShaderCurve(*inverseCurve, inv) * inverse;
				p = (p + shf - .5) / sca + .5;
				if (loop == 1)
					p = ShaderFract(ShaderFract(p) + 1);
				else if (loop == 2)
					p = 1 - std::abs((p - 2 * std::floor(p / 2)) - 1);
				if (!std::isfinite(p))
					return c.Fail(
						Status::UnsupportedExecution, "Draw Gradient progress division is nonfinite", "scale"
					);
				p = EvalShaderCurve(*progress, p);
				if (!std::isfinite(p))
					return c.Fail(
						Status::UnsupportedExecution,
						"Draw Gradient progress curve is nonfinite",
						"progress_remap"
					);
				auto colour = GradientEval(sampler, p);
				colour[3] *= alpha * (mask ? SampleNearest(*mask, u, v)[3] : 1);
				for (size_t k = 0; k < 3; ++k)
					colour[k] = levelOut.X +
								(levelOut.Y - levelOut.X) * (colour[k] - levelIn.X) / (levelIn.Y - levelIn.X);
				const auto w = (colour[0] + colour[1] + colour[2]) / 3;
				const auto target = EvalShaderCurve(*outputCurve, w);
				for (size_t k = 0; k < 3; ++k)
					colour[k] = w == 0 ? target : colour[k] * target / w;
				if (!WritePixel(*out, x, y, colour))
					return c.Fail(
						Status::UnsupportedExecution,
						"Draw Gradient shader sample is nonfinite or exceeds surface range",
						"surface_out"
					);
			}
		return c.FailureCode == Status::Ok;
	}
}
