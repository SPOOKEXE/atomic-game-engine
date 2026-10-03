#include "../SourceMappedInputs.hpp"
#include "../SourceMirrorPathProjection.hpp"
#include "Curve.hpp"
#include "Sampler.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <type_traits>

namespace engine::imagegraph::detail {
	bool SurfaceSize(NodeContext &, double, double, uint32_t &, uint32_t &);
	namespace {
		constexpr uint64_t PolarWorkLimit = 64'000'000;
		const Value *PolarOriginal(const NodeContext &c, std::string_view port) {
			for (auto it = c.ProcessorOriginalValues.rbegin(); it != c.ProcessorOriginalValues.rend(); ++it)
				if (it->first == port) return it->second;
			for (const auto &[id, value] : c.Values)
				if (id == port) return &value;
			return c.Find(port);
		}
		std::optional<double> PolarNumber(const ElementValue &v) {
			if (const auto *d = std::get_if<double>(&v)) return *d;
			if (const auto *i = std::get_if<int64_t>(&v)) return double(*i);
			if (const auto *e = std::get_if<EnumValue>(&v)) return double(e->Value);
			if (const auto *b = std::get_if<bool>(&v)) return *b ? 1. : 0.;
			return std::nullopt;
		}
		template <class Visit>
		void PolarItems(const std::vector<SourceArrayItem> &items, size_t depth, Visit &visit) {
			if (depth > Limits::MaximumArrayDepth) return;
			for (const auto &item : items) {
				if (const auto *leaf = std::get_if<ElementValue>(&item.Data))
					visit(*leaf);
				else if (const auto *image = std::get_if<Image>(&item.Data))
					visit(*image);
				else
					PolarItems(std::get<std::vector<SourceArrayItem>>(item.Data), depth + 1, visit);
			}
		}
		template <class Visit> void PolarLeaves(const Value *value, Visit &&visit) {
			if (!value) return;
			if (const auto *array = std::get_if<ArrayValue>(value)) {
				for (const auto &leaf : array->Elements)
					visit(leaf);
				for (const auto &row : array->Nested)
					for (const auto &leaf : row)
						visit(leaf);
				PolarItems(array->Items, 0, visit);
			} else
				visit(*value);
		}
		template <class Variant> std::optional<Vector2> PolarTuple(const Variant &value) {
			if (const auto *v = std::get_if<Vector2>(&value)) return *v;
			if (const auto *d = std::get_if<double>(&value)) return Vector2{*d, *d};
			if (const auto *i = std::get_if<int64_t>(&value)) return Vector2{double(*i), double(*i)};
			if (const auto *e = std::get_if<EnumValue>(&value))
				return Vector2{double(e->Value), double(e->Value)};
			if (const auto *b = std::get_if<bool>(&value)) return Vector2{*b ? 1. : 0., *b ? 1. : 0.};
			if (const auto *s = std::get_if<SurfaceValue>(&value))
				return Vector2{double(s->Data.Width), double(s->Data.Height)};
			if (const auto *a = std::get_if<AtlasValue>(&value); a && a->Data)
				return Vector2{double(a->Data->Surface.Data.Width), double(a->Data->Surface.Data.Height)};
			return std::nullopt;
		}
		std::optional<Vector2> PolarTuple(const Image &image) {
			return Vector2{double(image.Width), double(image.Height)};
		}
		std::optional<Vector2> PolarFirstItem(const SourceArrayItem &item, size_t depth) {
			if (depth > Limits::MaximumArrayDepth) return std::nullopt;
			if (const auto *leaf = std::get_if<ElementValue>(&item.Data)) return PolarTuple(*leaf);
			if (const auto *image = std::get_if<Image>(&item.Data)) return PolarTuple(*image);
			const auto &children = std::get<std::vector<SourceArrayItem>>(item.Data);
			if (children.empty()) return std::nullopt;
			if (children.size() >= 2) {
				const auto *a = std::get_if<ElementValue>(&children[0].Data),
						   *b = std::get_if<ElementValue>(&children[1].Data);
				const auto x = a ? PolarNumber(*a) : std::nullopt, y = b ? PolarNumber(*b) : std::nullopt;
				if (x && y) return Vector2{*x, *y};
			}
			return PolarFirstItem(children.front(), depth + 1);
		}
		std::optional<size_t> PolarFirstImageIndex(const ImageArrayItem &item, size_t depth) {
			if (depth > Limits::MaximumArrayDepth) return std::nullopt;
			if (const auto *index = std::get_if<size_t>(&item.Data)) return *index;
			const auto &children = std::get<std::vector<ImageArrayItem>>(item.Data);
			return children.empty() ? std::nullopt : PolarFirstImageIndex(children.front(), depth + 1);
		}
		Vector2 PolarFirstVector(const NodeContext &c, std::string_view port, Vector2 fallback) {
			for (const auto &[id, images] : c.ImageArrays)
				if (id == port && images) {
					const auto index = images->Items.empty() ? std::optional<size_t>{0}
															 : PolarFirstImageIndex(images->Items.front(), 0);
					if (index && *index < images->Images.size()) return *PolarTuple(images->Images[*index]);
				}
			const auto *value = PolarOriginal(c, port);
			if (!value) return fallback;
			const auto *array = std::get_if<ArrayValue>(value);
			if (!array) return PolarTuple(*value).value_or(fallback);
			if (!array->Elements.empty()) {
				if (array->Elements.size() >= 2) {
					const auto x = PolarNumber(array->Elements[0]), y = PolarNumber(array->Elements[1]);
					if (x && y) return {*x, *y};
				}
				return PolarTuple(array->Elements.front()).value_or(fallback);
			}
			if (!array->Nested.empty() && array->Nested.front().size() >= 2) {
				const auto x = PolarNumber(array->Nested.front()[0]),
						   y = PolarNumber(array->Nested.front()[1]);
				if (x && y) return {*x, *y};
			}
			return array->Items.empty() ? fallback
										: PolarFirstItem(array->Items.front(), 0).value_or(fallback);
		}
		double PolarFirstMode(const NodeContext &c) {
			const auto *value = PolarOriginal(c, "output_dimension");
			if (const auto *a = value ? std::get_if<ArrayValue>(value) : nullptr) {
				if (!a->Elements.empty()) return PolarNumber(a->Elements.front()).value_or(0);
				if (!a->Nested.empty() && !a->Nested.front().empty())
					return PolarNumber(a->Nested.front().front()).value_or(0);
				if (!a->Items.empty()) {
					const auto *leaf = std::get_if<ElementValue>(&a->Items.front().Data);
					if (leaf) return PolarNumber(*leaf).value_or(0);
				}
			}
			return double(c.Integer("output_dimension"));
		}
		Vector2 PolarVector(NodeContext &c, std::string_view port, Vector2 fallback) {
			if (const Image *image = c.Input(port)) return {double(image->Width), double(image->Height)};
			if (const auto *value = c.Find(port); value && std::holds_alternative<Path2D>(*value)) {
				c.Fail(
					Status::UnsupportedExecution,
					"Polar Mirror path Vec2 getter requires its authored ratio observation",
					port
				);
				return {};
			}
			if (const auto *value = c.Find(port)) {
				if (const auto tuple = PolarTuple(*value)) return *tuple;
				if (const auto *array = std::get_if<ArrayValue>(value);
					array && array->Nested.empty() && array->Items.empty() && array->Elements.size() >= 2) {
					const auto x = PolarNumber(array->Elements[0]), y = PolarNumber(array->Elements[1]);
					if (x && y) return {*x, *y};
				}
				c.Fail(
					Status::UnsupportedExecution,
					"Polar Mirror vector getter needs a prepared numeric pair or surface",
					port
				);
				return {};
			}
			return fallback;
		}
		Vector2 PolarConstant(NodeContext &c) {
			if (c.IsCatalogueDefault("constant_dimension").value_or(false))
				return {double(c.Project.SurfaceWidth), double(c.Project.SurfaceHeight)};
			return PolarVector(
				c, "constant_dimension", {double(c.Project.SurfaceWidth), double(c.Project.SurfaceHeight)}
			);
		}
		// Reference getters run before processor selection, using raw getDimension(0), not stored dimensions.
		Vector2 PolarDimensions(NodeContext &c, const Image &source, bool first) {
			Vector2 size{double(source.Width), double(source.Height)};
			const double mode = first ? PolarFirstMode(c) : double(c.Integer("output_dimension"));
			if (first) size = PolarFirstVector(c, "surface_in", size);
			if (mode == 1) {
				const auto relative = first ? PolarFirstVector(c, "relative_dimension", {1, 1})
											: PolarVector(c, "relative_dimension", {1, 1});
				size.X *= relative.X;
				size.Y *= relative.Y;
			} else if (mode == 2) {
				size = PolarConstant(c);
				if (first && !c.IsCatalogueDefault("constant_dimension").value_or(false))
					size = PolarFirstVector(c, "constant_dimension", size);
			}
			return size;
		}
		bool PolarBatch(NodeContext &c) {
			if (c.ProcessorRow != 0) return true;
			bool activeFound = false, active = false, inactive = false;
			PolarLeaves(PolarOriginal(c, "active"), [&](const auto &leaf) {
				using T = std::remove_cvref_t<decltype(leaf)>;
				if constexpr (!std::is_same_v<T, Image>) {
					if (const auto *b = std::get_if<bool>(&leaf)) {
						activeFound = true;
						active |= *b;
						inactive |= !*b;
					}
					if (const auto *i = std::get_if<int64_t>(&leaf)) {
						activeFound = true;
						active |= *i != 0;
						inactive |= *i == 0;
					}
					if (const auto *d = std::get_if<double>(&leaf)) {
						activeFound = true;
						active |= *d != 0;
						inactive |= *d == 0;
					}
				}
			});
			if (!activeFound) {
				active = c.Boolean("active", true);
				inactive = !active;
			}
			Vector2 source{1, 1},
				relative{-std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()},
				constant = PolarConstant(c);
			const auto maxima = [&](Vector2 &bounds, Vector2 v) {
				if (!std::isfinite(v.X) || !std::isfinite(v.Y)) {
					c.Fail(
						Status::InvalidValue,
						"Polar Mirror original dimensions must be finite",
						"output_dimension"
					);
					return;
				}
				bounds.X = std::max(bounds.X, v.X);
				bounds.Y = std::max(bounds.Y, v.Y);
			};
			const auto dimensions = [&](std::string_view port, Vector2 &bounds) {
				if (const Image *image = c.Input(port)) maxima(bounds, *PolarTuple(*image));
				for (const auto &[id, images] : c.ImageArrays)
					if (id == port && images)
						for (const auto &image : images->Images)
							maxima(bounds, *PolarTuple(image));
				PolarLeaves(PolarOriginal(c, port), [&](const auto &leaf) {
					if (const auto v = PolarTuple(leaf)) maxima(bounds, *v);
				});
			};
			dimensions("surface_in", source);
			if (active) {
				dimensions("relative_dimension", relative);
				if (relative.X == -std::numeric_limits<double>::infinity()) relative = {1, 1};
				if (!c.IsCatalogueDefault("constant_dimension").value_or(false))
					dimensions("constant_dimension", constant);
			}
			uint8_t modes = 0;
			PolarLeaves(PolarOriginal(c, "output_dimension"), [&](const auto &leaf) {
				using T = std::remove_cvref_t<decltype(leaf)>;
				if constexpr (!std::is_same_v<T, Image>) {
					double v = 0;
					if (const auto *e = std::get_if<EnumValue>(&leaf))
						v = double(e->Value);
					else if (const auto *i = std::get_if<int64_t>(&leaf))
						v = double(*i);
					else if (const auto *d = std::get_if<double>(&leaf))
						v = *d;
					modes |= v == 1 ? 2 : (v == 2 ? 4 : 1);
				}
			});
			if (!modes || !active) modes = 1;
			Vector2 maximum{1, 1};
			if ((modes & 1) || inactive) maxima(maximum, source);
			if (modes & 2) maxima(maximum, {source.X * relative.X, source.Y * relative.Y});
			if (modes & 4) maxima(maximum, constant);
			uint32_t width = 0, height = 0;
			if (c.FailureCode != Status::Ok) return false;
			if (!SurfaceSize(c, maximum.X, maximum.Y, width, height)) return false;
			bool curved = c.Boolean("spokes_curved");
			PolarLeaves(PolarOriginal(c, "spokes_curved"), [&](const auto &leaf) {
				using T = std::remove_cvref_t<decltype(leaf)>;
				if constexpr (!std::is_same_v<T, Image>) {
					if (const auto *b = std::get_if<bool>(&leaf)) curved |= *b;
					if (const auto *d = std::get_if<double>(&leaf)) curved |= *d != 0;
					if (const auto *i = std::get_if<int64_t>(&leaf)) curved |= *i != 0;
				}
			});
			// Includes mapped reads, transcendental polar math, nine paired Lanczos taps and bounded curve
			// Newton work.
			const uint64_t cost = active ? (curved ? 704 : 128) : 1,
						   rows = std::max(uint64_t{1}, uint64_t(c.ProcessorCount));
			if (rows > PolarWorkLimit / cost || uint64_t(width) * height > PolarWorkLimit / cost / rows)
				return c.Fail(
					Status::LimitExceeded,
					"Polar Mirror complete processor batch exceeds work budget",
					"output_dimension"
				);
			return true;
		}
		bool PolarCoordinate(NodeContext &c, double u, double v) {
			return (std::isfinite(u) && std::isfinite(v)) ||
				   c.Fail(
					   Status::InvalidValue, "Polar Mirror sampling coordinate is undefined", "surface_out"
				   );
		}

		// Preserve the source's nine paired Lanczos taps, refusing undefined divisions before a texel cast.
		Rgba PolarTexture(
			NodeContext &c, const Image &image, double u, double v, const SamplerSettings &settings
		) {
			if (!PolarCoordinate(c, u, v)) return {};
			if (settings.Interpolation != 4) return TextureInterpolated(image, u, v, settings);
			const double centerU = u - (Fract(u * image.Width) - .5) / image.Width;
			const double centerV = v - (Fract(v * image.Height) - .5) / image.Height;
			const double offsetX = (u - centerU) * image.Width, offsetY = (v - centerV) * image.Height;
			Rgba colour{};
			double weight = 0;
			for (int x = -1; x <= 1; ++x)
				for (int y = -1; y <= 1; ++y) {
					const double wxa = LanczosWeight(x * 2 - 1 - offsetX, 3),
								 wxb = LanczosWeight(x * 2 - offsetX, 3);
					const double wya = LanczosWeight(y * 2 - 1 - offsetY, 3),
								 wyb = LanczosWeight(y * 2 - offsetY, 3);
					const double wx = wxa + wxb, wy = wya + wyb, w = wx * wy;
					const double su = centerU + (x * 2 - .5 + wxb / wx) / image.Width;
					const double sv = centerV + (y * 2 - .5 + wyb / wy) / image.Height;
					if (!PolarCoordinate(c, su, sv)) return {};
					const auto sample = Texture(image, su, sv, true);
					for (size_t channel = 0; channel < 4; ++channel)
						colour[channel] += w * sample[channel];
					weight += w;
				}
			if (weight == 0 || !std::isfinite(weight)) {
				c.Fail(Status::InvalidValue, "Polar Mirror Lanczos weight is undefined", "interpolate");
				return {};
			}
			for (auto &channel : colour)
				channel /= weight;
			return colour;
		}
	}
	bool SourceMirrorPolar(NodeContext &c) {
		ENGINE_PROFILE("imagegraph.source.mirror_polar");
		bool failed = false;
		if (!PolarBatch(c)) return false;
		if (CopyWhenInactive(c, failed)) return !failed;
		const auto *source = c.Input("surface_in");
		if (!source) return c.Fail(Status::InvalidValue, "Polar Mirror requires its surface", "surface_in");
		const auto size = PolarDimensions(c, *source, false), reference = PolarDimensions(c, *source, true);
		uint32_t width = 0, height = 0;
		if (!SurfaceSize(c, size.X, size.Y, width, height)) return false;
		const auto format = ResolveProcessorSurfaceFormat(c, source);
		if (!format) return false;
		const auto settings = ReadSampler(c);
		const bool red = source->Format == SurfaceFormat::R8Unorm ||
						 source->Format == SurfaceFormat::R16Float ||
						 source->Format == SurfaceFormat::R32Float;
		Vector2 position = PolarVector(c, "position", {}), center = PolarVector(c, "center", {.5, .5}),
				scale = PolarVector(c, "scale", {1, 1});
		const auto positionUnit = c.Integer("position_unit", 1), centerUnit = c.Integer("center_unit", 1);
		if (!c.Input("position") && !SourceMirrorPathSampled(c, "position") && positionUnit == 1) {
			position.X *= reference.X;
			position.Y *= reference.Y;
		}
		if (!c.Input("center") && !SourceMirrorPathSampled(c, "center") && centerUnit == 1) {
			center.X *= reference.X;
			center.Y *= reference.Y;
		}
		const double rotation = c.Scalar("rotation") * (std::numbers::pi / 180),
					 angle = c.Scalar("angle") * (std::numbers::pi / 180), trim = c.Scalar("trim_radius");
		const int64_t radial = c.Integer("radial_scale");
		const bool reflective = c.Boolean("reflective"), mapped = c.Boolean("spokes_mapped");
		Vector2 spokes;
		if (mapped) {
			if (!ReadSourceMappedRange(c, "spokes", spokes)) return false;
		} else {
			const double value = c.Scalar("spokes", 4);
			spokes = {value, value};
		}
		const Image *map = mapped ? c.Input("spokes_map") : nullptr;
		const Curve *curve = nullptr;
		if (!red) {
			if (!SupportedSampler(c, settings)) return false;
			if (!PolarCoordinate(c, reference.X, reference.Y) ||
				!PolarCoordinate(c, position.X, position.Y) || !PolarCoordinate(c, center.X, center.Y) ||
				!PolarCoordinate(c, scale.X, scale.Y) || !PolarCoordinate(c, rotation, angle) ||
				!std::isfinite(trim))
				return false;
			if (c.Boolean("spokes_curved")) {
				const auto *value = c.Find("spokes_curve");
				curve = value ? std::get_if<Curve>(value) : nullptr;
				if (!curve || curve->Anchors.empty() || curve->Anchors.size() > 9 || curve->Header[1] == 0)
					return c.Fail(
						Status::UnsupportedExecution,
						"Polar Mirror curve requires one to nine GLSL anchors and nonzero scale",
						"spokes_curve"
					);
			}
		}
		if (c.FailureCode != Status::Ok) return false;
		auto *output = c.NewImage("surface_out", width, height, *format);
		if (!output) return false;
		const double tau = 2 * std::numbers::pi, cs = std::cos(rotation), sn = std::sin(rotation);
		for (uint32_t y = 0; y < height; ++y)
			for (uint32_t x = 0; x < width; ++x) {
				const double u = (double(x) + .5) / width, v = (double(y) + .5) / height;
				Rgba colour{};
				// Safe single-red drawing replaces the polar shader, retaining only stretched sampling.
				if (red) {
					const auto value = Texture(*source, u, v, Filtered(settings))[0];
					colour = {value, value, value, 1};
				} else {
					double spk = spokes.X;
					if (map) {
						const auto sample = Texture(*map, u, v, false);
						spk += (spokes.Y - spokes.X) * ((sample[0] + sample[1] + sample[2]) / 3);
					}
					const double dx = double(x) + .5 - position.X - center.X,
								 dy = double(y) + .5 - position.Y - center.Y;
					// GLSL vec2 *= mat2 multiplies a row vector by the column-major constructor.
					const double px = dx * cs - dy * sn, py = dx * sn + dy * cs;
					if (!PolarCoordinate(c, px, py)) return false;
					if (curve)
						spk *=
							EvalShaderCurve(*curve, std::hypot(px / width, py / height) / std::sqrt(2.) * 2);
					const double sector = tau / spk * (reflective ? 2 : 1);
					if (!std::isfinite(spk) || spk == 0 || !std::isfinite(sector) || sector == 0)
						return c.Fail(
							Status::InvalidValue, "Polar Mirror spoke divisor is undefined", "spokes"
						);
					double folded = std::atan2(py, px) + angle;
					folded = tau - (folded - tau * std::floor(folded / tau));
					const double quotient = folded / sector;
					if (!std::isfinite(quotient))
						return c.Fail(
							Status::InvalidValue, "Polar Mirror angular modulo is undefined", "spokes"
						);
					folded -= sector * std::floor(quotient);
					if (reflective && folded > sector / 2) folded = sector - folded;
					double distance = std::hypot(px, py);
					if (radial == 0)
						distance *= scale.Y;
					else if (radial == 1)
						distance = std::pow(distance, scale.Y);
					if (!std::isfinite(distance))
						return c.Fail(
							Status::InvalidValue, "Polar Mirror radial distance is undefined", "scale"
						);
					if (trim > 0 && distance > trim * .5 * width) continue;
					const double alpha = (angle + std::numbers::pi) - (folded + angle),
								 incident = (angle + std::numbers::pi) + alpha * scale.X;
					if (!std::isfinite(incident))
						return c.Fail(
							Status::InvalidValue, "Polar Mirror incident angle is undefined", "scale"
						);
					const double su = (center.X + std::cos(incident) * distance) / width,
								 sv = (center.Y - std::sin(incident) * distance) / height;
					if (!PolarCoordinate(c, su, sv)) return false;
					colour = PolarTexture(c, *source, Fract(su), Fract(sv), settings);
					if (c.FailureCode != Status::Ok) return false;
				}
				if (!WritePixel(*output, x, y, colour))
					return c.Fail(
						Status::InvalidValue, "Polar Mirror sample exceeds surface range", "surface_out"
					);
			}
		return true;
	}
}
