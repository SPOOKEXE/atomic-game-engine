#include "SourceJpeg.hpp"

#include "../AtlasPayload.hpp"
#include "Families.hpp"
#include "Sampler.hpp"
#include "Source2DGenerator.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>
#include <string_view>
#include <vector>

namespace engine::imagegraph::detail {
	namespace {
		constexpr uint64_t JPEG_WORK_LIMIT = 64000000;
		// grug preserve the pinned shader's approximate PI, separate from host phase conversion.
		constexpr float JPEG_PI = 3.1415972f, JPEG_ZERO_WEIGHT = .70710678118f;
		struct JpegInputs {
			const Image *Source = nullptr, *Mask = nullptr;
			int32_t Patch = 8, Reconstruction = 8;
			int64_t Transformation = 0;
			float Compression = 32, Phase = 0;
			double Feather = 0;
			bool Inactive = false, SkipDct = false;
		};
		bool JpegFloat(NodeContext &context, std::string_view port, double number, float &result) {
			result = float(number);
			return std::isfinite(result) ||
				   context.Fail(Status::InvalidValue, "JPEG uniform exceeds finite shader range", port);
		}
		bool JpegCounter(NodeContext &context, std::string_view port, int32_t minimum, int32_t &result) {
			const double number = context.Scalar(port, 8);
			if (!std::isfinite(number))
				return context.Fail(Status::InvalidValue, "JPEG counter is nonfinite", port);
			// grug Int getters apply their minimum validator before host rounding, including linked values.
			const double rounded = source2d::GeneratorRoundHalfEven(std::max(double(minimum), number));
			if (rounded > std::numeric_limits<int32_t>::max())
				return context.Fail(
					Status::LimitExceeded, "JPEG counter exceeds signed shader integer range", port
				);
			result = int32_t(rounded);
			return context.FailureCode == Status::Ok;
		}
		bool JpegSurface(NodeContext &context, std::string_view port, const Image *image, bool raw) {
			if (!image) return true;
			if (!ValidSurfaceLayout(*image, Limits::MaximumDimension, Limits::MaximumEvaluationBytes))
				return context.Fail(Status::InvalidValue, "JPEG surface layout is invalid", port);
			if (const auto *value = context.Find(port))
				if (const auto *atlas = std::get_if<AtlasValue>(value)) {
					if (!ValidAtlasPayload(*atlas))
						return context.Fail(Status::InvalidValue, "JPEG Atlas is malformed", port);
					if (raw)
						return context.Fail(
							Status::UnsupportedExecution, "JPEG raw mask binding rejects Atlas", port
						);
				}
			return true;
		}
		bool PrepareJpeg(NodeContext &context, JpegInputs &inputs) {
			inputs.Source = context.Input("surface_in");
			if (!inputs.Source)
				return context.Fail(Status::InvalidValue, "JPEG requires Surface In", "surface_in");
			if (!JpegSurface(context, "surface_in", inputs.Source, false)) return false;
			inputs.Inactive = !context.Boolean("active", true);
			if (inputs.Inactive) return context.FailureCode == Status::Ok;
			if (!ResolveProcessorSurfaceFormat(context, inputs.Source)) return false;
			inputs.SkipDct = context.Boolean("deconstruct_only");
			inputs.Transformation = context.Integer("transformation");
			if (inputs.Transformation < 0 || inputs.Transformation > 2)
				return context.Fail(
					Status::UnsupportedExecution,
					inputs.Transformation == 3 ? "JPEG Step basis reads uninitialized source shader values"
											   : "JPEG transformation is undefined",
					"transformation"
				);
			if (!JpegCounter(context, "patch_size", 1, inputs.Patch)) return false;
			if (context.Boolean("reconstruct_all"))
				inputs.Reconstruction = inputs.Patch;
			else if (!JpegCounter(context, "reconstruction", 0, inputs.Reconstruction))
				return false;
			if (!JpegFloat(context, "phase", context.Scalar("phase") * std::numbers::pi / 180., inputs.Phase))
				return false;
			if (!inputs.SkipDct &&
				!JpegFloat(context, "compression", context.Scalar("compression", 32), inputs.Compression))
				return false;
			inputs.Mask = context.Input("mask");
			if (!JpegSurface(context, "mask", inputs.Mask, true)) return false;
			inputs.Feather = context.Scalar("mask_feather");
			if (!std::isfinite(inputs.Feather))
				return context.Fail(Status::InvalidValue, "JPEG mask feather is nonfinite", "mask_feather");
			return context.FailureCode == Status::Ok;
		}
		bool QuoteJpeg(NodeContext &context, const JpegInputs &inputs, uint64_t &work) {
			const uint64_t pixels = uint64_t(inputs.Source->Width) * inputs.Source->Height;
			long double cost = pixels;
			if (!inputs.Inactive) {
				const long double forward =
					inputs.SkipDct ? 0 : static_cast<long double>(inputs.Patch) * inputs.Patch;
				const long double inverse =
					static_cast<long double>(inputs.Reconstruction) * inputs.Reconstruction;
				cost += pixels * (128.L + 32.L * (forward + inverse));
				if (inputs.Mask && inputs.Feather > 0) {
					const double radius = std::max(1., std::round(inputs.Feather));
					if (radius > std::numeric_limits<int>::max())
						return context.Fail(
							Status::LimitExceeded,
							"JPEG mask feather exceeds supported radius",
							"mask_feather"
						);
					cost += static_cast<long double>(inputs.Mask->Width) * inputs.Mask->Height *
								(2 * (2 * radius - 1) * 16 + 2) +
							radius * 16;
				}
			}
			if (!std::isfinite(cost) || work > JPEG_WORK_LIMIT || cost > JPEG_WORK_LIMIT - work)
				return context.Fail(
					Status::LimitExceeded, "JPEG complete batch exceeds work limit", "surface_out"
				);
			work += uint64_t(std::ceil(cost));
			return true;
		}
		float JpegFract(float number) {
			return number - std::floor(number);
		}
		float JpegAxis(const JpegInputs &inputs, float frequency, float position) {
			if (inputs.Transformation == 0) return std::cos(JPEG_PI * frequency * position + inputs.Phase);
			const float progress = JpegFract(frequency * position + inputs.Phase / JPEG_PI / 2.f + .5f);
			float triangle = progress < .5f ? progress * 2.f : (1.f - progress) * 2.f;
			if (inputs.Transformation == 2) triangle = triangle * triangle * (3.f - 2.f * triangle);
			return triangle * 2.f - 1.f;
		}
		float JpegRound(float number) {
			return JpegFract(number) > .5f ? std::ceil(number) : std::floor(number);
		}
		bool JpegPass(
			NodeContext &context,
			const JpegInputs &inputs,
			const Image &source,
			Image &target,
			bool reconstruct
		) {
			const float patch = float(inputs.Patch);
			const int32_t count = reconstruct ? inputs.Reconstruction : inputs.Patch;
			for (uint32_t y = 0; y < target.Height; ++y)
				for (uint32_t x = 0; x < target.Width; ++x) {
					const float pixelX = ((float(x) + .5f) / target.Width) * target.Width;
					const float pixelY = ((float(y) + .5f) / target.Height) * target.Height;
					const float frequencyX = pixelX - patch * std::floor(pixelX / patch) - .5f;
					const float frequencyY = pixelY - patch * std::floor(pixelY / patch) - .5f;
					const float blockX = std::floor(pixelX - frequencyX),
								blockY = std::floor(pixelY - frequencyY);
					std::array<float, 4> accumulated{};
					for (int32_t first = 0; first < count; ++first)
						for (int32_t second = 0; second < count; ++second) {
							const float sampleU = (blockX + float(first) + .5f) / source.Width;
							const float sampleV = (blockY + float(second) + .5f) / source.Height;
							const auto sample = SampleNearest(source, sampleU, sampleV);
							float coefficient =
								reconstruct ? JpegAxis(inputs, float(first), (frequencyX + .5f) / patch) *
												  JpegAxis(inputs, float(second), (frequencyY + .5f) / patch)
											: JpegAxis(inputs, frequencyX, (float(first) + .5f) / patch) *
												  JpegAxis(inputs, frequencyY, (float(second) + .5f) / patch);
							coefficient *=
								(reconstruct ? first == 0 : frequencyX < .5f) ? JPEG_ZERO_WEIGHT : 1.f;
							coefficient *=
								(reconstruct ? second == 0 : frequencyY < .5f) ? JPEG_ZERO_WEIGHT : 1.f;
							for (size_t channel = 0; channel < 4; ++channel)
								accumulated[channel] += float(sample[channel]) * coefficient;
						}
					Rgba result{};
					for (size_t channel = 0; channel < 4; ++channel) {
						float value = accumulated[channel] / patch * 2.f;
						if (!reconstruct && inputs.Compression != 0)
							value =
								JpegRound(value / patch * inputs.Compression) / inputs.Compression * patch;
						result[channel] = channel == 3 ? 1.f : value;
					}
					if (!WritePixel(target, x, y, result))
						return context.Fail(
							Status::InvalidValue,
							"JPEG pass exceeds finite half-float storage range",
							reconstruct ? "reconstruction" : "compression"
						);
				}
			return true;
		}
		bool DrawJpeg(NodeContext &context) {
			ENGINE_PROFILE("imagegraph.source.jpeg");
			JpegInputs inputs;
			uint64_t work = 0;
			if (!PrepareJpeg(context, inputs) || !QuoteJpeg(context, inputs, work)) return false;
			bool failed = false;
			if (CopyWhenInactive(context, failed)) return !failed;
			const auto format = ResolveProcessorSurfaceFormat(context, inputs.Source);
			if (!format) return false;
			const auto width = inputs.Source->Width, height = inputs.Source->Height;
			const uint64_t bytes = uint64_t(width) * height * 8;
			auto scratchCharge = context.ReserveWorkspace(bytes * (inputs.SkipDct ? 1 : 2), "surface_out");
			if (!scratchCharge) return false;
			Image coefficients,
				reconstructed{
					width, height, std::vector<uint8_t>(size_t(bytes)), 0, SurfaceFormat::RGBA16Float
				};
			const Image *reconstructionSource = inputs.Source;
			if (!inputs.SkipDct) {
				coefficients = {
					width, height, std::vector<uint8_t>(size_t(bytes)), 0, SurfaceFormat::RGBA16Float
				};
				if (!JpegPass(context, inputs, *inputs.Source, coefficients, false)) return false;
				reconstructionSource = &coefficients;
			}
			if (!JpegPass(context, inputs, *reconstructionSource, reconstructed, true)) return false;
			auto *output = context.NewImage("surface_out", width, height, *format);
			if (!output) return false;
			for (uint32_t y = 0; y < height; ++y)
				for (uint32_t x = 0; x < width; ++x)
					if (!WritePixel(*output, x, y, ReadPixel(reconstructed, x, y)))
						return context.Fail(
							Status::InvalidValue, "JPEG output exceeds numeric storage range", "surface_out"
						);
			FinishProcessor(context, *inputs.Source, *output);
			return context.FailureCode == Status::Ok;
		}
	}
	bool AdmitSourceJpeg(NodeContext &context, uint64_t &batchWork) {
		JpegInputs inputs;
		return PrepareJpeg(context, inputs) && QuoteJpeg(context, inputs, batchWork);
	}
	std::span<const ExecutorEntry> SourceJpegExecutors() {
		static constexpr ExecutorEntry entries[]{{"pc.jpeg", DrawJpeg, true}};
		return entries;
	}
}
