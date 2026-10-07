#include "SourceCellular.hpp"

#include "../SourceMappedInputs.hpp"
#include "Families.hpp"
#include "Source2DComplexGenerator.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>
#include <vector>

namespace engine::imagegraph::detail {
	namespace {
		constexpr uint64_t CELLULAR_WORK_LIMIT = 64000000, CELLULAR_BASE_WORK = 512,
						   CELLULAR_CANDIDATE_WORK = 256;
		constexpr float CELLULAR_TAU = 6.283185307179586f;
		using Float2 = std::array<float, 2>;
		using Float3 = std::array<float, 3>;
		struct CellularInputs {
			source2d::ComplexCanvas Canvas;
			SurfaceFormat Format = SurfaceFormat::RGBA8Unorm;
			Float2 Dimension{}, Position{}, Size{}, Scale{}, LevelIn{}, LevelOut{};
			Float3 GapColor{};
			const Image *Uv = nullptr, *Mask = nullptr, *ScaleMap = nullptr;
			float Seed = 0, Phase = 0, Rotation = 0, Randomness = 1, Gap = 0, UvMix = 1, RadialScale = 2,
				  RadialShatter = 0, IterScale = 2, IterAmplitude = .5f, Amplitude = 0, Contrast = 1,
				  Middle = .5f;
			int32_t Iteration = 1;
			int Type = 0, Pattern = 0, Blend = 0;
			bool Inverted = false, Colored = false;
		};
		bool CellularFinite(NodeContext &c, std::string_view port, float value) {
			return std::isfinite(value) ||
				   c.Fail(Status::InvalidValue, "Cellular shader arithmetic exceeds finite range", port);
		}
		bool CellularFloat(NodeContext &c, std::string_view port, double value, float &out) {
			if (!std::isfinite(value) || std::abs(value) > double(std::numeric_limits<float>::max()))
				return c.Fail(Status::InvalidValue, "Cellular value exceeds shader float range", port);
			out = float(value);
			return true;
		}
		bool CellularPair(NodeContext &c, std::string_view port, Vector2 value, Float2 &out) {
			return CellularFloat(c, port, value.X, out[0]) && CellularFloat(c, port, value.Y, out[1]);
		}
		bool CellularSurface(NodeContext &c, std::string_view port, const Image *image) {
			if (const auto *value = c.Find(port); value && std::holds_alternative<AtlasValue>(*value))
				return c.Fail(
					Status::UnsupportedExecution, "Cellular raw sampler binding rejects Atlas", port
				);
			return !image ||
				   ValidSurfaceLayout(*image, Limits::MaximumDimension, Limits::MaximumEvaluationBytes) ||
				   c.Fail(Status::InvalidValue, "Cellular sampler layout is invalid", port);
		}
		bool PrepareCellular(NodeContext &c, CellularInputs &in) {
			if (!c.Find("seed"))
				return c.Fail(
					Status::UnsupportedExecution, "Cellular requires a resolved source seed", "seed"
				);
			in.Uv = c.Input("uv_map");
			in.Mask = c.Input("mask");
			if (!CellularSurface(c, "uv_map", in.Uv) || !CellularSurface(c, "mask", in.Mask) ||
				!source2d::ResolveGeneratorDimensions(c, in.Mask, in.Canvas.Width, in.Canvas.Height))
				return false;
			if (in.Canvas.Width > c.Request.MaximumImageDimension ||
				in.Canvas.Height > c.Request.MaximumImageDimension)
				return c.Fail(
					Status::LimitExceeded, "Cellular output exceeds request dimensions", "dimension"
				);
			in.Canvas.Raw = c.Vec2("dimension", {1, 1});
			if (!c.IsLinked("dimension")) {
				const auto unit = c.Integer("dimension_unit", 1);
				if (unit == 1) {
					in.Canvas.Raw.X *= c.Project.SurfaceWidth;
					in.Canvas.Raw.Y *= c.Project.SurfaceHeight;
				} else if (unit == 2) {
					in.Canvas.Raw.X *= in.Mask->Width;
					in.Canvas.Raw.Y *= in.Mask->Height;
				}
			}
			const auto format = ResolveProcessorSurfaceFormat(c, nullptr);
			if (!format) return false;
			in.Format = *format;
			Vector2 position;
			if (!source2d::ReferenceVector(c, "position", in.Canvas.Raw, position) ||
				!CellularPair(c, "dimension", in.Canvas.Raw, in.Dimension) ||
				!CellularPair(c, "position", position, in.Position) ||
				!CellularPair(c, "size", c.Vec2("size", {1, 1}), in.Size) ||
				!CellularPair(c, "level_in", c.Vec2("level_in", {0, 1}), in.LevelIn) ||
				!CellularPair(c, "level_out", c.Vec2("level_out", {0, 1}), in.LevelOut))
				return false;
			Vector2 scale;
			if (SourceRangeMapped(c, "scale")) {
				if (!ReadSourceMappedRange(c, "scale", scale)) return false;
				in.ScaleMap = c.Input("scale_map");
				if (!CellularSurface(c, "scale_map", in.ScaleMap)) return false;
			} else {
				const double value = c.Scalar("scale", 4);
				scale = {value, value};
			}
			if (!CellularPair(c, "scale", scale, in.Scale)) return false;
			const auto scalar = [&](std::string_view port, double fallback, float &out) {
				return CellularFloat(c, port, c.Scalar(port, fallback), out);
			};
			if (!scalar("seed", 0, in.Seed) || !scalar("randomness", 1, in.Randomness) ||
				!scalar("gap", 0, in.Gap) || !scalar("radial_scale", 2, in.RadialScale) ||
				!scalar("radial_shatter", 0, in.RadialShatter) || !scalar("iter_scale", 2, in.IterScale) ||
				!scalar("iter_amplitude", .5, in.IterAmplitude) || !scalar("contrast", 1, in.Contrast) ||
				!scalar("middle", .5, in.Middle))
				return false;
			if (!CellularFloat(c, "phase", c.Scalar("phase") / 360., in.Phase) ||
				!CellularFloat(c, "rotation", c.Scalar("rotation") * std::numbers::pi / 180., in.Rotation))
				return false;
			if (in.Uv && !scalar("uv_mix", 1, in.UvMix)) return false;
			const auto type = c.SourceChoice("type"), pattern = c.SourceChoice("pattern"),
					   blend = c.SourceChoice("blend_mode");
			if (type < 0 || type > 3 || std::trunc(type) != type)
				return c.Fail(Status::UnsupportedExecution, "Cellular type selector is undefined", "type");
			if (pattern < 0 || pattern > 2 || std::trunc(pattern) != pattern)
				return c.Fail(
					Status::UnsupportedExecution, "Cellular pattern selector is undefined", "pattern"
				);
			if (blend < 0 || blend > 2 || std::trunc(blend) != blend)
				return c.Fail(
					Status::UnsupportedExecution, "Cellular blend selector is undefined", "blend_mode"
				);
			in.Type = int(type);
			in.Pattern = int(pattern);
			in.Blend = int(blend);
			in.Inverted = c.Boolean("inverted");
			in.Colored = c.Boolean("colored");
			const auto iterations = c.Integer("iteration", 1);
			if (iterations < std::numeric_limits<int32_t>::min() ||
				iterations > std::numeric_limits<int32_t>::max())
				return c.Fail(
					Status::UnsupportedExecution,
					"Cellular iteration exceeds shader integer range",
					"iteration"
				);
			in.Iteration = int32_t(iterations);
			const auto gapColor = source2d::InputColour(c, "gap_color", {0, 0, 0, 255});
			for (size_t i = 0; i < 3; ++i)
				if (!CellularFloat(c, "gap_color", gapColor[i], in.GapColor[i])) return false;
			if (in.LevelIn[0] == in.LevelIn[1])
				return c.Fail(
					Status::UnsupportedExecution, "Cellular equal input levels divide by zero", "level_in"
				);
			if (!CellularFinite(c, "level_in", in.LevelIn[1] - in.LevelIn[0])) return false;
			const bool samples = in.Iteration > 0 && !(in.Type == 3 && in.Blend == 2);
			if (samples) {
				if (in.Dimension[0] == 0 || in.Dimension[1] == 0)
					return c.Fail(
						Status::UnsupportedExecution, "Cellular raw dimension divides by zero", "dimension"
					);
				if ((in.Type == 3 || in.Pattern < 2) && (in.Size[0] == 0 || in.Size[1] == 0))
					return c.Fail(Status::UnsupportedExecution, "Cellular size divides by zero", "size");
			}
			const bool amplitudeConsumed = in.Iteration > 0 && (in.Blend == 0 || in.Type == 2);
			if (amplitudeConsumed) {
				if (in.IterAmplitude <= 0 || in.IterAmplitude == 1)
					return c.Fail(
						Status::UnsupportedExecution,
						"Cellular consumed amplitude has an undefined power domain",
						"iter_amplitude"
					);
				const float reciprocal = 1.f / in.IterAmplitude;
				const float denominator = std::pow(reciprocal, float(in.Iteration)) - 1.f;
				in.Amplitude = std::pow(reciprocal, float(in.Iteration) - 1.f) / denominator;
				if (!std::isfinite(reciprocal) || !std::isfinite(denominator) || denominator == 0 ||
					!std::isfinite(in.Amplitude))
					return c.Fail(
						Status::UnsupportedExecution,
						"Cellular consumed amplitude is undefined",
						"iter_amplitude"
					);
			}
			return c.FailureCode == Status::Ok;
		}
		float CellularFract(float x) {
			return x - std::floor(x);
		}
		float CellularMod(float x, float divisor) {
			return x - divisor * std::floor(x / divisor);
		}
		float CellularDot(Float2 a, Float2 b) {
			return a[0] * b[0] + a[1] * b[1];
		}
		Float2 CellularAdd(Float2 a, Float2 b) {
			return {a[0] + b[0], a[1] + b[1]};
		}
		Float2 CellularSubtract(Float2 a, Float2 b) {
			return {a[0] - b[0], a[1] - b[1]};
		}
		float CellularRandom(Float2 p) {
			return CellularFract(std::sin(CellularDot(p, {12.9898f, 78.233f})) * 43758.5453123f);
		}
		Float2 CellularRandom2(Float2 p) {
			return {
				CellularFract(std::sin(CellularDot(p, {127.1f, 311.7f})) * 43758.5453f),
				CellularFract(std::sin(CellularDot(p, {269.5f, 183.3f})) * 43758.5453f)
			};
		}
		bool CellularVectorFinite(NodeContext &c, std::string_view port, Float2 p) {
			return CellularFinite(c, port, p[0]) && CellularFinite(c, port, p[1]);
		}
		bool CellularInt(NodeContext &c, std::string_view port, float value, int32_t &out) {
			if (!std::isfinite(value) || double(value) < double(std::numeric_limits<int32_t>::min()) ||
				double(value) >= double(std::numeric_limits<int32_t>::max()))
				return c.Fail(
					Status::UnsupportedExecution,
					"Cellular float integer conversion exceeds shader range",
					port
				);
			out = int32_t(value);
			return true;
		}
		bool
		CellularRing(NodeContext &c, const CellularInputs &in, float scale, int32_t ring, int32_t &amount) {
			int32_t base = 0, shatter = 0;
			if (!CellularInt(c, "scale", scale, base) ||
				!CellularInt(c, "radial_shatter", float(ring) * in.RadialShatter, shatter))
				return false;
			const int64_t sum = int64_t(base) + shatter;
			if (sum < std::numeric_limits<int32_t>::min() || sum >= std::numeric_limits<int32_t>::max())
				return c.Fail(
					Status::UnsupportedExecution,
					"Cellular radial count overflows shader integer",
					"radial_shatter"
				);
			amount = int32_t(sum);
			return amount != 0 ||
				   c.Fail(Status::UnsupportedExecution, "Cellular radial angle divides by zero", "scale");
		}
		bool CellularScale(
			NodeContext &c, const CellularInputs &in, float u, float v, float &scale, float &maximum
		) {
			scale = in.Scale[0];
			maximum = std::max(in.Scale[0], in.Scale[1]);
			if (in.ScaleMap) {
				const auto map = SampleNearest(*in.ScaleMap, u, v);
				const float amount = (float(map[0]) + float(map[1]) + float(map[2])) / 3.f;
				scale = in.Scale[0] * (1.f - amount) + in.Scale[1] * amount;
			}
			if (in.Type != 3 && in.Pattern == 0) {
				scale = std::floor(scale);
				maximum = std::floor(maximum);
			}
			return CellularFinite(c, "scale", scale) && CellularFinite(c, "scale", maximum);
		}
		bool CellularCandidates(
			NodeContext &c, const CellularInputs &in, float scale, float maximum, uint64_t &count
		) {
			if (in.Type == 3) {
				if (scale == 0)
					return c.Fail(
						Status::UnsupportedExecution, "Cellular crystal modulus divides by zero", "scale"
					);
				count = 27;
				return CellularFinite(c, "scale", scale * 2.f);
			}
			if (in.Pattern < 2) {
				if ((in.Pattern == 0 || in.Type != 0) && maximum == 0)
					return c.Fail(
						Status::UnsupportedExecution, "Cellular tiled modulus divides by zero", "scale"
					);
				count = in.Type == 0 ? 9 : 34;
				return true;
			}
			int32_t rings = 0;
			if (!CellularInt(c, "scale", scale / 2.f, rings)) return false;
			if (scale == 0 && rings >= 0)
				return c.Fail(
					Status::UnsupportedExecution, "Cellular radial radius divides by zero", "scale"
				);
			count = 0;
			if (rings < 0) return true;
			if (uint64_t(rings) + 1 > CELLULAR_WORK_LIMIT / CELLULAR_CANDIDATE_WORK)
				return c.Fail(
					Status::LimitExceeded, "Cellular radial ring work exceeds limit", "surface_out"
				);
			for (int32_t j = 0; j <= rings; ++j) {
				int32_t amount = 0;
				if (!CellularRing(c, in, scale, j, amount)) return false;
				const uint64_t visits = 1 + uint64_t(amount >= 0 ? int64_t(amount) + 1 : 0);
				if (visits > CELLULAR_WORK_LIMIT / CELLULAR_CANDIDATE_WORK - count)
					return c.Fail(
						Status::LimitExceeded, "Cellular radial candidate work exceeds limit", "surface_out"
					);
				count += visits;
			}
			if (in.Type != 0) count *= 2;
			return true;
		}
		Float2 CellularTransform(
			Float2 ntx, Float2 pos, float scale, float angle, Float2 size, float multiplier = 1.f
		) {
			const Float2 delta = CellularSubtract(ntx, pos);
			const float cosine = std::cos(angle), sine = std::sin(angle);
			return {
				(delta[0] * cosine - delta[1] * sine) * scale * multiplier / size[0],
				(delta[0] * sine + delta[1] * cosine) * scale * multiplier / size[1]
			};
		}
		bool CellularDistance(NodeContext &c, Float2 delta, float &distance) {
			distance = std::sqrt(CellularDot(delta, delta));
			return CellularFinite(c, "randomness", distance);
		}
		bool
		CellularBoundary(NodeContext &c, Float2 nearest, Float2 candidate, float cutoff, float &distance) {
			const auto delta = CellularSubtract(candidate, nearest);
			const float squared = CellularDot(delta, delta);
			if (!CellularFinite(c, "randomness", squared)) return false;
			if (squared > cutoff) {
				const float length = std::sqrt(squared);
				const Float2 normal{delta[0] / length, delta[1] / length};
				const Float2 middle{.5f * (nearest[0] + candidate[0]), .5f * (nearest[1] + candidate[1])};
				const float edge = CellularDot(middle, normal);
				if (!CellularFinite(c, "randomness", edge)) return false;
				distance = std::min(distance, edge);
			}
			return true;
		}
		bool CellularGrid(
			NodeContext &c,
			const CellularInputs &in,
			Float2 ntx,
			Float2 pos,
			float scale,
			float maximum,
			float seed,
			Float3 &result
		) {
			const Float2 st = CellularTransform(ntx, pos, scale, in.Rotation, in.Size);
			if (!CellularVectorFinite(c, "size", st)) return false;
			const Float2 cell{std::floor(st[0]), std::floor(st[1])},
				fraction{CellularFract(st[0]), CellularFract(st[1])};
			float nearestDistance = in.Type == 0 ? 2.f : in.Type == 1 ? 8.f : 1.f;
			Float2 nearest{}, winner{}, pointId{};
			bool hit = false;
			for (int y = -1; y <= 1; ++y)
				for (int x = -1; x <= 1; ++x) {
					const Float2 neighbor{float(x), float(y)};
					Float2 key = CellularAdd(cell, neighbor);
					if (in.Pattern == 0) key = {CellularMod(key[0], maximum), CellularMod(key[1], maximum)};
					const auto point = CellularRandom2(key);
					if (!CellularVectorFinite(c, "scale", point)) return false;
					Float2 pointSample{};
					for (size_t axis = 0; axis < 2; ++axis) {
						const float wave =
							in.Type == 2 ? point[axis] + in.Phase : CellularFract(point[axis] + in.Phase);
						pointSample[axis] = .5f + .5f * std::sin(seed + CELLULAR_TAU * wave) * in.Randomness;
					}
					const auto delta = CellularSubtract(CellularAdd(neighbor, pointSample), fraction);
					float distance = 0;
					if (!CellularDistance(c, delta, distance)) return false;
					if (in.Type == 0)
						nearestDistance = std::min(nearestDistance, distance);
					else if (distance < nearestDistance) {
						nearestDistance = distance;
						nearest = delta;
						winner = neighbor;
						pointId = point;
						hit = true;
					}
				}
			if (in.Type == 0) {
				result = {nearestDistance, 0, 0};
				return true;
			}
			if (!hit)
				return c.Fail(
					Status::UnsupportedExecution,
					"Cellular source nearest candidate remains uninitialized",
					"randomness"
				);
			nearestDistance = 8.f;
			// grug source second pass always wraps and drops randomness, even Uniform pattern.
			for (int y = -2; y <= 2; ++y)
				for (int x = -2; x <= 2; ++x) {
					const auto neighbor = CellularAdd(winner, {float(x), float(y)});
					const auto key = CellularAdd(cell, neighbor);
					const auto point =
						CellularRandom2({CellularMod(key[0], maximum), CellularMod(key[1], maximum)});
					if (!CellularVectorFinite(c, "scale", point)) return false;
					Float2 pointSample{};
					for (size_t axis = 0; axis < 2; ++axis)
						pointSample[axis] =
							.5f + .5f * std::sin(seed + CELLULAR_TAU * CellularFract(point[axis] + in.Phase));
					const auto delta = CellularSubtract(CellularAdd(neighbor, pointSample), fraction);
					if (!CellularBoundary(c, nearest, delta, .000001f, nearestDistance)) return false;
				}
			result = in.Type == 1 ? Float3{nearestDistance, 0, 0}
								  : Float3{pointId[0], pointId[1], nearestDistance};
			return true;
		}
		bool CellularRadialPoint(
			NodeContext &c,
			const CellularInputs &in,
			Float2 pos,
			float scale,
			float seed,
			int32_t ring,
			int32_t index,
			int32_t amount,
			bool second,
			Float2 &point,
			Float2 &id
		) {
			float angle = CELLULAR_TAU / float(amount) * float(index) + float(ring);
			float jitter = 0;
			if (in.Type == 0)
				angle = in.Rotation + CELLULAR_TAU / float(amount) * float(index) + float(ring) +
						(CellularRandom({.684f, 1.387f}) + seed) * in.Randomness;
			else if (in.Type == 1 && !second)
				angle += seed;
			else {
				angle += CellularRandom({.684f, 1.387f});
				angle += seed;
			}
			jitter = CellularRandom({angle, angle});
			const float jitterMultiplier = second || in.Type == 0 ? 1.f : in.Randomness;
			if (ring == 0 && in.RadialScale <= 0)
				return c.Fail(
					Status::UnsupportedExecution,
					"Cellular radial power is undefined at zero radius",
					"radial_scale"
				);
			const float radius =
				std::pow(float(ring) / scale, in.RadialScale) * scale * .5f + jitter * .1f * jitterMultiplier;
			if (!CellularFinite(c, "radial_scale", radius) || !CellularFinite(c, "radial_shatter", angle))
				return false;
			const float phased = angle + CELLULAR_TAU * in.Phase;
			const Float2 neighbor{std::cos(phased) * radius, std::sin(phased) * radius};
			point = CellularAdd(neighbor, pos);
			id = in.Type == 2 ? point : neighbor;
			return CellularVectorFinite(c, "radial_scale", point);
		}
		bool CellularRadial(
			NodeContext &c,
			const CellularInputs &in,
			Float2 ntx,
			Float2 pos,
			float scale,
			float seed,
			Float3 &result
		) {
			int32_t rings = 0;
			if (!CellularInt(c, "scale", scale / 2.f, rings)) return false;
			float distance = in.Type == 0 ? 2.f : in.Type == 1 ? 8.f : 1.f;
			Float2 nearest{}, pointId{};
			bool hit = false;
			for (int32_t j = 0; j <= rings; ++j) {
				int32_t amount = 0;
				if (!CellularRing(c, in, scale, j, amount)) return false;
				for (int32_t i = 0; i <= amount; ++i) {
					Float2 point{}, id{};
					if (!CellularRadialPoint(c, in, pos, scale, seed, j, i, amount, false, point, id))
						return false;
					const auto delta = CellularSubtract(point, ntx);
					float candidate = 0;
					if (!CellularDistance(c, delta, candidate)) return false;
					if (in.Type == 0)
						distance = std::min(distance, candidate);
					else if (candidate < distance) {
						distance = candidate;
						nearest = delta;
						pointId = id;
						hit = true;
					}
				}
			}
			if (in.Type == 0) {
				result = {distance, 0, 0};
				return true;
			}
			if (!hit)
				return c.Fail(
					Status::UnsupportedExecution,
					"Cellular source nearest candidate remains uninitialized",
					"scale"
				);
			distance = 1.f;
			for (int32_t j = 0; j <= rings; ++j) {
				int32_t amount = 0;
				if (!CellularRing(c, in, scale, j, amount)) return false;
				for (int32_t i = 0; i <= amount; ++i) {
					Float2 point{}, id{};
					if (!CellularRadialPoint(c, in, pos, scale, seed, j, i, amount, true, point, id))
						return false;
					if (!CellularBoundary(c, nearest, CellularSubtract(point, ntx), .0001f, distance))
						return false;
				}
			}
			result = in.Type == 1 ? Float3{distance, 0, 0} : Float3{pointId[0], pointId[1], distance};
			return true;
		}
		bool
		CellularCrystal(NodeContext &c, const CellularInputs &in, Float3 input, float scale, float &out) {
			Float3 cell{}, fraction{};
			for (size_t axis = 0; axis < 3; ++axis) {
				cell[axis] = std::floor(input[axis]);
				fraction[axis] = CellularFract(input[axis]);
			}
			Float2 distances{100, 100};
			for (int k = -1; k <= 1; ++k)
				for (int j = -1; j <= 1; ++j)
					for (int i = -1; i <= 1; ++i) {
						const Float3 neighbor{float(i), float(j), float(k)};
						Float3 point{};
						for (size_t axis = 0; axis < 3; ++axis)
							point[axis] = CellularMod(cell[axis] + neighbor[axis], scale * 2.f);
						const Float3 hash{
							CellularFract(std::sin(point[0] + point[1] * 57.f + point[2] * 113.f) * 438.54f),
							CellularFract(std::sin(point[0] * 57.f + point[1] * 113.f + point[2]) * 438.54f),
							CellularFract(std::sin(point[0] * 113.f + point[1] + point[2] * 57.f) * 438.54f)
						};
						Float3 delta{};
						for (size_t axis = 0; axis < 3; ++axis) {
							if (!CellularFinite(c, "scale", hash[axis])) return false;
							delta[axis] = neighbor[axis] - fraction[axis] + hash[axis] * in.Randomness;
						}
						const float squared = delta[0] * delta[0] + delta[1] * delta[1] + delta[2] * delta[2];
						if (!CellularFinite(c, "randomness", squared)) return false;
						const float condition = distances[0] > squared ? 1.f : 0.f,
									negative = 1.f - condition;
						const float condition2 = negative * (distances[1] > squared ? 1.f : 0.f),
									negative2 = 1.f - condition2;
						const Float2 previous = distances;
						distances = {
							squared * condition + previous[0] * negative,
							previous[0] * condition + previous[1] * negative
						};
						distances[1] = condition2 * squared + negative2 * distances[1];
					}
			out = distances[1];
			return true;
		}
		bool CellularLevel(NodeContext &c, const CellularInputs &in, float value, float &out) {
			const float ratio = (value - in.LevelIn[0]) / (in.LevelIn[1] - in.LevelIn[0]);
			out = in.LevelOut[0] * (1.f - ratio) + in.LevelOut[1] * ratio;
			return CellularFinite(c, "level_out", out);
		}
		bool ShadeCellular(NodeContext &c, const CellularInputs &in, uint32_t x, uint32_t y, Rgba &pixel) {
			const float u = (float(x) + .5f) / in.Canvas.Width, v = (float(y) + .5f) / in.Canvas.Height;
			float scale = 0, maximum = 0, alpha = 1;
			Float2 uv{u, v};
			if (in.Uv) {
				const auto map = SampleNearest(*in.Uv, u, v);
				uv = {
					u * (1.f - in.UvMix) + float(map[0]) * in.UvMix,
					v * (1.f - in.UvMix) + (1.f - float(map[1])) * in.UvMix
				};
				alpha = float(map[3]);
			}
			if (!CellularFinite(c, "uv_map", alpha)) return false;
			Float3 accumulated{};
			const bool samples = in.Iteration > 0 && !(in.Type == 3 && in.Blend == 2);
			if (samples) {
				if (!CellularVectorFinite(c, "uv_map", uv) || !CellularScale(c, in, u, v, scale, maximum))
					return false;
				const Float2 ntx{uv[0], uv[1] * (in.Dimension[1] / in.Dimension[0])};
				Float2 position{in.Position[0] / in.Dimension[0], in.Position[1] / in.Dimension[1]};
				if (!CellularVectorFinite(c, "dimension", ntx) ||
					!CellularVectorFinite(c, "position", position))
					return false;
				Float2 crystal{};
				if (in.Type == 3) {
					crystal = CellularTransform(ntx, position, scale, in.Rotation, in.Size, .75f);
					if (!CellularVectorFinite(c, "size", crystal)) return false;
				}
				float seed = CellularMod(in.Seed, 100000.f), amplitude = in.Amplitude;
				for (int32_t iteration = 0; iteration < in.Iteration; ++iteration) {
					Float3 noise{};
					if (in.Type == 3) {
						if (!CellularCrystal(
								c,
								in,
								{crystal[0], crystal[1], CellularMod(in.Seed, 100000.f) / 10.f},
								scale,
								noise[0]
							))
							return false;
					} else {
						Float3 cellular{};
						if (!(in.Pattern == 2
								  ? CellularRadial(c, in, ntx, position, scale, seed, cellular)
								  : CellularGrid(c, in, ntx, position, scale, maximum, seed, cellular)))
							return false;
						if (in.Type == 2) {
							const Float2 id{cellular[0], cellular[1]};
							if (!in.Colored) {
								float color =
									in.Middle +
									(CellularRandom(CellularAdd(id, {1, 1})) - in.Middle) * in.Contrast;
								if (in.Inverted) color = 1.f - color;
								noise = {color * amplitude, color * amplitude, color * amplitude};
							} else {
								noise = {
									CellularRandom(id) * amplitude,
									CellularRandom(CellularAdd(id, {1.7227f, 4.55529f})) * amplitude,
									CellularRandom(CellularAdd(id, {6.9950f, 6.82063f})) * amplitude
								};
							}
							if (cellular[2] < in.Gap) noise = in.GapColor;
						} else
							noise = {cellular[0], cellular[0], cellular[0]};
					}
					for (size_t channel = 0; channel < 3; ++channel) {
						if (!CellularFinite(c, "gap_color", noise[channel])) return false;
						if (in.Blend == 0)
							accumulated[channel] += noise[channel] * amplitude;
						else if (in.Blend == 1)
							accumulated[channel] = std::max(accumulated[channel], noise[channel]);
						else
							accumulated[channel] = std::max(accumulated[channel], 1.f - noise[channel]);
						if (!CellularFinite(c, "iter_amplitude", accumulated[channel])) return false;
					}
					if (in.Type == 3) {
						for (auto &axis : crystal) {
							axis *= in.IterScale;
							axis += CELLULAR_TAU;
						}
					} else {
						seed += 1.753f;
						for (auto &axis : position) {
							axis *= in.IterScale;
							axis += CELLULAR_TAU;
						}
					}
					amplitude *= in.IterAmplitude;
				}
			}
			if (in.Type == 2) {
				for (size_t channel = 0; channel < 3; ++channel) {
					float value = 0;
					if (!CellularLevel(c, in, accumulated[channel], value)) return false;
					pixel[channel] = in.Blend == 2 ? 1.f - value : value;
				}
			} else {
				float distance = accumulated[0];
				if (in.Blend == 2) distance = 1.f - distance;
				float color = in.Middle + (distance - in.Middle) * in.Contrast;
				if (!CellularLevel(c, in, color, color)) return false;
				if (in.Inverted) color = 1.f - color;
				pixel[0] = pixel[1] = pixel[2] = color;
			}
			pixel[3] = alpha;
			return true;
		}
		bool QuoteCellular(NodeContext &c, const CellularInputs &in, uint64_t &work) {
			const bool mask =
				in.Mask || std::any_of(c.ImageArrays.begin(), c.ImageArrays.end(), [](const auto &input) {
					return input.first == "mask" && input.second && !input.second->Images.empty();
				});
			if (!source2d::ComplexBatchAdmission(c, 1, "surface_out", "surface_out", 1 + size_t(mask)))
				return false;
			const uint64_t pixels = uint64_t(in.Canvas.Width) * in.Canvas.Height;
			if (work > CELLULAR_WORK_LIMIT || pixels > (CELLULAR_WORK_LIMIT - work) / CELLULAR_BASE_WORK)
				return c.Fail(
					Status::LimitExceeded, "Cellular complete batch exceeds work limit", "surface_out"
				);
			work += pixels * CELLULAR_BASE_WORK;
			if (in.Iteration <= 0 || (in.Type == 3 && in.Blend == 2)) return true;
			for (uint32_t y = 0; y < in.Canvas.Height; ++y)
				for (uint32_t x = 0; x < in.Canvas.Width; ++x) {
					float scale = 0, maximum = 0;
					uint64_t candidates = 0;
					if (!CellularScale(
							c,
							in,
							(float(x) + .5f) / in.Canvas.Width,
							(float(y) + .5f) / in.Canvas.Height,
							scale,
							maximum
						) ||
						!CellularCandidates(c, in, scale, maximum, candidates))
						return false;
					const uint64_t perIteration = 1024 + candidates * CELLULAR_CANDIDATE_WORK;
					if (uint64_t(in.Iteration) > (CELLULAR_WORK_LIMIT - work) / perIteration)
						return c.Fail(
							Status::LimitExceeded, "Cellular complete batch exceeds work limit", "surface_out"
						);
					work += uint64_t(in.Iteration) * perIteration;
				}
			return true;
		}
		bool DrawCellular(NodeContext &c) {
			ENGINE_PROFILE("imagegraph.source.cellular");
			CellularInputs in;
			uint64_t work = 0;
			if (!PrepareCellular(c, in) || !QuoteCellular(c, in, work)) return false;
			const uint64_t bytes = uint64_t(in.Canvas.Width) * in.Canvas.Height * 4;
			auto charge = c.ReserveWorkspace(in.Mask ? bytes : 0, "surface_out");
			if (!charge) return false;
			Image scratch;
			if (in.Mask)
				scratch = {
					in.Canvas.Width,
					in.Canvas.Height,
					std::vector<uint8_t>(size_t(bytes)),
					0,
					SurfaceFormat::RGBA8Unorm
				};
			auto *output = c.NewImage("surface_out", in.Canvas.Width, in.Canvas.Height, in.Format);
			if (!output) return false;
			for (uint32_t y = 0; y < in.Canvas.Height; ++y)
				for (uint32_t x = 0; x < in.Canvas.Width; ++x) {
					Rgba pixel{};
					if (!ShadeCellular(c, in, x, y, pixel)) return false;
					if (!WritePixel(*output, x, y, pixel))
						return c.Fail(
							Status::InvalidValue,
							"Cellular sample exceeds output storage range",
							"surface_out"
						);
					if (in.Mask) {
						pixel = ReadPixel(*output, x, y);
						const auto mask = SampleNearest(
							*in.Mask, (float(x) + .5f) / in.Canvas.Width, (float(y) + .5f) / in.Canvas.Height
						);
						const float brightness =
							(float(mask[0]) + float(mask[1]) + float(mask[2])) / 3.f * float(mask[3]);
						pixel[3] = float(pixel[3]) * brightness;
						if (!CellularFinite(c, "mask", brightness) || !WritePixel(scratch, x, y, pixel))
							return c.Fail(
								Status::InvalidValue, "Cellular mask exceeds finite storage range", "mask"
							);
					}
				}
			if (in.Mask)
				for (uint32_t y = 0; y < in.Canvas.Height; ++y)
					for (uint32_t x = 0; x < in.Canvas.Width; ++x)
						if (!WritePixel(*output, x, y, ReadPixel(scratch, x, y)))
							return c.Fail(
								Status::InvalidValue,
								"Cellular mask copy exceeds storage range",
								"surface_out"
							);
			return c.FailureCode == Status::Ok;
		}
	}
	bool AdmitSourceCellular(NodeContext &context, uint64_t &batchWork) {
		CellularInputs in;
		return PrepareCellular(context, in) && QuoteCellular(context, in, batchWork);
	}
	std::span<const ExecutorEntry> SourceCellularExecutors() {
		static constexpr ExecutorEntry entries[]{{"pc.cellular", DrawCellular, true}};
		return entries;
	}
}
