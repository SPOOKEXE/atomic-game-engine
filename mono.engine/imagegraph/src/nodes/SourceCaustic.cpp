#include "SourceCaustic.hpp"

#include "../SourceMappedInputs.hpp"
#include "Families.hpp"
#include "Source2DComplexGenerator.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace engine::imagegraph::detail {
	namespace {
		constexpr uint64_t CAUSTIC_WORK_LIMIT = 64000000;
		constexpr uint64_t CAUSTIC_BASE_WORK = 512, CAUSTIC_DETAIL_WORK = 4096;
		using Float3 = std::array<float, 3>;
		using Float4 = std::array<float, 4>;
		struct CausticInputs {
			source2d::ComplexCanvas Canvas;
			SurfaceFormat Format = SurfaceFormat::RGBA8Unorm;
			std::array<float, 2> Dimension{}, Position{}, Scale{}, Progress{}, Intensity{};
			const Image *Uv = nullptr, *Mask = nullptr, *ProgressMap = nullptr, *IntensityMap = nullptr;
			float Seed = 0, UvMix = 1, Amplitude = 0;
			bool Covered = false;
			int32_t Detail = 0;
		};
		bool CausticFloat(NodeContext &c, std::string_view port, double value, float &out) {
			if (!std::isfinite(value) || std::abs(value) > double(std::numeric_limits<float>::max()))
				return c.Fail(Status::InvalidValue, "Caustic value exceeds finite shader range", port);
			out = float(value);
			return true;
		}
		bool CausticPair(NodeContext &c, std::string_view port, Vector2 value, std::array<float, 2> &out) {
			return CausticFloat(c, port, value.X, out[0]) && CausticFloat(c, port, value.Y, out[1]);
		}
		bool CausticSurface(NodeContext &c, std::string_view port, const Image *image) {
			const auto *value = c.Find(port);
			if (value && std::holds_alternative<AtlasValue>(*value))
				return c.Fail(
					Status::UnsupportedExecution, "Caustic raw sampler binding rejects Atlas", port
				);
			return !image ||
				   ValidSurfaceLayout(*image, Limits::MaximumDimension, Limits::MaximumEvaluationBytes) ||
				   c.Fail(Status::InvalidValue, "Caustic sampler layout is invalid", port);
		}
		bool CausticMapped(
			NodeContext &c,
			std::string_view port,
			std::string_view mapPort,
			std::array<float, 2> &range,
			const Image *&image
		) {
			Vector2 value;
			if (!ReadSourceMappedRange(c, port, value)) return false;
			image = c.Input(mapPort);
			if (!CausticSurface(c, mapPort, image)) return false;
			return CausticPair(c, port, value, range);
		}
		bool PrepareCaustic(NodeContext &c, CausticInputs &in) {
			if (!c.Find("seed"))
				return c.Fail(
					Status::UnsupportedExecution, "Caustic requires a resolved source seed", "seed"
				);
			in.Uv = c.Input("uv_map");
			in.Mask = c.Input("mask");
			if (!CausticSurface(c, "uv_map", in.Uv) || !CausticSurface(c, "mask", in.Mask) ||
				!source2d::ResolveGeneratorDimensions(c, in.Mask, in.Canvas.Width, in.Canvas.Height))
				return false;
			if (in.Canvas.Width > c.Request.MaximumImageDimension ||
				in.Canvas.Height > c.Request.MaximumImageDimension)
				return c.Fail(
					Status::LimitExceeded, "Caustic output exceeds request dimensions", "dimension"
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
			Vector2 position, scale;
			if (!source2d::ReferenceVector(c, "position", in.Canvas.Raw, position) ||
				!source2d::ReferenceVector(c, "scale", in.Canvas.Raw, scale))
				return false;
			if (!CausticPair(c, "dimension", in.Canvas.Raw, in.Dimension) ||
				!CausticPair(c, "position", position, in.Position) ||
				!CausticPair(c, "scale", scale, in.Scale) ||
				!CausticFloat(c, "seed", c.Scalar("seed"), in.Seed))
				return false;
			in.Covered = in.Dimension[0] > .5f && in.Dimension[1] > .5f;
			if (in.Uv && !CausticFloat(c, "uv_mix", c.Scalar("uv_mix", 1), in.UvMix)) return false;
			const auto detail = c.Integer("detail", 1);
			if (detail < std::numeric_limits<int32_t>::min() || detail > std::numeric_limits<int32_t>::max())
				return c.Fail(
					Status::UnsupportedExecution, "Caustic detail exceeds shader integer range", "detail"
				);
			in.Detail = int32_t(detail);
			// grug source computes unused amplitude for empty loop. empty loop still paints zero.
			if (in.Covered && in.Detail > 0) {
				// grug unmapped source leaves UseSurf stale and uploads one float into a vec2.
				for (const std::string_view port :
					 {std::string_view("progress"), std::string_view("intensity")})
					if (!SourceRangeMapped(c, port))
						return c.Fail(
							Status::UnsupportedExecution,
							"Caustic unmapped control needs unresolved source shader uniform state",
							port
						);
				if (!CausticMapped(c, "progress", "progress_map", in.Progress, in.ProgressMap) ||
					!CausticMapped(c, "intensity", "intensity_map", in.Intensity, in.IntensityMap))
					return false;
				if (in.Scale[0] == 0 || in.Scale[1] == 0)
					return c.Fail(Status::UnsupportedExecution, "Caustic scale divides by zero", "scale");
				const float power = std::pow(2.f, float(in.Detail));
				in.Amplitude = std::pow(2.f, float(in.Detail) - 1.f) / (power - 1.f);
				if (!std::isfinite(power) || !std::isfinite(in.Amplitude))
					return c.Fail(
						Status::UnsupportedExecution,
						"Caustic amplitude exceeds finite shader range",
						"detail"
					);
			}
			return c.FailureCode == Status::Ok;
		}
		float CausticDot(const Float3 &a, const Float3 &b) {
			return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
		}
		float CausticPermute(float x, float seed) {
			const float value = ((x + seed / 10000.f) * 34.f + 1.f) * x;
			return value - std::floor(value / 289.f) * 289.f;
		}
		// grug keep source gradient, not classic scalar simplex. warps need all four lanes.
		bool CausticSimplex(NodeContext &c, const Float3 &v, float seed, Float4 &out) {
			constexpr float sixth = 1.f / 6.f, third = 1.f / 3.f;
			Float3 cell{}, x0{}, i1{}, i2{};
			const float skew = CausticDot(v, {third, third, third});
			for (size_t k = 0; k < 3; ++k)
				cell[k] = std::floor(v[k] + skew);
			const float unskew = CausticDot(cell, {sixth, sixth, sixth});
			for (size_t k = 0; k < 3; ++k)
				x0[k] = v[k] - cell[k] + unskew;
			const Float3 g{
				x0[0] >= x0[1] ? 1.f : 0.f, x0[1] >= x0[2] ? 1.f : 0.f, x0[2] >= x0[0] ? 1.f : 0.f
			};
			const Float3 l{1.f - g[0], 1.f - g[1], 1.f - g[2]};
			for (size_t k = 0; k < 3; ++k) {
				i1[k] = std::min(g[k], l[(k + 2) % 3]);
				i2[k] = std::max(g[k], l[(k + 2) % 3]);
			}
			std::array<Float3, 4> points{x0, Float3{}, Float3{}, Float3{}};
			for (size_t k = 0; k < 3; ++k) {
				points[1][k] = x0[k] - i1[k] + sixth;
				points[2][k] = x0[k] - i2[k] + third;
				points[3][k] = x0[k] - .5f;
			}
			Float4 perm{}, gx{}, gy{}, h{};
			for (size_t corner = 0; corner < 4; ++corner) {
				const Float3 offset = corner == 0	? Float3{}
									  : corner == 1 ? i1
									  : corner == 2 ? i2
													: Float3{1, 1, 1};
				float p = CausticPermute(cell[2] + offset[2], seed);
				p = CausticPermute(p + cell[1] + offset[1], seed);
				p = CausticPermute(p + cell[0] + offset[0], seed);
				perm[corner] = p;
				const float j = p - 49.f * std::floor(p / 49.f);
				const float x = std::floor(j / 7.f), y = std::floor(j - 7.f * x);
				gx[corner] = (x * 2.f + .5f) / 7.f - 1.f;
				gy[corner] = (y * 2.f + .5f) / 7.f - 1.f;
				h[corner] = 1.f - std::abs(gx[corner]) - std::abs(gy[corner]);
			}
			std::array<Float3, 4> gradients{};
			for (size_t k = 0; k < 4; ++k) {
				const float sh = h[k] <= 0.f ? -1.f : 0.f;
				gradients[k] = {
					gx[k] + (std::floor(gx[k]) * 2.f + 1.f) * sh,
					gy[k] + (std::floor(gy[k]) * 2.f + 1.f) * sh,
					h[k]
				};
			}
			Float3 gradient{};
			Float4 px{}, m4{};
			for (size_t k = 0; k < 4; ++k) {
				const float m = std::max(.6f - CausticDot(points[k], points[k]), 0.f), m2 = m * m,
							m3 = m2 * m;
				m4[k] = m2 * m2;
				px[k] = CausticDot(points[k], gradients[k]);
				for (size_t axis = 0; axis < 3; ++axis) {
					gradient[axis] += -6.f * m3 * points[k][axis] * px[k];
					gradient[axis] += m4[k] * gradients[k][axis];
				}
			}
			out = {
				42.f * gradient[0],
				42.f * gradient[1],
				42.f * gradient[2],
				42.f * (m4[0] * px[0] + m4[1] * px[1] + m4[2] * px[2] + m4[3] * px[3])
			};
			for (float value : perm)
				if (!std::isfinite(value))
					return c.Fail(
						Status::InvalidValue, "Caustic permutation exceeds finite shader range", "seed"
					);
			for (float value : out)
				if (!std::isfinite(value))
					return c.Fail(
						Status::InvalidValue, "Caustic simplex exceeds finite shader range", "scale"
					);
			return true;
		}
		float CausticMap(const std::array<float, 2> &range, const Image *map, float u, float v) {
			if (!map) return range[0];
			const auto sample = SampleNearest(*map, u, v);
			const float amount = (float(sample[0]) + float(sample[1]) + float(sample[2])) / 3.f;
			return range[0] * (1.f - amount) + range[1] * amount;
		}
		bool ShadeCaustic(NodeContext &c, const CausticInputs &in, uint32_t x, uint32_t y, Rgba &pixel) {
			// grug sprite covers raw size. allocated size only chooses which pixels exist.
			if (!in.Covered || float(x) + .5f >= in.Dimension[0] || float(y) + .5f >= in.Dimension[1]) {
				pixel = {0, 0, 0, 0};
				return true;
			}
			float u = (float(x) + .5f) / in.Dimension[0], v = (float(y) + .5f) / in.Dimension[1], alpha = 1;
			const float progress = CausticMap(in.Progress, in.ProgressMap, u, v),
						intensity = CausticMap(in.Intensity, in.IntensityMap, u, v);
			if (!std::isfinite(progress))
				return c.Fail(
					Status::InvalidValue, "Caustic mapped progress exceeds shader range", "progress"
				);
			if (!std::isfinite(intensity))
				return c.Fail(
					Status::InvalidValue, "Caustic mapped intensity exceeds shader range", "intensity"
				);
			if (in.Uv) {
				const auto sample = SampleNearest(*in.Uv, u, v);
				alpha = float(sample[3]);
				u = u * (1.f - in.UvMix) + float(sample[0]) * in.UvMix;
				v = v * (1.f - in.UvMix) + (1.f - float(sample[1])) * in.UvMix;
			}
			if (!std::isfinite(u) || !std::isfinite(v) || !std::isfinite(alpha))
				return c.Fail(Status::InvalidValue, "Caustic UV exceeds shader range", "uv_map");
			if (in.Detail <= 0) {
				pixel = {0, 0, 0, alpha};
				return true;
			}
			float px = u * (in.Dimension[0] / in.Dimension[1]), py = v;
			px = (px - in.Position[0] / in.Dimension[0]) * in.Dimension[0] / in.Scale[0];
			py = (py - in.Position[1] / in.Dimension[1]) * in.Dimension[1] / in.Scale[1];
			if (!std::isfinite(px) || !std::isfinite(py))
				return c.Fail(Status::InvalidValue, "Caustic transform exceeds shader range", "scale");
			float amplitude = in.Amplitude, color = 0;
			for (int32_t iteration = 0; iteration < in.Detail; ++iteration) {
				Float3 position{px, progress, py};
				Float4 noise{};
				for (int warp = 0; warp < 3; ++warp) {
					if (!CausticSimplex(c, position, in.Seed, noise)) return false;
					if (warp < 2)
						for (size_t axis = 0; axis < 3; ++axis)
							position[axis] -= .07f * noise[axis];
				}
				color += std::exp(noise[3] * 3.f - 1.5f) * intensity * amplitude;
				if (!std::isfinite(color))
					return c.Fail(
						Status::InvalidValue, "Caustic accumulation exceeds shader range", "intensity"
					);
				amplitude *= .5f;
				px *= 2.f;
				py *= 2.f;
			}
			pixel = {color, color, color, alpha};
			return true;
		}
		bool QuoteCaustic(NodeContext &c, const CausticInputs &in, uint64_t &work) {
			const bool maskScratch =
				in.Mask || std::any_of(c.ImageArrays.begin(), c.ImageArrays.end(), [](const auto &input) {
					return input.first == "mask" && input.second && !input.second->Images.empty();
				});
			if (!source2d::ComplexBatchAdmission(c, 1, "surface_out", "surface_out", 1 + size_t(maskScratch)))
				return false;
			const uint64_t perPixel =
				CAUSTIC_BASE_WORK + uint64_t(in.Covered ? std::max(in.Detail, 0) : 0) * CAUSTIC_DETAIL_WORK;
			const uint64_t pixels = uint64_t(in.Canvas.Width) * in.Canvas.Height;
			if (work > CAUSTIC_WORK_LIMIT || perPixel > CAUSTIC_WORK_LIMIT ||
				pixels > (CAUSTIC_WORK_LIMIT - work) / perPixel)
				return c.Fail(
					Status::LimitExceeded, "Caustic complete batch exceeds work limit", "surface_out"
				);
			work += pixels * perPixel;
			return true;
		}
		bool DrawCaustic(NodeContext &c) {
			ENGINE_PROFILE("imagegraph.source.caustic");
			CausticInputs in;
			uint64_t work = 0;
			if (!PrepareCaustic(c, in) || !QuoteCaustic(c, in, work)) return false;
			const uint64_t scratchBytes = uint64_t(in.Canvas.Width) * in.Canvas.Height * 4;
			auto charge = c.ReserveWorkspace(in.Mask ? scratchBytes : 0, "surface_out");
			if (!charge) return false;
			Image scratch;
			if (in.Mask)
				scratch = {
					in.Canvas.Width,
					in.Canvas.Height,
					std::vector<uint8_t>(size_t(scratchBytes)),
					0,
					SurfaceFormat::RGBA8Unorm
				};
			auto *output = c.NewImage("surface_out", in.Canvas.Width, in.Canvas.Height, in.Format);
			if (!output) return false;
			for (uint32_t y = 0; y < in.Canvas.Height; ++y)
				for (uint32_t x = 0; x < in.Canvas.Width; ++x) {
					Rgba pixel{};
					if (!ShadeCaustic(c, in, x, y, pixel)) return false;
					if (!WritePixel(*output, x, y, pixel))
						return c.Fail(
							Status::InvalidValue, "Caustic sample exceeds output storage range", "surface_out"
						);
					if (in.Mask) {
						pixel = ReadPixel(*output, x, y);
						const auto mask = SampleNearest(
							*in.Mask, (float(x) + .5f) / in.Canvas.Width, (float(y) + .5f) / in.Canvas.Height
						);
						const float brightness =
							(float(mask[0]) + float(mask[1]) + float(mask[2])) / 3.f * float(mask[3]);
						pixel[3] = float(pixel[3]) * brightness;
						if (!std::isfinite(brightness) || !WritePixel(scratch, x, y, pixel))
							return c.Fail(
								Status::InvalidValue, "Caustic mask exceeds finite storage range", "mask"
							);
					}
				}
			if (in.Mask)
				for (uint32_t y = 0; y < in.Canvas.Height; ++y)
					for (uint32_t x = 0; x < in.Canvas.Width; ++x)
						if (!WritePixel(*output, x, y, ReadPixel(scratch, x, y)))
							return c.Fail(
								Status::InvalidValue,
								"Caustic masked copy exceeds storage range",
								"surface_out"
							);
			return c.FailureCode == Status::Ok;
		}
	}
	bool AdmitSourceCaustic(NodeContext &context, uint64_t &work) {
		CausticInputs in;
		return PrepareCaustic(context, in) && QuoteCaustic(context, in, work);
	}
	std::span<const ExecutorEntry> SourceCausticExecutors() {
		static constexpr ExecutorEntry entries[]{{"pc.caustic", DrawCaustic, true}};
		return entries;
	}
}
