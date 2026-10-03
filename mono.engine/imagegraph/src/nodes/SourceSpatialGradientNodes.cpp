#include "Processor.hpp"
#include "Source2DGenerator.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <type_traits>
#include <vector>

namespace engine::imagegraph::detail {
	namespace {
		constexpr uint64_t SPATIAL_WORK_LIMIT = 64000000;
		// Grid ordinals are at most4095; stack port names avoid allocating per anchor.
		struct SpatialPortName {
			std::array<char, 32> Bytes{};
			size_t Size = 0;
			std::string_view View() const {
				return {Bytes.data(), Size};
			}
		};
		SpatialPortName SpatialPort(std::string_view prefix, uint64_t ordinal) {
			SpatialPortName p;
			std::copy(prefix.begin(), prefix.end(), p.Bytes.begin());
			const auto result =
				std::to_chars(p.Bytes.data() + prefix.size(), p.Bytes.data() + p.Bytes.size(), ordinal);
			p.Size = size_t(result.ptr - p.Bytes.data());
			return p;
		}
		template <class Variant> std::optional<double> SpatialNumber(const Variant &value) {
			if (const auto *v = std::get_if<double>(&value)) return *v;
			if (const auto *v = std::get_if<int64_t>(&value)) return double(*v);
			if (const auto *v = std::get_if<EnumValue>(&value)) return double(v->Value);
			return std::nullopt;
		}
		template <class VectorFn, class NumberFn>
		void SpatialItems(
			const std::vector<SourceArrayItem> &items, bool vectors, VectorFn &vector, NumberFn &number
		);
		template <class Variant, class VectorFn, class NumberFn>
		void SpatialVisit(const Variant &value, bool vectors, VectorFn &vector, NumberFn &number) {
			if (const auto n = SpatialNumber(value)) {
				number(*n);
			} else if (const auto *v = std::get_if<Vector2>(&value)) {
				vector(*v);
			} else if constexpr (std::is_same_v<Variant, Value>) {
				if (const auto *a = std::get_if<ArrayValue>(&value)) {
					const auto row = [&](const std::vector<ElementValue> &elements) {
						if (vectors && !elements.empty()) {
							const auto x = SpatialNumber(elements[0]);
							const auto y =
								elements.size() > 1 ? SpatialNumber(elements[1]) : std::optional<double>{0};
							if (x && y) {
								vector(Vector2{*x, *y});
								return;
							}
						}
						for (const auto &e : elements)
							SpatialVisit(e, vectors, vector, number);
					};
					row(a->Elements);
					for (const auto &elements : a->Nested)
						row(elements);
					SpatialItems(a->Items, vectors, vector, number);
				}
			}
		}
		template <class VectorFn, class NumberFn>
		void SpatialItems(
			const std::vector<SourceArrayItem> &items, bool vectors, VectorFn &vector, NumberFn &number
		) {
			if (vectors && !items.empty()) {
				const auto *x = std::get_if<ElementValue>(&items[0].Data);
				const auto *y = items.size() > 1 ? std::get_if<ElementValue>(&items[1].Data) : nullptr;
				const auto a = x ? SpatialNumber(*x) : std::nullopt;
				const auto b = y ? SpatialNumber(*y) : std::optional<double>{0};
				if (a && b) {
					vector(Vector2{*a, *b});
					return;
				}
			}
			for (const auto &item : items) {
				if (const auto *e = std::get_if<ElementValue>(&item.Data))
					SpatialVisit(*e, vectors, vector, number);
				else if (const auto *children = std::get_if<std::vector<SourceArrayItem>>(&item.Data))
					SpatialItems(*children, vectors, vector, number);
				else if (vectors) {
					const auto &image = std::get<Image>(item.Data);
					vector(Vector2{double(image.Width), double(image.Height)});
				}
			}
		}
		const Value *SpatialOriginal(const NodeContext &c, std::string_view port) {
			for (auto input = c.ProcessorOriginalValues.rbegin(); input != c.ProcessorOriginalValues.rend();
				 ++input)
				if (input->first == port) return input->second;
			for (const auto &[id, value] : c.Values)
				if (id == port) return &value;
			return c.Find(port);
		}
		// Admit conservative extrema of all borrowed rows before the first row allocates or draws.
		// Row selection and source unit/getter semantics still determine each row's actual output.
		bool SpatialBatchWork(NodeContext &c, bool gridNode) {
			if (c.ProcessorRow != 0) return true;
			bool referenceUnit = c.Integer("dimension_unit", 1) == 1;
			auto ignoreVector = [](Vector2) {};
			auto maximumUnit = [&](double n) { referenceUnit = referenceUnit || n == 1; };
			if (const auto *v = SpatialOriginal(c, "dimension_unit"))
				SpatialVisit(*v, false, ignoreVector, maximumUnit);
			uint64_t width = 1, height = 1;
			bool valid = true;
			auto dimension = [&](Vector2 p) {
				if (!c.IsLinked("dimension") && referenceUnit) {
					p.X *= c.Project.SurfaceWidth;
					p.Y *= c.Project.SurfaceHeight;
				}
				if (!std::isfinite(p.X) || !std::isfinite(p.Y) || p.X <= 0 || p.Y <= 0 ||
					p.X > Limits::MaximumDimension + .5 || p.Y > Limits::MaximumDimension + .5) {
					valid = false;
					return;
				}
				const auto rounded = [](double n) {
					const double floor = std::floor(n), part = n - floor;
					return uint64_t(
						std::max(1., floor + (part > .5 || (part == .5 && std::fmod(floor, 2.) != 0)))
					);
				};
				width = std::max(width, rounded(p.X));
				height = std::max(height, rounded(p.Y));
			};
			auto square = [&](double n) { dimension({n, n}); };
			if (const auto *v = SpatialOriginal(c, "dimension"))
				SpatialVisit(*v, true, dimension, square);
			else
				dimension(c.Vec2("dimension", {1, 1}));
			for (const auto &[port, array] : c.ImageArrays)
				if (port == "dimension" && array) {
					for (const auto &image : array->Images)
						dimension({double(image.Width), double(image.Height)});
				}
			if (!valid || width > Limits::MaximumDimension || height > Limits::MaximumDimension)
				return c.Fail(
					Status::LimitExceeded, "Spatial batch dimensions exceed native limits", "dimension"
				);
			const uint64_t budget = SPATIAL_WORK_LIMIT / std::max(size_t{1}, c.ProcessorCount),
						   pixels = width * height;
			if (!gridNode) {
				if (pixels > budget / 4)
					return c.Fail(
						Status::LimitExceeded,
						"Four-point gradient exceeds processor work budget",
						"dimension"
					);
				return true;
			}
			double subdivision = double(c.Integer("subdivision", 4)),
				   smoothing = std::max(0., c.Scalar("smooth_mesh", .5));
			auto maximumSubdivision = [&](double n) {
				subdivision = std::isfinite(n) ? std::max(subdivision, n) : n;
			};
			auto maximumSmoothing = [&](double n) {
				smoothing = std::isfinite(n) ? std::max(smoothing, n) : n;
			};
			if (const auto *v = SpatialOriginal(c, "subdivision"))
				SpatialVisit(*v, false, ignoreVector, maximumSubdivision);
			if (const auto *v = SpatialOriginal(c, "smooth_mesh"))
				SpatialVisit(*v, false, ignoreVector, maximumSmoothing);
			const auto g = c.Vec2("grid", {2, 2});
			if (!std::isfinite(subdivision) || !std::isfinite(smoothing) || !std::isfinite(g.X) ||
				!std::isfinite(g.Y) || subdivision < 1 || subdivision > 4096 || g.X < 1 || g.Y < 1 ||
				g.X > 4096 || g.Y > 4096)
				return c.Fail(
					Status::LimitExceeded, "Grid batch topology exceeds native limits", "subdivision"
				);
			const uint64_t sw = uint64_t(g.X) * uint64_t(subdivision),
						   sh = uint64_t(g.Y) * uint64_t(subdivision), count = (sw + 1) * (sh + 1),
						   triangles = sw * sh * 2;
			const double iterations = std::max(0., std::ceil(smoothing));
			if (count > Limits::MaximumArrayElements || triangles > Limits::MaximumLinks ||
				count > budget / 10 || iterations > double((budget - count * 10) / count / 9) ||
				triangles > (budget - count * 10 - uint64_t(iterations) * count * 9) / pixels)
				return c.Fail(
					Status::LimitExceeded, "Grid gradient exceeds processor work budget", "subdivision"
				);
			return true;
		}

		struct SpatialVertex {
			double X = 0, Y = 0;
			Rgba Color{};
		};
		bool SpatialDimensions(NodeContext &c, uint32_t &w, uint32_t &h, Vector2 &raw) {
			if (!ResolveDimension(c, "dimension", w, h)) return false;
			raw = c.Vec2("dimension", {1, 1});
			if (!c.IsLinked("dimension") && c.Integer("dimension_unit", 1) == 1) {
				raw.X *= c.Project.SurfaceWidth;
				raw.Y *= c.Project.SurfaceHeight;
			}
			if (!std::isfinite(raw.X) || !std::isfinite(raw.Y) || raw.X <= 0 || raw.Y <= 0)
				return c.Fail(
					Status::UnsupportedExecution,
					"Spatial gradient source dimensions must be positive",
					"dimension"
				);
			return true;
		}
		Vector2 SpatialPosition(
			const NodeContext &c, std::string_view id, std::string_view unit, Vector2 fallback, Vector2 raw
		) {
			auto p = c.Vec2(id, fallback);
			if (!c.IsLinked(id) && c.Integer(unit, 1) == 1) {
				p.X *= raw.X;
				p.Y *= raw.Y;
			}
			return p;
		}
		Rgba SpatialColour(const NodeContext &c, std::string_view id) {
			const auto p = c.Get<Colour>(id, {255, 255, 255, 255});
			return {p.Red / 255., p.Green / 255., p.Blue / 255., p.Alpha / 255.};
		}
		bool SpatialFinite(const Rgba &p) {
			return std::all_of(p.begin(), p.end(), [](double v) { return std::isfinite(v); });
		}
		// The pinned HTML5 merge_color truncates each RGB channel at every nested merge; alpha is separate.
		Rgba SpatialMerge(const Rgba &a, const Rgba &b, double t) {
			Rgba r{};
			for (size_t k = 0; k < 3; k++)
				r[k] = std::trunc((a[k] * (1 - t) + b[k] * t) * 255.) / 255.;
			r[3] = a[3] + (b[3] - a[3]) * t;
			return r;
		}
		Rgba SpatialLms(const Rgba &c) {
			constexpr double m[3][3] = {
				{.4121656120, .5362752080, .0514575653},
				{.2118591070, .6807189584, .1074065790},
				{.0883097947, .2818474174, .6302613616}
			};
			Rgba r{};
			for (size_t i = 0; i < 3; i++) {
				double n = 0;
				for (size_t j = 0; j < 3; j++)
					n += m[i][j] * std::pow(c[j], 2.2);
				r[i] = std::pow(n, 1. / 3.);
			}
			r[3] = c[3];
			return r;
		}
		Rgba SpatialRgb(const Rgba &c) {
			constexpr double m[3][3] = {
				{4.0767245293, -3.3072168827, .2307590544},
				{-1.2681437731, 2.6093323231, -.3411344290},
				{-.0041119885, -.7034763098, 1.7068625689}
			};
			Rgba r{};
			for (size_t i = 0; i < 3; i++) {
				double n = 0;
				for (size_t j = 0; j < 3; j++)
					n += m[i][j] * c[j] * c[j] * c[j];
				r[i] = std::pow(n, 1. / 2.2);
			}
			r[3] = c[3];
			return r;
		}
	}
	bool SourceGradientPoints(NodeContext &c) {
		if (!SpatialBatchWork(c, false)) return false;
		uint32_t w = 0, h = 0;
		Vector2 raw{};
		if (!SpatialDimensions(c, w, h, raw)) return false;
		if (uint64_t(w) * h > SPATIAL_WORK_LIMIT / std::max(size_t{1}, c.ProcessorCount) / 4)
			return c.Fail(
				Status::LimitExceeded, "Four-point gradient exceeds processor work budget", "dimension"
			);
		const int64_t space = c.Integer("color_space");
		if (space < 0 || space > 1)
			return c.Fail(Status::InvalidValue, "Four-point gradient color space is invalid", "color_space");
		std::array<Vector2, 4> center{};
		std::array<Rgba, 4> colors{};
		std::array<double, 4> strength{};
		constexpr std::array<std::string_view, 4> centerIds{"center_1", "center_2", "center_3", "center_4"},
			unitIds{"center_1_unit", "center_2_unit", "center_3_unit", "center_4_unit"},
			colorIds{"color_1", "color_2", "color_3", "color_4"},
			falloffIds{"falloff_1", "falloff_2", "falloff_3", "falloff_4"};
		const auto *pv = c.Find("palette");
		const auto *pal = pv ? std::get_if<ArrayValue>(pv) : nullptr;
		if (c.Boolean("use_palette") && !pal)
			return c.Fail(Status::InvalidValue, "Four-point gradient requires palette colors", "palette");
		for (size_t i = 0; i < 4; i++) {
			center[i] = SpatialPosition(c, centerIds[i], unitIds[i], {double(i % 2), double(i / 2)}, raw);
			center[i].X /= raw.X;
			center[i].Y /= raw.Y;
			strength[i] = c.Scalar(falloffIds[i], 6);
			colors[i] = SpatialColour(c, colorIds[i]);
			if (c.Boolean("use_palette")) {
				Colour p{0, 0, 0, 255};
				const ElementValue *item = nullptr;
				if (!pal->Items.empty()) {
					if (i < pal->Items.size()) {
						const auto *e = std::get_if<ElementValue>(&pal->Items[i].Data);
						if (!e)
							return c.Fail(
								Status::InvalidValue, "Four-point palette requires flat colors", "palette"
							);
						item = e;
					}
				} else if (i < pal->Elements.size()) {
					item = &pal->Elements[i];
				}
				if (item) {
					const auto *v = std::get_if<Colour>(item);
					if (!v)
						return c.Fail(Status::InvalidValue, "Four-point palette requires colors", "palette");
					p = *v;
				}
				colors[i] = {p.Red / 255., p.Green / 255., p.Blue / 255., p.Alpha / 255.};
			}
			if (!std::isfinite(center[i].X) || !std::isfinite(center[i].Y) || !std::isfinite(strength[i]))
				return c.Fail(Status::InvalidValue, "Four-point controls must be finite", centerIds[i]);
			if (space == 1) colors[i] = SpatialLms(colors[i]);
		}
		const auto fmt = ResolveProcessorSurfaceFormat(c, nullptr);
		if (!fmt) return false;
		auto *out = c.NewImage("surface_out", w, h, *fmt);
		if (!out) return false;
		for (uint32_t y = 0; y < h; y++)
			for (uint32_t x = 0; x < w; x++) {
				double alpha = 1;
				const auto uv = source2d::GeneratorUv(c, (x + .5) / w, (y + .5) / h, alpha);
				std::array<double, 4> weight{};
				double maxDistance = 0;
				for (size_t i = 0; i < 4; i++) {
					weight[i] = std::hypot(uv.X - center[i].X, uv.Y - center[i].Y);
					maxDistance = std::max(maxDistance, weight[i]);
				}
				maxDistance *= 2;
				if (maxDistance == 0)
					return c.Fail(
						Status::UnsupportedExecution,
						"Four-point shader has zero maximum distance",
						"center_1"
					);
				double norm = 0;
				for (size_t i = 0; i < 4; i++) {
					weight[i] = std::pow((maxDistance - weight[i]) / maxDistance, strength[i]);
					norm += c.Boolean("normalize_weight", true) ? weight[i] * weight[i] : weight[i];
				}
				if (c.Boolean("normalize_weight", true)) norm = std::sqrt(norm);
				if (!(norm > 0) || !std::isfinite(norm))
					return c.Fail(
						Status::UnsupportedExecution,
						"Four-point shader weight normalization is undefined",
						"falloff_1"
					);
				Rgba result{};
				for (size_t i = 0; i < 4; i++)
					for (size_t k = 0; k < 4; k++)
						result[k] += colors[i][k] * weight[i] / norm;
				if (space == 1) result = SpatialRgb(result);
				result[3] *= alpha;
				if (!SpatialFinite(result))
					return c.Fail(
						Status::UnsupportedExecution, "Four-point shader color is nonfinite", "color_space"
					);
				if (!WritePixel(*out, x, y, result))
					return c.Fail(
						Status::InvalidValue, "Four-point pixel exceeds surface range", "surface_out"
					);
			}
		return true;
	}
	bool SourceGradientGrid(NodeContext &c) {
		for (const auto &[port, value] : c.ProcessorOriginalValues)
			if (port == "grid" && value && std::holds_alternative<ArrayValue>(*value))
				return c.Fail(
					Status::UnsupportedExecution, "Grid source topology control rejects arrays", "grid"
				);
		if (!SpatialBatchWork(c, true)) return false;
		uint32_t w = 0, h = 0;
		Vector2 raw{};
		if (!SpatialDimensions(c, w, h, raw)) return false;
		const auto grid = c.Vec2("grid", {2, 2});
		const auto subdivision = c.Integer("subdivision", 4);
		double smoothing = c.Scalar("smooth_mesh", .5),
			   colorSmooth = std::max(.0001, c.Scalar("smooth_color", 1));
		if (!std::isfinite(grid.X) || !std::isfinite(grid.Y) || !std::isfinite(smoothing) ||
			!std::isfinite(colorSmooth))
			return c.Fail(Status::InvalidValue, "Grid gradient controls must be finite", "grid");
		if (grid.X < 1 || grid.Y < 1 || grid.X != std::trunc(grid.X) || grid.Y != std::trunc(grid.Y) ||
			subdivision < 1)
			return c.Fail(
				Status::UnsupportedExecution, "Grid gradient requires positive integer topology", "grid"
			);
		if (grid.X > 4096 || grid.Y > 4096 || subdivision > 4096)
			return c.Fail(
				Status::LimitExceeded, "Grid gradient topology exceeds native limits", "subdivision"
			);
		const uint64_t gw = uint64_t(grid.X), gh = uint64_t(grid.Y), sw = gw * uint64_t(subdivision),
					   sh = gh * uint64_t(subdivision), count = (sw + 1) * (sh + 1), triangles = sw * sh * 2;
		if (count > Limits::MaximumArrayElements || triangles > Limits::MaximumLinks)
			return c.Fail(
				Status::LimitExceeded, "Grid gradient subdivision exceeds topology limits", "subdivision"
			);
		const double iterations = std::max(0., std::ceil(smoothing));
		const uint64_t batch = std::max(size_t{1}, c.ProcessorCount), budget = SPATIAL_WORK_LIMIT / batch;
		if (count > budget / 10 || iterations > double((budget - count * 10) / count / 9) ||
			triangles > (budget - count * 10 - uint64_t(iterations) * count * 9) / (uint64_t(w) * h))
			return c.Fail(
				Status::LimitExceeded, "Grid gradient exceeds processor work budget", "subdivision"
			);
		auto charge =
			c.ReserveWorkspace((count + (gw + 1) * (gh + 1)) * sizeof(SpatialVertex), "subdivision");
		if (!charge) return false;
		std::vector<SpatialVertex> anchors(size_t((gw + 1) * (gh + 1))),
			vertices(size_t(count), SpatialVertex{});
		const uint64_t capacityBytes =
			(uint64_t(anchors.capacity()) + uint64_t(vertices.capacity())) * sizeof(SpatialVertex);
		if (!charge->Resize(capacityBytes))
			return c.Fail(
				Status::LimitExceeded, "Grid vector capacity exceeds workspace admission", "subdivision"
			);
		size_t authoredAnchors = 0;
		for (const auto &input : c.Authored.DynamicInputs) {
			size_t group = 0;
			const auto *slot = FindDynamicTemplate(c.Entry, input.Id, group);
			if (slot && slot->SourceIndex == 0) ++authoredAnchors;
		}
		for (uint64_t y = 0; y <= gh; y++)
			for (uint64_t x = 0; x <= gw; x++) {
				const auto i = y * (gw + 1) + x;
				auto &v = anchors[size_t(i)];
				if (authoredAnchors == anchors.size()) {
					const auto anchorPort = SpatialPort("anchor_i_", i),
							   unitPort = SpatialPort("anchor_i_unit_", i),
							   colorPort = SpatialPort("color_i_", i);
					auto p = c.Vec2(anchorPort.View());
					if (!c.IsLinked(anchorPort.View()) && c.Integer(unitPort.View(), 1) == 1) {
						p.X *= raw.X;
						p.Y *= raw.Y;
					}
					v = {p.X, p.Y, SpatialColour(c, colorPort.View())};
				} else
					v = {raw.X * double(x) / double(gw), raw.Y * double(y) / double(gh), {1, 1, 1, 1}};
				if (!std::isfinite(v.X) || !std::isfinite(v.Y))
					return c.Fail(
						Status::InvalidValue, "Grid anchor must be finite", SpatialPort("anchor_i_", i).View()
					);
			}
		const auto lerp = [](double a, double b, double t) { return a + (b - a) * t; };
		for (uint64_t y = 0; y < gh; y++)
			for (uint64_t x = 0; x < gw; x++) {
				const auto &a = anchors[size_t(y * (gw + 1) + x)], &b = anchors[size_t(y * (gw + 1) + x + 1)],
						   &d = anchors[size_t((y + 1) * (gw + 1) + x)],
						   &e = anchors[size_t((y + 1) * (gw + 1) + x + 1)];
				for (int64_t j = 0; j <= subdivision; j++)
					for (int64_t i = 0; i <= subdivision; i++) {
						if ((i == subdivision && x != gw - 1) || (j == subdivision && y != gh - 1)) continue;
						const double u = double(i) / double(subdivision), v = double(j) / double(subdivision),
									 cu = std::clamp(.5 + (u - .5) / colorSmooth, 0., 1.),
									 cv = std::clamp(.5 + (v - .5) / colorSmooth, 0., 1.);
						// The source's final bottom-right geometry uniquely uses color-remapped cx1 for its x
						// interpolation.
						const double gx =
							(i == subdivision && j == subdivision && x == gw - 1 && y == gh - 1) ? cu : u;
						auto &p = vertices[size_t(
							(y * uint64_t(subdivision) + uint64_t(j)) * (sw + 1) + x * uint64_t(subdivision) +
							uint64_t(i)
						)];
						p.X = lerp(lerp(a.X, b.X, gx), lerp(d.X, e.X, gx), v);
						p.Y = lerp(lerp(a.Y, d.Y, v), lerp(b.Y, e.Y, v), gx);
						p.Color = SpatialMerge(
							SpatialMerge(a.Color, b.Color, cu), SpatialMerge(d.Color, e.Color, cu), cv
						);
					}
			}
		while (smoothing > 0) {
			const double amount = std::min(smoothing, 1.);
			for (uint64_t y = 1; y < sh; y++)
				for (uint64_t x = 1; x < sw; x++) {
					double sumX = 0, sumY = 0;
					for (int64_t dx = -1; dx <= 1; dx++)
						for (int64_t dy = -1; dy <= 1; dy++) {
							const auto &p = vertices[size_t(
								uint64_t(int64_t(y) + dy) * (sw + 1) + uint64_t(int64_t(x) + dx)
							)];
							sumX += p.X;
							sumY += p.Y;
						}
					auto &p = vertices[size_t(y * (sw + 1) + x)];
					p.X = lerp(p.X, sumX / 9., amount);
					p.Y = lerp(p.Y, sumY / 9., amount);
				}
			smoothing -= 1;
		}
		const auto fmt = ResolveProcessorSurfaceFormat(c, nullptr);
		if (!fmt) return false;
		auto *out = c.NewImage("surface_out", w, h, *fmt);
		if (!out) return false;
		const auto draw = [&](const SpatialVertex &a, const SpatialVertex &b, const SpatialVertex &d) {
			const double det = (b.X - a.X) * (d.Y - a.Y) - (b.Y - a.Y) * (d.X - a.X);
			if (det == 0) return true;
			if (!std::isfinite(det))
				return c.Fail(Status::InvalidValue, "Grid triangle geometry is nonfinite", "grid");
			for (uint32_t py = 0; py < h; py++)
				for (uint32_t px = 0; px < w; px++) {
					const double dx = px + .5 - a.X, dy = py + .5 - a.Y,
								 wb = (dx * (d.Y - a.Y) - dy * (d.X - a.X)) / det,
								 wd = ((b.X - a.X) * dy - (b.Y - a.Y) * dx) / det, wa = 1 - wb - wd;
					if (wa < 0 || wb < 0 || wd < 0) continue;
					Rgba value{};
					for (size_t k = 0; k < 4; k++)
						value[k] = a.Color[k] * wa + b.Color[k] * wb + d.Color[k] * wd;
					if (!WritePixel(*out, px, py, value))
						return c.Fail(
							Status::InvalidValue, "Grid gradient pixel exceeds surface range", "surface_out"
						);
				}
			return true;
		};
		for (uint64_t y = 0; y < sh; y++)
			for (uint64_t x = 0; x < sw; x++) {
				const auto &a = vertices[size_t(y * (sw + 1) + x)],
						   &b = vertices[size_t(y * (sw + 1) + x + 1)],
						   &d = vertices[size_t((y + 1) * (sw + 1) + x)],
						   &e = vertices[size_t((y + 1) * (sw + 1) + x + 1)];
				if (!draw(a, b, d) || !draw(b, d, e)) return false;
			}
		return true;
	}
}
