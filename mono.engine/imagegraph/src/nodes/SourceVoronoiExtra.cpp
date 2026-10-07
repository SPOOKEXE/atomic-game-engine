#include "SourceVoronoiExtra.hpp"

#include "Families.hpp"
#include "Source2DComplexGenerator.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>
#include <vector>
// grug keep source shader MIT notice in docs/SourceVoronoiExtra-LICENSE.txt.
namespace engine::imagegraph::detail {
	namespace {
		constexpr uint64_t VORONOI_EXTRA_WORK_LIMIT = 64000000, VORONOI_EXTRA_BASE_WORK = 512;
		constexpr float VORONOI_EXTRA_PI = 3.14159265359f, VORONOI_EXTRA_RADIANS = .017453292519943295f;
		using Float2 = std::array<float, 2>;
		using Float3 = std::array<float, 3>;
		struct VoronoiExtraInputs {
			source2d::ComplexCanvas Canvas;
			SurfaceFormat Format = SurfaceFormat::RGBA8Unorm;
			Float2 Dimension{}, Position{}, Scale{}, LevelIn{0, 1}, LevelOut{0, 1};
			const Image *Uv = nullptr, *Mask = nullptr;
			float Seed = 0, Progress = 0, Parameter = 0, Rotation = 0, UvMix = 1;
			int32_t Mode = 0;
			bool Tile = true, Covered = false;
		};
		bool VoronoiExtraFinite(NodeContext &c, std::string_view port, float value) {
			return std::isfinite(value) ||
				   c.Fail(Status::InvalidValue, "Extra Voronoi shader arithmetic exceeds finite range", port);
		}
		bool VoronoiExtraFloat(NodeContext &c, std::string_view port, double value, float &out) {
			if (!std::isfinite(value) || std::abs(value) > double(std::numeric_limits<float>::max()))
				return c.Fail(Status::InvalidValue, "Extra Voronoi value exceeds shader float range", port);
			out = float(value);
			return true;
		}
		bool VoronoiExtraPair(NodeContext &c, std::string_view port, Vector2 value, Float2 &out) {
			return VoronoiExtraFloat(c, port, value.X, out[0]) && VoronoiExtraFloat(c, port, value.Y, out[1]);
		}
		bool VoronoiExtraSurface(NodeContext &c, std::string_view port, const Image *image) {
			if (const auto *value = c.Find(port); value && std::holds_alternative<AtlasValue>(*value))
				return c.Fail(
					Status::UnsupportedExecution, "Extra Voronoi raw sampler binding rejects Atlas", port
				);
			return !image ||
				   ValidSurfaceLayout(*image, Limits::MaximumDimension, Limits::MaximumEvaluationBytes) ||
				   c.Fail(Status::InvalidValue, "Extra Voronoi sampler layout is invalid", port);
		}
		bool PrepareVoronoiExtra(NodeContext &c, VoronoiExtraInputs &in) {
			in.Uv = c.Input("uv_map");
			in.Mask = c.Input("mask");
			if (!VoronoiExtraSurface(c, "uv_map", in.Uv) || !VoronoiExtraSurface(c, "mask", in.Mask) ||
				!source2d::ResolveGeneratorDimensions(c, in.Mask, in.Canvas.Width, in.Canvas.Height))
				return false;
			if (in.Canvas.Width > c.Request.MaximumImageDimension ||
				in.Canvas.Height > c.Request.MaximumImageDimension)
				return c.Fail(
					Status::LimitExceeded, "Extra Voronoi output exceeds request dimensions", "dimension"
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
			if (!VoronoiExtraPair(c, "dimension", in.Canvas.Raw, in.Dimension)) return false;
			const auto format = ResolveProcessorSurfaceFormat(c, nullptr);
			if (!format) return false;
			in.Format = *format;
			const auto mode = c.Integer("mode");
			if (mode < std::numeric_limits<int32_t>::min() || mode > std::numeric_limits<int32_t>::max())
				return c.Fail(
					Status::UnsupportedExecution, "Extra Voronoi mode exceeds shader integer range", "mode"
				);
			in.Mode = int32_t(mode);
			in.Covered = in.Dimension[0] > .5f && in.Dimension[1] > .5f;
			// grug target is cleared before raw sprite draw. no covered fragment reads shader controls.
			if (!in.Covered) return c.FailureCode == Status::Ok;
			if (!VoronoiExtraPair(c, "level_in", c.Vec2("level_in", {0, 1}), in.LevelIn) ||
				!VoronoiExtraPair(c, "level_out", c.Vec2("level_out", {0, 1}), in.LevelOut))
				return false;
			if (in.LevelIn[0] == in.LevelIn[1])
				return c.Fail(
					Status::UnsupportedExecution,
					"Extra Voronoi equal input levels divide by zero",
					"level_in"
				);
			if (!VoronoiExtraFinite(c, "level_in", in.LevelIn[1] - in.LevelIn[0])) return false;
			if (in.Uv && !VoronoiExtraFloat(c, "uv_mix", c.Scalar("uv_mix", 1), in.UvMix)) return false;
			if (in.Mode < 0 || in.Mode > 2) return c.FailureCode == Status::Ok;
			Vector2 position;
			if (!source2d::ReferenceVector(c, "position", in.Canvas.Raw, position) ||
				!VoronoiExtraPair(c, "position", position, in.Position) ||
				!VoronoiExtraPair(c, "scale", c.Vec2("scale", {4, 4}), in.Scale) ||
				!VoronoiExtraFloat(c, "rotation", c.Scalar("rotation"), in.Rotation) ||
				!VoronoiExtraFloat(c, "progress", c.Scalar("progress"), in.Progress))
				return false;
			if (in.Mode == 0 || in.Mode == 2)
				if (!VoronoiExtraFloat(c, "parameter_a", c.Scalar("parameter_a"), in.Parameter)) return false;
			if (in.Mode == 1 || in.Mode == 2) {
				if (!c.Find("seed"))
					return c.Fail(
						Status::UnsupportedExecution,
						"Extra Voronoi consumed hash requires a resolved source seed",
						"seed"
					);
				if (!VoronoiExtraFloat(c, "seed", c.Scalar("seed"), in.Seed)) return false;
			}
			if (in.Mode == 1) in.Tile = c.Boolean("tile", true);
			return c.FailureCode == Status::Ok;
		}
		float VoronoiExtraFract(float value) {
			return value - std::floor(value);
		}
		float VoronoiExtraMod(float value, float divisor) {
			return value - divisor * std::floor(value / divisor);
		}
		float VoronoiExtraDot(Float2 a, Float2 b) {
			return a[0] * b[0] + a[1] * b[1];
		}
		Float2 VoronoiExtraSubtract(Float2 a, Float2 b) {
			return {a[0] - b[0], a[1] - b[1]};
		}
		bool VoronoiExtraVectorFinite(NodeContext &c, std::string_view port, Float2 value) {
			return VoronoiExtraFinite(c, port, value[0]) && VoronoiExtraFinite(c, port, value[1]);
		}
		bool VoronoiExtraHash(NodeContext &c, const VoronoiExtraInputs &in, Float2 point, Float2 &out) {
			const float first = VoronoiExtraDot(point, {127.1f + in.Seed, 311.7f});
			const float second =
				VoronoiExtraDot(point, {269.5f, 183.3f + VoronoiExtraMod(in.Seed, 100000.f) / 10.f});
			if (!VoronoiExtraFinite(c, "seed", first) || !VoronoiExtraFinite(c, "seed", second)) return false;
			out = {
				VoronoiExtraFract(std::sin(first) * 43758.5453f),
				VoronoiExtraFract(std::sin(second) * 43758.5453f)
			};
			return VoronoiExtraVectorFinite(c, "seed", out);
		}
		float VoronoiExtraBlockMetric(const VoronoiExtraInputs &in, Float2 point) {
			point = {VoronoiExtraFract(point[0]) - .5f, VoronoiExtraFract(point[1]) - .5f};
			return std::max(std::abs(point[0]) * (.866f + in.Parameter) + point[1] * .5f, -point[1]);
		}
		bool VoronoiExtraBlock(NodeContext &c, const VoronoiExtraInputs &in, Float2 point, float &out) {
			const Float2 offset{std::sin(1.93f + in.Progress) * .166f, std::sin(in.Progress) * .166f};
			const float a = VoronoiExtraBlockMetric(in, {point[0] + offset[0], point[1]});
			const float b = VoronoiExtraBlockMetric(in, {point[0], point[1] + (.5f + offset[1])});
			const float angle = 120.f * VORONOI_EXTRA_RADIANS, cosine = std::cos(angle),
						sine = std::sin(angle);
			const Float2 shifted{point[0] + .5f, point[1] + .5f};
			// grug this inner matrix multiplies a column. main transform multiplies a row.
			point = {cosine * shifted[0] + sine * shifted[1], -sine * shifted[0] + cosine * shifted[1]};
			if (!VoronoiExtraVectorFinite(c, "scale", point)) return false;
			const float cc = VoronoiExtraBlockMetric(in, {point[0] + offset[0], point[1]});
			const float d = VoronoiExtraBlockMetric(in, {point[0], point[1] + (.5f + offset[1])});
			if (!VoronoiExtraFinite(c, "parameter_a", a) || !VoronoiExtraFinite(c, "parameter_a", b) ||
				!VoronoiExtraFinite(c, "parameter_a", cc) || !VoronoiExtraFinite(c, "parameter_a", d))
				return false;
			out = .1f + std::min(std::min(a, b), std::min(cc, d)) * 2.f;
			return VoronoiExtraFinite(c, "parameter_a", out);
		}
		Float3 VoronoiExtraTriEdges(Float2 point, float sine) {
			return {-point[1], point[0] * sine + point[1] * .5f, point[0] * (-sine) + point[1] * .5f};
		}
		float VoronoiExtraTriDistance(Float2 point, float sine) {
			const auto edges = VoronoiExtraTriEdges(point, sine);
			return std::max(edges[0], std::max(edges[1], edges[2]));
		}
		float VoronoiExtraSign(float value) {
			return value > 0 ? 1.f : value < 0 ? -1.f : 0.f;
		}
		float VoronoiExtraPick(Float3 value, Float3 mask) {
			return value[0] * mask[0] + value[1] * mask[1] + value[2] * mask[2];
		}
		bool VoronoiExtraTriangleBorder(NodeContext &c, Float2 a, Float2 b, float sine, float &out) {
			const auto ta = VoronoiExtraTriEdges(a, sine), tb = VoronoiExtraTriEdges(b, sine),
					   relative = VoronoiExtraTriEdges(VoronoiExtraSubtract(b, a), sine);
			Float3 axis{};
			for (size_t k = 0; k < 3; ++k) {
				const float current = std::abs(relative[k]), next = std::abs(relative[(k + 1) % 3]),
							previous = std::abs(relative[(k + 2) % 3]);
				axis[k] = (1.f - (next >= current ? 1.f : 0.f)) * (current >= previous ? 1.f : 0.f) *
						  VoronoiExtraSign(relative[k]);
			}
			const Float3 zxy{axis[2], axis[0], axis[1]}, yzx{axis[1], axis[2], axis[0]};
			float distance = 0;
			if (axis[0] + axis[1] + axis[2] < 0) {
				const float i = VoronoiExtraPick(ta, axis), j = VoronoiExtraPick(tb, zxy),
							k = VoronoiExtraPick(tb, yzx);
				distance = std::max(i - j, i - k);
			} else {
				const float i = VoronoiExtraPick(tb, axis), j = VoronoiExtraPick(ta, zxy),
							k = VoronoiExtraPick(ta, yzx);
				distance = std::min(i - j, i - k);
			}
			out = std::abs(distance / (sine * 2.f));
			return VoronoiExtraFinite(c, "scale", out);
		}
		bool VoronoiExtraAnimatedPoint(
			NodeContext &c,
			const VoronoiExtraInputs &in,
			Float2 cell,
			Float2 fraction,
			Float2 lattice,
			Float2 scale,
			bool triangle,
			Float2 &out
		) {
			Float2 key{lattice[0] + cell[0], lattice[1] + cell[1]};
			if (triangle && in.Tile)
				for (size_t k = 0; k < 2; ++k)
					key[k] = VoronoiExtraMod(key[k], scale[k] * 2.f);
			if (!VoronoiExtraVectorFinite(c, "scale", key)) return false;
			Float2 hash{};
			if (!VoronoiExtraHash(c, in, key, hash)) return false;
			const float phase = triangle ? in.Progress * VORONOI_EXTRA_PI * 2.f : in.Progress;
			for (size_t k = 0; k < 2; ++k) {
				const float angle = phase + 6.2831f * hash[k];
				if (!VoronoiExtraFinite(c, "progress", angle)) return false;
				const float offset = .5f + .5f * std::sin(angle);
				out[k] = cell[k] + offset * (triangle ? 1.f : in.Parameter) - fraction[k];
			}
			return VoronoiExtraVectorFinite(c, "parameter_a", out);
		}
		bool VoronoiExtraTriangle(
			NodeContext &c, const VoronoiExtraInputs &in, Float2 point, Float2 scale, float &out
		) {
			const Float2 lattice{std::floor(point[0]), std::floor(point[1])},
				fraction{VoronoiExtraFract(point[0]), VoronoiExtraFract(point[1])};
			const float sine = std::sin(VORONOI_EXTRA_PI / 3.f);
			Float2 nearest{}, winner{};
			float distance = 8;
			bool hit = false;
			for (int j = -2; j <= 2; ++j)
				for (int i = -2; i <= 2; ++i) {
					const Float2 cell{float(i), float(j)};
					Float2 candidate{};
					if (!VoronoiExtraAnimatedPoint(c, in, cell, fraction, lattice, scale, true, candidate))
						return false;
					const float current = VoronoiExtraTriDistance(candidate, sine);
					if (!VoronoiExtraFinite(c, "scale", current)) return false;
					if (current < distance) {
						distance = current;
						nearest = candidate;
						winner = cell;
						hit = true;
					}
				}
			if (!hit)
				return c.Fail(
					Status::UnsupportedExecution,
					"Extra Voronoi Triangle nearest source candidate is uninitialized",
					"scale"
				);
			distance = 8;
			for (int j = -3; j <= 3; ++j)
				for (int i = -3; i <= 3; ++i) {
					const Float2 cell{winner[0] + float(i), winner[1] + float(j)};
					Float2 candidate{};
					if (!VoronoiExtraAnimatedPoint(c, in, cell, fraction, lattice, scale, true, candidate))
						return false;
					const float current =
						VoronoiExtraTriDistance(VoronoiExtraSubtract(nearest, candidate), sine);
					if (!VoronoiExtraFinite(c, "scale", current)) return false;
					if (current > .00001f) {
						float border = 0;
						if (!VoronoiExtraTriangleBorder(c, nearest, candidate, sine, border)) return false;
						distance = std::min(distance, border);
					}
				}
			out = distance;
			return true;
		}
		bool VoronoiExtraSquare(NodeContext &c, const VoronoiExtraInputs &in, Float2 point, float &out) {
			const Float2 lattice{std::floor(point[0]), std::floor(point[1])},
				fraction{VoronoiExtraFract(point[0]), VoronoiExtraFract(point[1])};
			Float2 nearest{}, winner{};
			float distance = 8;
			bool hit = false;
			for (int j = -1; j <= 1; ++j)
				for (int i = -1; i <= 1; ++i) {
					const Float2 cell{float(i), float(j)};
					Float2 candidate{};
					if (!VoronoiExtraAnimatedPoint(c, in, cell, fraction, lattice, {}, false, candidate))
						return false;
					const float current = VoronoiExtraDot(candidate, candidate);
					if (!VoronoiExtraFinite(c, "parameter_a", current)) return false;
					if (current < distance) {
						distance = current;
						nearest = candidate;
						winner = cell;
						hit = true;
					}
				}
			if (!hit)
				return c.Fail(
					Status::UnsupportedExecution,
					"Extra Voronoi Square nearest source candidate is uninitialized",
					"parameter_a"
				);
			distance = 8;
			for (int j = -2; j <= 2; ++j)
				for (int i = -2; i <= 2; ++i) {
					const Float2 cell{winner[0] + float(i), winner[1] + float(j)};
					Float2 candidate{};
					if (!VoronoiExtraAnimatedPoint(c, in, cell, fraction, lattice, {}, false, candidate))
						return false;
					const auto delta = VoronoiExtraSubtract(candidate, nearest);
					const float squared = VoronoiExtraDot(delta, delta);
					if (!VoronoiExtraFinite(c, "parameter_a", squared)) return false;
					if (squared > .00001f) {
						const float length = std::sqrt(squared);
						const Float2 normal{delta[0] / length, delta[1] / length},
							middle{.5f * (nearest[0] + candidate[0]), .5f * (nearest[1] + candidate[1])};
						const float border = VoronoiExtraDot(middle, normal);
						if (!VoronoiExtraFinite(c, "parameter_a", border)) return false;
						distance = std::min(distance, border);
					}
				}
			out = distance;
			return true;
		}
		bool
		ShadeVoronoiExtra(NodeContext &c, const VoronoiExtraInputs &in, uint32_t x, uint32_t y, Rgba &pixel) {
			if (!in.Covered || float(x) + .5f >= in.Dimension[0] || float(y) + .5f >= in.Dimension[1]) {
				pixel = {0, 0, 0, 0};
				return true;
			}
			const float u = (float(x) + .5f) / in.Dimension[0], v = (float(y) + .5f) / in.Dimension[1];
			Float2 uv{u, v};
			float alpha = 1;
			if (in.Uv) {
				const auto sample = SampleNearest(*in.Uv, u, v);
				uv = {
					u * (1.f - in.UvMix) + float(sample[0]) * in.UvMix,
					v * (1.f - in.UvMix) + (1.f - float(sample[1])) * in.UvMix
				};
				alpha = float(sample[3]);
			}
			if (!VoronoiExtraFinite(c, "uv_map", alpha)) return false;
			float value = 0;
			if (in.Mode >= 0 && in.Mode <= 2) {
				if (!VoronoiExtraVectorFinite(c, "uv_map", uv)) return false;
				const Float2 ntx{uv[0], uv[1] * (in.Dimension[1] / in.Dimension[0])};
				const Float2 delta{
					ntx[0] - in.Position[0] / in.Dimension[0], ntx[1] - in.Position[1] / in.Dimension[1]
				};
				if (!VoronoiExtraVectorFinite(c, "position", delta)) return false;
				const bool tiling = in.Mode == 1 && in.Tile;
				const float angle =
					(tiling ? std::floor(in.Rotation / 90.f) * 90.f : in.Rotation) * VORONOI_EXTRA_RADIANS;
				Float2 scale{in.Scale[0] / 4.f, in.Scale[1] / 4.f};
				if (tiling)
					for (auto &axis : scale)
						axis = std::floor(axis);
				if (tiling && (scale[0] == 0 || scale[1] == 0))
					return c.Fail(
						Status::UnsupportedExecution,
						"Extra Voronoi tiled Triangle modulus divides by zero",
						"scale"
					);
				if (!VoronoiExtraFinite(c, "rotation", angle)) return false;
				const float cosine = std::cos(angle), sine = std::sin(angle);
				const Float2 pos{
					(delta[0] * cosine - delta[1] * sine) * scale[0],
					(delta[0] * sine + delta[1] * cosine) * scale[1]
				};
				if (!VoronoiExtraVectorFinite(c, "scale", pos)) return false;
				if (in.Mode == 0) {
					if (!VoronoiExtraBlock(c, in, {pos[0], -pos[1]}, value)) return false;
				} else if (in.Mode == 1) {
					if (!VoronoiExtraTriangle(c, in, {pos[0] * 2.f, pos[1] * 2.f}, scale, value))
						return false;
				} else if (!VoronoiExtraSquare(c, in, {pos[0] * 4.f, pos[1] * 4.f}, value))
					return false;
			}
			const float progress = (value - in.LevelIn[0]) / (in.LevelIn[1] - in.LevelIn[0]);
			value = in.LevelOut[0] * (1.f - progress) + in.LevelOut[1] * progress;
			if (!VoronoiExtraFinite(c, "level_out", value)) return false;
			pixel = {value, value, value, alpha};
			return true;
		}
		bool QuoteVoronoiExtra(NodeContext &c, const VoronoiExtraInputs &in, uint64_t &work) {
			const bool mask =
				in.Mask || std::any_of(c.ImageArrays.begin(), c.ImageArrays.end(), [](const auto &input) {
					return input.first == "mask" && input.second && !input.second->Images.empty();
				});
			if (!source2d::ComplexBatchAdmission(
					c, 1, "surface_out", "surface_out", 1 + size_t(mask) + size_t(c.NoiseFieldRequested)
				))
				return false;
			uint64_t perPixel = VORONOI_EXTRA_BASE_WORK;
			if (in.Covered)
				perPixel += in.Mode == 0 ? 512 : in.Mode == 1 ? 74 * 256 : in.Mode == 2 ? 34 * 192 : 0;
			const uint64_t pixels = uint64_t(in.Canvas.Width) * in.Canvas.Height;
			if (work > VORONOI_EXTRA_WORK_LIMIT || pixels > (VORONOI_EXTRA_WORK_LIMIT - work) / perPixel)
				return c.Fail(
					Status::LimitExceeded, "Extra Voronoi complete batch exceeds work limit", "surface_out"
				);
			work += pixels * perPixel;
			if (in.Covered && in.Mode == 1 && in.Tile &&
				(std::floor(in.Scale[0] / 4.f) == 0 || std::floor(in.Scale[1] / 4.f) == 0))
				return c.Fail(
					Status::UnsupportedExecution,
					"Extra Voronoi tiled Triangle modulus divides by zero",
					"scale"
				);
			return true;
		}
		bool DrawVoronoiExtra(NodeContext &c) {
			ENGINE_PROFILE("imagegraph.source.voronoi_extra");
			VoronoiExtraInputs in;
			uint64_t work = 0;
			if (!PrepareVoronoiExtra(c, in) || !QuoteVoronoiExtra(c, in, work)) return false;
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
			auto *out = c.NewImage("surface_out", in.Canvas.Width, in.Canvas.Height, in.Format);
			if (!out) return false;
			for (uint32_t y = 0; y < in.Canvas.Height; ++y)
				for (uint32_t x = 0; x < in.Canvas.Width; ++x) {
					Rgba pixel{};
					if (!ShadeVoronoiExtra(c, in, x, y, pixel)) return false;
					if (!WritePixel(*out, x, y, pixel))
						return c.Fail(
							Status::InvalidValue,
							"Extra Voronoi sample exceeds output storage range",
							"surface_out"
						);
					if (in.Mask) {
						pixel = ReadPixel(*out, x, y);
						const auto mask = SampleNearest(
							*in.Mask, (float(x) + .5f) / in.Canvas.Width, (float(y) + .5f) / in.Canvas.Height
						);
						const float brightness =
							(float(mask[0]) + float(mask[1]) + float(mask[2])) / 3.f * float(mask[3]);
						pixel[3] = float(pixel[3]) * brightness;
						if (!VoronoiExtraFinite(c, "mask", brightness) || !WritePixel(scratch, x, y, pixel))
							return c.Fail(
								Status::InvalidValue,
								"Extra Voronoi mask exceeds finite storage range",
								"mask"
							);
					}
				}
			if (in.Mask)
				for (uint32_t y = 0; y < in.Canvas.Height; ++y)
					for (uint32_t x = 0; x < in.Canvas.Width; ++x)
						if (!WritePixel(*out, x, y, ReadPixel(scratch, x, y)))
							return c.Fail(
								Status::InvalidValue,
								"Extra Voronoi mask copy exceeds storage range",
								"surface_out"
							);
			return c.FailureCode == Status::Ok;
		}
	}
	bool AdmitSourceVoronoiExtra(NodeContext &context, uint64_t &work) {
		VoronoiExtraInputs in;
		return PrepareVoronoiExtra(context, in) && QuoteVoronoiExtra(context, in, work);
	}
	std::span<const ExecutorEntry> SourceVoronoiExtraExecutors() {
		static constexpr ExecutorEntry entries[]{{"pc.voronoi_extra", DrawVoronoiExtra, true}};
		return entries;
	}
}
