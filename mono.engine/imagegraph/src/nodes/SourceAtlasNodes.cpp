#include "../AtlasPayload.hpp"
#include "../SourceRandom.hpp"
#include "ArraySource.hpp"
#include "Curve.hpp"
#include "Families.hpp"
#include "Sampler.hpp"

#include <numbers>
namespace engine::imagegraph::detail {
	namespace {
		// Ports and transform order follow pinned node_atlas_get/set/to_struct/draw/affector.
		// Atlas fields remain owned and independent of their optional rendered preview.
		const AtlasValue *AtlasInput(NodeContext &context, std::string_view port) {
			const auto *value = context.Find(port);
			const auto *atlas = value ? std::get_if<AtlasValue>(value) : nullptr;
			if (!atlas || !atlas->Data || !ValidAtlasPayload(*atlas)) {
				context.Fail(Status::TypeMismatch, "operation requires a bounded source Atlas", port);
				return nullptr;
			}
			return atlas;
		}
		Vector2 AtlasRotate(Vector2 point, Vector2 pivot, double degrees) {
			if (degrees == 0) return point;
			if (degrees == 180) return {2 * pivot.X - point.X, 2 * pivot.Y - point.Y};
			const double angle = -degrees * std::numbers::pi / 180, cosine = std::cos(angle),
						 sine = std::sin(angle);
			const double x = point.X - pivot.X, y = point.Y - pivot.Y;
			return {pivot.X + x * cosine - y * sine, pivot.Y + x * sine + y * cosine};
		}
		Colour AtlasMultiply(Colour a, Colour b) {
			// colorMultiply packs products through source bitwise integer conversion.
			return {
				uint8_t(a.Red * b.Red / 255.0),
				uint8_t(a.Green * b.Green / 255.0),
				uint8_t(a.Blue * b.Blue / 255.0),
				uint8_t(a.Alpha * b.Alpha / 255.0)
			};
		}
		bool AtlasGet(NodeContext &context) {
			const auto *atlas = AtlasInput(context, "input_0");
			if (!atlas) return false;
			auto charge = context.ReserveWorkspace(AtlasStorageBytes(*atlas, true), "input_0");
			if (!charge) return false;
			const auto &a = *atlas->Data;
			const auto &surface = a.Surface.Data;
			if (surface.Width && surface.Height) {
				auto *out = context.NewImage("surface", surface.Width, surface.Height, surface.Format);
				if (!out) return false;
				out->Pixels = surface.Pixels;
				out->Hash = surface.Hash;
			} else
				context.SetValue("surface", SurfaceValue{});
			context.SetValue("position", a.Position);
			context.SetValue("rotation", a.RotationDegrees);
			context.SetValue("scale", a.Scale);
			context.SetValue("blend", a.Blend);
			context.SetValue("alpha", a.Alpha);
			return context.FailureCode == Status::Ok;
		}
		bool AtlasStruct(NodeContext &context) {
			const auto *atlas = AtlasInput(context, "input_0");
			if (!atlas) return false;
			auto charge = context.ReserveWorkspace(
				AtlasStorageBytes(*atlas, true) + sizeof(StructData) +
					7 * (sizeof(std::pair<std::string, Value>) + 16),
				"input_0"
			);
			if (!charge) return false;
			const auto &a = *atlas->Data;
			StructValue result;
			result.Data.emplace();
			result.Data->Fields.reserve(7);
			result.Data->Fields = {
				{"surface", a.Surface},
				{"size", Vector2{double(a.Surface.Data.Width), double(a.Surface.Data.Height)}},
				{"position", a.Position},
				{"rotation", a.RotationDegrees},
				{"scale", a.Scale},
				{"blend", a.Blend},
				{"alpha", a.Alpha}
			};
			context.SetValue("struct", std::move(result));
			return context.FailureCode == Status::Ok;
		}
		bool AtlasSet(NodeContext &context) {
			const auto *atlas = AtlasInput(context, "input_0");
			if (!atlas) return false;
			if (atlas->Data->Kind != AtlasKind::SurfaceAtlas)
				return context.Fail(
					Status::UnsupportedExecution,
					"source Atlas Set calls SurfaceAtlas-only surface.get/setSurface methods",
					"input_0"
				);
			const auto *replacement = context.Input("surface");
			uint64_t bytes =
				AtlasStorageBytes(*atlas, true) + (replacement ? replacement->Pixels.capacity() : 0);
			auto charge = context.ReserveWorkspace(bytes, "input_0");
			if (!charge) return false;
			AtlasValue result = *atlas;
			auto &a = *result.Data;
			const auto &old = atlas->Data->Surface.Data;
			// SurfaceAtlas.clone rebuilds dimensions from its current surface before any replacement.
			a.Dimension = {double(old.Width), double(old.Height)};
			if (context.Boolean("set_surface")) {
				if (!replacement)
					return context.Fail(
						Status::UnsupportedExecution,
						"source Atlas.setSurface requires a valid surface",
						"surface"
					);
				a.Surface.Data = *replacement;
				a.Dimension = {double(replacement->Width), double(replacement->Height)};
			}
			if (context.Boolean("set_position")) {
				auto p = context.Vec2("position");
				if (context.Integer("mode")) {
					a.Position.X += p.X;
					a.Position.Y += p.Y;
				} else
					a.Position = p;
			}
			if (context.Boolean("set_rotation")) {
				double previous = a.RotationDegrees, next = context.Integer("mode_2")
																? previous + context.Scalar("rotation")
																: context.Scalar("rotation");
				a.RotationDegrees = next;
				if (context.Boolean("recalculate_position", true)) {
					Vector2 pivot{old.Width * a.Scale.X / 2, old.Height * a.Scale.Y / 2};
					auto p0 = AtlasRotate({}, pivot, -previous), p1 = AtlasRotate({}, pivot, next);
					a.Position.X += -p0.Y + p1.X;
					a.Position.Y += -p0.X + p1.Y;
				}
			}
			if (context.Boolean("set_scale")) {
				auto scale = context.Vec2("scale", {1, 1}), anchor = context.Vec2("anchor", {.5, .5});
				auto previous = a.Scale;
				int64_t mode = context.Integer("mode_3");
				if (mode == 0)
					a.Scale = scale;
				else if (mode == 1) {
					a.Scale.X += scale.X;
					a.Scale.Y += scale.Y;
				} else if (mode == 2) {
					a.Scale.X *= scale.X;
					a.Scale.Y *= scale.Y;
				}
				a.Position.X -= (a.Scale.X - previous.X) * old.Width * anchor.X;
				a.Position.Y -= (a.Scale.Y - previous.Y) * old.Height * anchor.Y;
			}
			if (context.Boolean("set_blending")) {
				auto blend = context.Get<Colour>("blend", {255, 255, 255, 255});
				a.Blend = context.Integer("mode_4") ? AtlasMultiply(a.Blend, blend) : blend;
			}
			if (context.Boolean("set_alpha")) {
				double alpha = context.Scalar("alpha", 1);
				int64_t mode = context.Integer("mode_5");
				if (mode == 0)
					a.Alpha = alpha;
				else if (mode == 1)
					a.Alpha += alpha;
				else if (mode == 2)
					a.Alpha *= alpha;
			}
			if (!ValidAtlasPayload(result))
				return context.Fail(
					Status::InvalidValue, "source Atlas transform exceeds finite payload bounds", "atlas"
				);
			context.SetValue("atlas", std::move(result));
			return context.FailureCode == Status::Ok;
		}
		std::vector<const AtlasValue *> AtlasRows(const Value *value) {
			std::vector<const AtlasValue *> rows;
			if (!value) return rows;
			if (const auto *atlas = std::get_if<AtlasValue>(value)) {
				rows.push_back(atlas);
				return rows;
			}
			if (const auto *array = std::get_if<ArrayValue>(value)) {
				if (!array->Items.empty()) {
					rows.reserve(array->Items.size());
					for (const auto &item : array->Items) {
						const auto *leaf = std::get_if<ElementValue>(&item.Data);
						rows.push_back(leaf ? std::get_if<AtlasValue>(leaf) : nullptr);
					}
				} else if (!array->Nested.empty()) {
					rows.resize(array->Nested.size());
				} else {
					rows.reserve(array->Elements.size());
					for (const auto &leaf : array->Elements)
						rows.push_back(std::get_if<AtlasValue>(&leaf));
				}
			}
			return rows;
		}
		bool AdmitAtlasRenderWork(
			NodeContext &context,
			size_t count,
			uint32_t width,
			uint32_t height,
			std::string_view port,
			uint64_t itemCost = 0
		) {
			// Bound Lanczos texel reads and blending across the entire processor batch.
			const uint64_t workLimit = 64'000'000 / std::max(uint64_t{1}, uint64_t(context.ProcessorCount));
			constexpr uint64_t layerCost = 48;
			if (count > workLimit / (layerCost + itemCost) ||
				uint64_t(width) * height > (workLimit - count * itemCost) / (1 + count * layerCost))
				return context.Fail(
					Status::LimitExceeded, "Atlas render exceeds bounded batch sampling", port
				);
			return true;
		}
		bool AtlasRender(
			NodeContext &context,
			std::span<const AtlasValue *const> rows,
			std::string_view outputPort,
			uint32_t width,
			uint32_t height,
			Vector2 offset,
			bool blendExt
		) {
			if (!AdmitAtlasRenderWork(context, rows.size(), width, height, outputPort)) return false;
			const uint64_t pixels = uint64_t(width) * height;
			auto *output = context.NewImage(outputPort, width, height, SurfaceFormat::RGBA8Unorm);
			if (!output) return false;
			auto charge = context.ReserveWorkspace(pixels * 12, outputPort);
			if (!charge) return false;
			std::array<Image, 3> scratch;
			for (auto &image : scratch) {
				image.Width = width;
				image.Height = height;
				image.Pixels.resize(pixels * 4);
			}
			SamplerSettings sampler =
				blendExt ? ReadSampler(context)
						 : SamplerSettings{context.InheritedInterpolation, context.InheritedOversample};
			if (!SupportedSampler(context, sampler)) return false;
			for (const auto *atlas : rows) {
				if (!atlas || !atlas->Data) continue;
				const auto &a = *atlas->Data;
				// draw_surface_ext_safe in the blend helper recognizes SurfaceAtlas, not base Atlas.
				if (blendExt && a.Kind != AtlasKind::SurfaceAtlas) continue;
				const auto &source = a.Surface.Data;
				if (!source.Width || !source.Height || a.Scale.X == 0 || a.Scale.Y == 0) continue;
				const double angle = a.RotationDegrees * std::numbers::pi / 180, cosine = std::cos(angle),
							 sine = std::sin(angle);
				for (uint32_t y = 0; y < height; y++)
					for (uint32_t x = 0; x < width; x++) {
						const double dx = x + .5 - a.Position.X - offset.X,
									 dy = y + .5 - a.Position.Y - offset.Y;
						const double u = (cosine * dx - sine * dy) / (source.Width * a.Scale.X),
									 v = (sine * dx + cosine * dy) / (source.Height * a.Scale.Y);
						Rgba front{};
						if (u >= 0 && u < 1 && v >= 0 && v < 1) {
							front = SampleTexture(source, u, v, sampler);
							front[0] *= a.Blend.Red / 255.0;
							front[1] *= a.Blend.Green / 255.0;
							front[2] *= a.Blend.Blue / 255.0;
							front[3] *= a.Alpha;
						}
						if (!WritePixel(scratch[2], x, y, front))
							return context.Fail(
								Status::InvalidValue,
								"Atlas foreground exceeds numeric surface range",
								outputPort
							);
						front = ReadPixel(scratch[2], x, y);
						const auto background = ReadPixel(scratch[0], x, y);
						Rgba result{};
						if (blendExt) {
							const double alpha = front[3] + background[3] * (1 - front[3]);
							if (alpha != 0) {
								for (size_t channel = 0; channel < 3; channel++)
									result[channel] = (front[channel] * front[3] +
													   background[channel] * background[3] * (1 - front[3])) /
													  alpha;
								result[3] = alpha;
							}
						} else {
							for (size_t channel = 0; channel < 3; channel++)
								result[channel] = front[channel] + background[channel] * (1 - front[3]);
							result[3] = front[3] + background[3];
						}
						if (!WritePixel(scratch[1], x, y, result))
							return context.Fail(
								Status::InvalidValue, "Atlas blend exceeds numeric surface range", outputPort
							);
					}
				scratch[0].Pixels.swap(scratch[1].Pixels);
			}
			for (uint32_t y = 0; y < height; y++)
				for (uint32_t x = 0; x < width; x++)
					if (!WritePixel(*output, x, y, ReadPixel(scratch[0], x, y))) return false;
			return context.FailureCode == Status::Ok;
		}
		bool AtlasDraw(NodeContext &context) {
			const auto *value = context.Find("input_1");
			if (value && !ValidRuntimeValue(*value))
				return context.Fail(
					Status::InvalidValue, "Atlas draw input exceeds payload bounds", "input_1"
				);
			size_t count = value ? 1 : 0;
			if (const auto *array = value ? std::get_if<ArrayValue>(value) : nullptr)
				count = !array->Items.empty()	 ? array->Items.size()
						: !array->Nested.empty() ? array->Nested.size()
												 : array->Elements.size();
			auto rowsCharge = context.ReserveWorkspace(count * sizeof(const AtlasValue *), "input_1");
			if (!rowsCharge) return false;
			auto rows = AtlasRows(value);
			uint32_t width = 0, height = 0;
			if (context.Boolean("use_base_dimension", true) && !rows.empty() && rows.front() &&
				rows.front()->Data) {
				const auto &original = rows.front()->Data->OriginalSurface;
				width = original && original->Data.Width ? original->Data.Width : 1;
				height = original && original->Data.Height ? original->Data.Height : 1;
			} else if (!ResolveDimension(context, "dimension", width, height))
				return false;
			const auto padding = context.Get<Vector4>("padding", {});
			const double w = width + padding.X + padding.Z, h = height + padding.Y + padding.W;
			if (!std::isfinite(w) || !std::isfinite(h) || w < 1 || h < 1 || w > Limits::MaximumDimension ||
				h > Limits::MaximumDimension || std::trunc(w) != w || std::trunc(h) != h)
				return context.Fail(
					Status::InvalidValue, "Atlas padding exceeds integral surface dimensions", "padding"
				);
			return AtlasRender(
				context, rows, "surface", uint32_t(w), uint32_t(h), {padding.Z, padding.Y}, true
			);
		}

		double AtlasAngleDifference(double a, double b) {
			return std::fmod(std::fmod(a - b, 360) + 540, 360) - 180;
		}
		double AtlasRoundEven(double a) {
			double lower = std::floor(a), fraction = a - lower;
			return lower + (fraction > .5 || (fraction == .5 && std::fmod(lower, 2) != 0));
		}
		double AtlasSegmentDistance(Vector2 p, Vector2 a, Vector2 b) {
			double dx = b.X - a.X, dy = b.Y - a.Y, den = dx * dx + dy * dy;
			double t = den ? std::clamp(((p.X - a.X) * dx + (p.Y - a.Y) * dy) / den, 0.0, 1.0) : 0;
			return std::hypot(p.X - a.X - t * dx, p.Y - a.Y - t * dy);
		}
		Colour AtlasMerge(Colour a, Colour b, double t) {
			const auto channel = [&](double x, double y) {
				double n = std::fmod(std::trunc(x * (1 - t) + y * t), 256.0);
				if (n < 0) n += 256;
				return uint8_t(n);
			};
			return {channel(a.Red, b.Red), channel(a.Green, b.Green), channel(a.Blue, b.Blue), 0};
		}
		bool AtlasAffector(NodeContext &context) {
			const auto *input = context.Find("atlas_in"), *target = context.Find("target_atlas");
			if ((input && !ValidRuntimeValue(*input)) || (target && !ValidRuntimeValue(*target)))
				return context.Fail(
					Status::InvalidValue, "Atlas affector input exceeds bounded payloads", "atlas_in"
				);
			const auto count = [](const Value *value) {
				if (!value) return size_t(0);
				const auto *a = std::get_if<ArrayValue>(value);
				return a ? (!a->Items.empty()	 ? a->Items.size()
							: !a->Nested.empty() ? a->Nested.size()
												 : a->Elements.size())
						 : size_t(1);
			};
			auto charge = context.ReserveWorkspace(
				(input ? RetainedPayloadBytes(*input) : 0) +
					(count(input) + count(target)) * (sizeof(const AtlasValue *) + sizeof(ElementValue)),
				"atlas_in"
			);
			if (!charge) return false;
			auto rows = AtlasRows(input), targets = AtlasRows(target);
			const int64_t shape = context.Integer("influence_shape");
			const auto *map = context.Input("influence_map");
			if (shape == 2 && !map)
				return context.Fail(
					Status::UnsupportedExecution,
					"source influence map retains prior outputs when no map is supplied",
					"influence_map"
				);
			const bool interpolate = context.Boolean("interpolate") && target &&
									 std::holds_alternative<ArrayValue>(*target) && !targets.empty();
			uint32_t width = 0, height = 0;
			if (!ResolveDimension(context, "dimension", width, height)) return false;
			if (context.Boolean("use_base_dimension", true) && !rows.empty() && rows[0] && rows[0]->Data) {
				const auto &original = rows[0]->Data->OriginalSurface;
				width = original && original->Data.Width ? original->Data.Width : 1;
				height = original && original->Data.Height ? original->Data.Height : 1;
			}
			auto area = context.Get<Area>("area", {});
			if (context.Integer("area_unit", 1) == 1) {
				area.CenterX *= width;
				area.CenterY *= height;
				area.HalfWidth *= width;
				area.HalfHeight *= height;
			}
			const auto wipe = UnitVector(context, "wipe_origin", width, height);
			const double fall = context.Scalar("falloff") * 2, angle = context.Scalar("wipe_angle"),
						 index = context.Scalar("order_index"), noise = context.Scalar("inf_noise"),
						 uniform = context.Scalar("influence");
			const auto *curveValue = context.Find("falloff_curve");
			const auto *curve = curveValue ? std::get_if<Curve>(curveValue) : nullptr;
			const uint64_t influenceCost =
				100 + ((shape == 0 || shape == 1 || shape == 3) && curve ? curve->Anchors.size() : 0);
			if (!AdmitAtlasRenderWork(context, rows.size(), width, height, "rendered", influenceCost))
				return false;
			const double seed = context.Scalar("seed");
			if (!std::isfinite(seed))
				return context.Fail(Status::InvalidValue, "Atlas seed must be finite", "seed");
			double seedWord = std::fmod(std::trunc(seed), 4294967296.0);
			if (seedWord < 0) seedWord += 4294967296.0;
			SourceRandom random{uint32_t(seedWord)};
			ArrayValue result;
			result.ElementType = ValueType::Any;
			result.Items.reserve(rows.size());
			for (size_t i = 0; i < rows.size(); i++) {
				const auto *atlas = rows[i];
				if (!atlas || !atlas->Data) {
					result.Items.push_back({ElementValue{UndefinedValue{}}});
					continue;
				}
				AtlasValue changed = *atlas;
				auto &a = *changed.Data;
				if (a.Kind == AtlasKind::SurfaceAtlas)
					a.Dimension = {double(a.Surface.Data.Width), double(a.Surface.Data.Height)};
				const double aw = a.Dimension.X, ah = a.Dimension.Y;
				const Vector2 center{a.Position.X + aw / 2, a.Position.Y + ah / 2};
				double influence = 0, ratio = 0;
				if (shape == 0 || shape == 1) {
					bool inside = false;
					double distance = 0;
					if (shape == 0) {
						if (area.Shape == 1) {
							double x0 = area.CenterX - area.HalfWidth, x1 = area.CenterX + area.HalfWidth,
								   y0 = area.CenterY - area.HalfHeight, y1 = area.CenterY + area.HalfHeight;
							inside = center.X >= x0 && center.X <= x1 && center.Y >= y0 && center.Y <= y1;
							distance = std::min(
								{AtlasSegmentDistance(center, {x0, y0}, {x1, y0}),
								 AtlasSegmentDistance(center, {x0, y1}, {x1, y1}),
								 AtlasSegmentDistance(center, {x0, y0}, {x0, y1}),
								 AtlasSegmentDistance(center, {x1, y0}, {x1, y1})}
							);
						} else if (area.Shape == 0) {
							double direction = -std::atan2(center.Y - area.CenterY, center.X - area.CenterX);
							Vector2 edge{
								area.CenterX + area.HalfWidth * std::cos(direction),
								area.CenterY - area.HalfHeight * std::sin(direction)
							};
							inside = std::hypot(center.X - area.CenterX, center.Y - area.CenterY) <
									 std::hypot(edge.X - area.CenterX, edge.Y - area.CenterY);
							distance = std::hypot(center.X - edge.X, center.Y - edge.Y);
						}
					} else {
						double direction =
								   -std::atan2(center.Y - wipe.Y, center.X - wipe.X) * 180 / std::numbers::pi,
							   delta = AtlasAngleDifference(direction, angle);
						inside = delta < 0;
						distance = std::abs(
							std::hypot(center.X - wipe.X, center.Y - wipe.Y) *
							std::sin(delta * std::numbers::pi / 180)
						);
					}
					ratio = inside ? .5 + distance / fall : .5 - distance / fall;
					if (std::isnan(ratio))
						return context.Fail(
							Status::UnsupportedExecution,
							"source Atlas influence divides zero distance by zero falloff",
							"falloff"
						);
					if (!curve)
						return context.Fail(
							Status::TypeMismatch, "Atlas influence requires its source curve", "falloff_curve"
						);
					influence = EvalCurveX(*curve, std::clamp(ratio, 0.0, 1.0));
				} else if (shape == 2) {
					if (map->Format != SurfaceFormat::RGBA8Unorm && map->Format != SurfaceFormat::R16Float &&
						map->Format != SurfaceFormat::R32Float)
						return context.Fail(
							Status::UnsupportedExecution,
							"source influence map color getter cannot consume float pixel arrays",
							"influence_map"
						);
					const uint32_t mx = uint32_t(
									   std::clamp(AtlasRoundEven(center.X), 0.0, double(map->Width - 1))
								   ),
								   my = uint32_t(
									   std::clamp(AtlasRoundEven(center.Y), 0.0, double(map->Height - 1))
								   );
					auto pixel = ReadPixel(*map, mx, my);
					if (map->Format == SurfaceFormat::RGBA8Unorm)
						influence = .299 * pixel[0] + .587 * pixel[1] + .224 * pixel[2];
					else {
						double packed = std::fmod(std::trunc(pixel[0]), 4294967296.0);
						if (packed < 0) packed += 4294967296.0;
						uint32_t bits = uint32_t(packed);
						influence =
							(.299 * (bits & 255) + .587 * ((bits >> 8) & 255) + .224 * ((bits >> 16) & 255)) /
							255;
					}
				} else if (shape == 3) {
					ratio = std::abs(double(i) - index) / fall;
					if (std::isnan(ratio))
						return context.Fail(
							Status::UnsupportedExecution,
							"source order influence divides zero by zero falloff",
							"falloff"
						);
					if (!curve)
						return context.Fail(
							Status::TypeMismatch, "Atlas influence requires its source curve", "falloff_curve"
						);
					influence = EvalCurveX(*curve, std::clamp(ratio, 0.0, 1.0));
				} else if (shape == 4) {
					const double start = random.Unit() * noise / 2, end = 1 - random.Unit() * noise / 2;
					influence = std::clamp((uniform - start) / (end - start), 0.0, 1.0);
				}
				if (!std::isfinite(influence))
					return context.Fail(
						Status::UnsupportedExecution, "source Atlas influence is nonfinite", "influence_shape"
					);
				if (context.Boolean("effect_position")) {
					auto position = context.Vec2("position");
					auto axes = context.Integer("axis", 3);
					bool relative = context.Integer("mode") != 0;
					if (axes & 1)
						a.Position.X = relative ? a.Position.X + position.X * influence
												: std::lerp(a.Position.X, position.X, influence);
					if (axes & 2)
						a.Position.Y = relative ? a.Position.Y + position.Y * influence
												: std::lerp(a.Position.Y, position.Y, influence);
				}
				if (context.Boolean("set_rotation")) {
					double previous = a.RotationDegrees, next = context.Integer("mode_2")
																	? previous + context.Scalar("rotation")
																	: context.Scalar("rotation");
					a.RotationDegrees = previous + AtlasAngleDifference(next, previous) * influence;
					if (context.Boolean("recalculate_position", true)) {
						Vector2 pivot{aw * a.Scale.X / 2, ah * a.Scale.Y / 2};
						auto p0 = AtlasRotate({}, pivot, -previous),
							 p1 = AtlasRotate({}, pivot, a.RotationDegrees);
						a.Position.X += -p0.Y + p1.X;
						a.Position.Y += -p0.X + p1.Y;
					}
				}
				if (context.Boolean("set_scale")) {
					auto scale = context.Vec2("scale", {1, 1}), anchor = context.Vec2("anchor", {.5, .5});
					auto old = a.Scale;
					auto mode = context.Integer("mode_3"), axes = context.Integer("axis_2", 3);
					Vector2 next = mode == 0   ? scale
								   : mode == 1 ? Vector2{old.X + scale.X, old.Y + scale.Y}
											   : Vector2{old.X * scale.X, old.Y * scale.Y};
					if (axes & 1) {
						a.Scale.X = std::lerp(old.X, next.X, influence);
						a.Position.X -= (a.Scale.X - old.X) * aw * anchor.X;
					}
					if (axes & 2) {
						a.Scale.Y = std::lerp(old.Y, next.Y, influence);
						a.Position.Y -= (a.Scale.Y - old.Y) * ah * anchor.Y;
					}
				}
				if (context.Boolean("set_blending")) {
					auto blend = context.Get<Colour>("blend", {255, 255, 255, 255});
					a.Blend = AtlasMerge(
						a.Blend, context.Integer("mode_4") ? AtlasMultiply(a.Blend, blend) : blend, influence
					);
				}
				if (context.Boolean("set_alpha")) {
					double alpha = context.Scalar("alpha", 1), next = a.Alpha;
					auto mode = context.Integer("mode_5");
					if (mode == 0)
						next = alpha;
					else if (mode == 1)
						next += alpha;
					else if (mode == 2)
						next *= alpha;
					a.Alpha = std::lerp(a.Alpha, next, influence);
				}
				if (interpolate) {
					const auto *targetAtlas = targets[i % targets.size()];
					if (!targetAtlas || !targetAtlas->Data)
						return context.Fail(
							Status::UnsupportedExecution,
							"source Atlas interpolation target has no draw fields",
							"target_atlas"
						);
					const auto &b = *targetAtlas->Data;
					a.Position = {
						std::lerp(a.Position.X, b.Position.X, influence),
						std::lerp(a.Position.Y, b.Position.Y, influence)
					};
					a.RotationDegrees = std::lerp(a.RotationDegrees, b.RotationDegrees, influence);
					a.Scale = {
						std::lerp(a.Scale.X, b.Scale.X, influence), std::lerp(a.Scale.Y, b.Scale.Y, influence)
					};
				}
				if (!ValidAtlasPayload(changed))
					return context.Fail(
						Status::InvalidValue,
						"Atlas affector exceeds bounded finite draw payload",
						"atlas_out"
					);
				result.Items.push_back({ElementValue{std::move(changed)}});
			}
			std::vector<const AtlasValue *> outputRows;
			outputRows.reserve(result.Items.size());
			for (const auto &item : result.Items)
				outputRows.push_back(std::get_if<AtlasValue>(&std::get<ElementValue>(item.Data)));
			if (!AtlasRender(context, outputRows, "rendered", width, height, {}, false)) return false;
			context.SetValue("atlas_out", std::move(result));
			return context.FailureCode == Status::Ok;
		}

	}
	std::span<const ExecutorEntry> SourceAtlasExecutors() {
		static const ExecutorEntry entries[] = {
			{"pc.atlas_get", AtlasGet, true},
			{"pc.atlas_set", AtlasSet, true},
			{"pc.atlas_struct", AtlasStruct, true},
			{"pc.atlas_draw", AtlasDraw, true},
			{"pc.atlas_affector", AtlasAffector, true}
		};
		return entries;
	}
}
