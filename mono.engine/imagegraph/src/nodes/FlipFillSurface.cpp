#include "FlipNodes.hpp"
#include "Sampler.hpp"

#include <engine/core/Float16.hpp>
#include <engine/imagegraph/FlipReplay.hpp>
namespace engine::imagegraph::detail {
	bool FlipFillSurface(NodeContext &context, const FluidDomainValue &input) {
		if (!context.ReserveOutput(FluidStorageBytes<true>(input), "domain")) return false;
		const Image *surface = context.Input("spawn_surface");
		if (!surface) return PublishFlipDomain(context, input);
		if (context.Request.RequireSourceGpuRasterCoverage)
			return context.Fail(
				Status::UnsupportedExecution,
				"FLIP surface-fill float32 grayscale/R16 readback profile requires "
				"captured GPU filtering and conversion",
				"spawn_surface"
			);
		const double requestedDensity = context.Scalar("density", .5);
		if (!std::isfinite(requestedDensity))
			return context.Fail(Status::InvalidValue, "FLIP surface fill density is nonfinite", "density");
		const double density = std::max(.001, requestedDensity / input.Data->Settings.Spacing);
		const uint64_t stride = uint64_t(std::max(1., std::ceil(1 / density))),
					   pixels = uint64_t(surface->Width) * surface->Height;
		if (!surface->Width || !surface->Height ||
			(pixels + stride - 1) / stride > FluidDomainLimits::MaximumWork)
			return context.Fail(
				Status::LimitExceeded,
				"FLIP surface fill exceeds bounded source buffer traversal",
				"spawn_surface"
			);
		const size_t capacity = input.Data->Settings.MaximumParticles - input.Data->ParticleCount;
		auto scratch = context.ReserveWorkspace(
			FluidStorageBytes<true>(input) + capacity * sizeof(FluidSpawnParticle), "spawn_surface"
		);
		if (!scratch) return false;
		std::vector<FluidSpawnParticle> particles;
		particles.reserve(capacity);
		uint64_t count = 0;
		const bool filtered = Filtered(ReadSampler(context));
		for (uint64_t index = 0; index < pixels; index += stride) {
			const uint32_t x = uint32_t(index % surface->Width), y = uint32_t(index / surface->Width);
			const auto color = Texture(
				*surface, (double(x) + .5) / surface->Width, (double(y) + .5) / surface->Height, filtered
			);
			// Source sh_greyscale multiplies luma by alpha once; BLEND_ALPHA uses one
			// as its RGB source factor before the R16Float buffer readback.
			const float grey = float(
								   float(float(float(color[0]) * .2126f) + float(float(color[1]) * .7152f)) +
								   float(float(color[2]) * .0722f)
							   ) *
							   float(color[3]);
			const float sampled = core::DecodeFloat16(core::EncodeFloat16(grey));
			if (std::isnan(sampled))
				return context.Fail(
					Status::InvalidValue,
					"FLIP surface-fill grayscale shader result is undefined",
					"spawn_surface"
				);
			if (sampled <= .5f) continue;
			++count;
			if (particles.size() < capacity)
				particles.push_back(
					{{std::clamp(double(x) + input.Data->Settings.Spacing, 0., input.Data->Settings.Width),
					  std::clamp(double(y) + input.Data->Settings.Spacing, 0., input.Data->Settings.Height)},
					 {0, 0}}
				);
		}
		FluidDomainValue output;
		Diagnostic diagnostic;
		const auto status =
			SpawnFlipReplay(input, particles, Limits::MaximumEvaluationBytes, output, diagnostic);
		if (status != Status::Ok) return context.Fail(status, diagnostic.Message, "domain");
		output.Data->SourceParticleCount = input.Data->SourceParticleCount;
		if (!AddSourceFluidParticles(*output.Data, int64_t(count)))
			return context.Fail(
				Status::LimitExceeded, "FLIP surface-fill source count exceeds bounded work", "spawn_surface"
			);
		return PublishFlipDomain(context, std::move(output));
	}
} // namespace engine::imagegraph::detail
