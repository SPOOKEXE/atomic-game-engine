#include "../ParticlePayload.hpp"
#include "FlipNodes.hpp"
namespace engine::imagegraph::detail {
	bool FlipToVfx(NodeContext &context) {
		const auto *input = FindFlipDomainInput(context);
		if (!input || !input->Data) return true;
		if (!ValidFluidPayload(*input))
			return context.Fail(Status::InvalidValue, "FLIP-to-VFX domain is invalid", "domain");
		const double amount = context.Scalar("attribute_part_amount", 512);
		const auto limit = std::ceil(amount);
		if (!std::isfinite(amount) || limit < 0 || limit > Limits::MaximumArrayElements)
			return context.Fail(
				Status::InvalidValue,
				"FLIP-to-VFX pool amount exceeds the native bound",
				"attribute_part_amount"
			);
		const auto &domain = *input->Data;
		const size_t count =
			std::min(size_t(domain.Settings.MaximumParticles - 1), size_t(SourceFluidParticleCount(domain)));
		if (limit == 0)
			for (size_t index = 0; index < count; ++index) {
				if (index < domain.ReadbackLife.size() && (domain.ReadbackPositions[index * 2] != 0 ||
														   domain.ReadbackPositions[index * 2 + 1] != 0))
					return context.Fail(
						Status::UnsupportedExecution,
						"source FLIP-to-VFX reads an undefined zero-capacity particle pool",
						"attribute_part_amount"
					);
			}
		size_t populated = 0;
		for (size_t index = 0; index < count && populated < size_t(limit); ++index) {
			if (index >= domain.ReadbackLife.size()) continue;
			if (domain.ReadbackPositions[index * 2] != 0 || domain.ReadbackPositions[index * 2 + 1] != 0)
				++populated;
		}
		if (!context.ReserveOutput(
				populated * (sizeof(ElementValue) + sizeof(ParticleData2D) +
							 std::max(context.Authored.Id.size(), std::string{}.capacity())),
				"particles"
			))
			return false;
		ArrayValue output;
		output.ElementType = ValueType::Particle;
		output.Elements.reserve(populated);
		for (size_t index = 0; index < count && output.Elements.size() < populated; ++index) {
			if (index >= domain.ReadbackLife.size()) continue;
			const double x = domain.ReadbackPositions[index * 2], y = domain.ReadbackPositions[index * 2 + 1];
			if (x == 0 && y == 0) continue;
			ParticleValue value;
			auto &data = value.Data.emplace();
			data.OriginNodeId = context.Authored.Id;
			data.OriginProcessorRow = context.ProcessorRow;
			data.State.Active = true;
			data.State.Position = {x, y};
			data.State.SourceSlot = output.Elements.size();
			output.Elements.emplace_back(std::move(value));
		}
		context.SetValue("particles", std::move(output));
		return context.FailureCode == Status::Ok;
	}
} // namespace engine::imagegraph::detail
