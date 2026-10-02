#include "Families.hpp"
#include "Sampler.hpp"

#include <numbers>
namespace engine::imagegraph::detail {
	namespace {
		// Node_Atlas is Pixel Expand. Its scratch surfaces are RGBA8, independently of output depth.
		bool AtlasExpand(NodeContext &context) {
			bool inactiveFailed = false;
			if (CopyWhenInactive(context, inactiveFailed)) return context.FailureCode == Status::Ok;
			const auto *source = context.Input("surface_in");
			if (!source)
				return context.Fail(
					Status::TypeMismatch, "pixel expand requires a source surface", "surface_in"
				);
			const auto format = ResolveProcessorSurfaceFormat(context, source);
			if (!format) return false;
			const int64_t method = context.Integer("method");
			const uint64_t pixels = uint64_t(source->Width) * source->Height;
			const float direction = float(context.Scalar("direction")),
						resolution = float(context.Integer("resolution", 32));
			const bool both = context.Boolean("both_side");
			const uint64_t iterations = (std::max(source->Width, source->Height) + 15) / 16;
			uint64_t perPixel = 1;
			if (method == 0) {
				const float angular = resolution * 6.283185307179586f * 2;
				if (!std::isfinite(angular) || angular == 0)
					return context.Fail(
						Status::UnsupportedExecution,
						"source radial expansion divides by zero angular sample count",
						"resolution"
					);
				if (angular >= 64'000'000)
					return context.Fail(
						Status::LimitExceeded,
						"radial expansion exceeds bounded angular samples",
						"resolution"
					);
				perPixel = iterations * (1 + 32 * (angular < 0 ? 0 : uint64_t(std::floor(angular)) + 1)) + 1;
			} else if (method == 1)
				perPixel = 2 * (uint64_t(source->Width) + source->Height) + 2;
			else if (method == 2) {
				if (!std::isfinite(direction))
					return context.Fail(
						Status::InvalidValue,
						"pixel expansion direction exceeds finite shader uniforms",
						"direction"
					);
				perPixel = (uint64_t(source->Width) + source->Height) * (both ? 2 : 1) + 1;
			} else
				return context.Fail(
					Status::UnsupportedExecution, "source pixel expand method has no content case", "method"
				);
			const uint64_t workLimit = 64'000'000 / std::max(uint64_t{1}, uint64_t(context.ProcessorCount));
			if (perPixel > workLimit || pixels > workLimit / perPixel)
				return context.Fail(
					Status::LimitExceeded, "pixel expansion exceeds bounded sample work", "resolution"
				);
			auto *output = context.NewImage("surface_out", source->Width, source->Height, *format);
			if (!output) return false;
			auto charge = context.ReserveWorkspace(pixels * 8, "surface_in");
			if (!charge) return false;
			std::array<Image, 2> scratch;
			for (auto &image : scratch) {
				image.Width = source->Width;
				image.Height = source->Height;
				image.Pixels.resize(pixels * 4);
			}
			const auto sample = [](const Image &image, float u, float v) {
				auto p = SampleNearest(image, u, v);
				return Rgba{float(p[0]), float(p[1]), float(p[2]), float(p[3])};
			};
			const auto store = [&](Image &image, uint32_t x, uint32_t y, const Rgba &pixel) {
				if (WritePixel(image, x, y, pixel)) return true;
				return context.Fail(
					Status::UnsupportedExecution,
					"pixel expansion source shader produced a nonfinite sample",
					"surface_out"
				);
			};
			for (uint32_t y = 0; y < source->Height; y++)
				for (uint32_t x = 0; x < source->Width; x++)
					if (!store(scratch[0], x, y, ReadPixel(*source, x, y))) return false;
			if (method == 0) {
				const float tau = 6.283185307179586f, angular = resolution * tau * 2;
				for (uint64_t pass = 0; pass < iterations; pass++) {
					for (uint32_t y = 0; y < source->Height; y++)
						for (uint32_t x = 0; x < source->Width; x++) {
							const float u = float((x + .5) / source->Width),
										v = float((y + .5) / source->Height);
							Rgba result = sample(scratch[0], u, v);
							bool found = false;
							if (result[3] != 1)
								for (float distance = 1; distance <= 32 && !found; distance++) {
									float minimum = 9999;
									for (float j = 0; j <= angular; j++) {
										const float
											angle = j / angular * tau,
											px =
												std::floor(float(x) + .5f + std::cos(angle) * distance) + .5f,
											py =
												std::floor(float(y) + .5f + std::sin(angle) * distance) + .5f;
										const auto nearby =
											sample(scratch[0], px / source->Width, py / source->Height);
										const float dx = float(x) + .5f - px, dy = float(y) + .5f - py,
													actual = std::sqrt(dx * dx + dy * dy);
										if (nearby[3] < 1 || actual > minimum) continue;
										result = nearby;
										minimum = actual;
										found = true;
									}
								}
							if (!store(scratch[1], x, y, result)) return false;
						}
					scratch[0].Pixels.swap(scratch[1].Pixels);
				}
			} else if (method == 1) {
				for (uint32_t axis = 0; axis < 2; axis++) {
					const uint32_t extent = axis ? source->Height : source->Width;
					for (uint32_t y = 0; y < source->Height; y++)
						for (uint32_t x = 0; x < source->Width; x++) {
							const float u = float((x + .5) / source->Width),
										v = float((y + .5) / source->Height);
							auto result = sample(scratch[0], u, v);
							if (result[3] <= 0)
								for (uint32_t distance = 1; distance < extent; distance++) {
									auto nearby = sample(
										scratch[0],
										u + (axis ? 0 : float(distance) / source->Width),
										v + (axis ? float(distance) / source->Height : 0)
									);
									if (nearby[3] > 0) {
										result = nearby;
										break;
									}
									nearby = sample(
										scratch[0],
										u - (axis ? 0 : float(distance) / source->Width),
										v - (axis ? float(distance) / source->Height : 0)
									);
									if (nearby[3] > 0) {
										result = nearby;
										break;
									}
								}
							if (!store(scratch[1], x, y, result)) return false;
						}
					scratch[0].Pixels.swap(scratch[1].Pixels);
				}
			} else {
				const float angle = direction * std::numbers::pi_v<float> / 180,
							sx = std::cos(angle) / source->Width, sy = std::sin(angle) / source->Height;
				for (uint32_t y = 0; y < source->Height; y++)
					for (uint32_t x = 0; x < source->Width; x++) {
						const float u = float((x + .5) / source->Width), v = float((y + .5) / source->Height);
						auto result = sample(*source, u, v);
						if (result[3] <= 0)
							for (uint32_t distance = 0; distance < source->Width + source->Height;
								 distance++) {
								auto nearby = sample(*source, u + sx * distance, v + sy * distance);
								if (nearby[3] > 0) {
									result = nearby;
									break;
								}
								if (both) {
									nearby = sample(*source, u - sx * distance, v - sy * distance);
									if (nearby[3] > 0) {
										result = nearby;
										break;
									}
								}
							}
						if (!store(*output, x, y, result)) return false;
					}
			}
			if (method != 2)
				for (uint32_t y = 0; y < source->Height; y++)
					for (uint32_t x = 0; x < source->Width; x++)
						if (!store(*output, x, y, ReadPixel(scratch[0], x, y))) return false;
			FinishProcessor(context, *source, *output);
			return context.FailureCode == Status::Ok;
		}
	}
	std::span<const ExecutorEntry> SourceAtlasPixelExecutors() {
		static const ExecutorEntry entries[] = {{"pc.atlas", AtlasExpand, true}};
		return entries;
	}
}
