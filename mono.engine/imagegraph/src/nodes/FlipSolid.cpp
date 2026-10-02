#include "FlipNodes.hpp"
#include "SourceFlipMask.hpp"
namespace engine::imagegraph::detail {
	bool FlipSolid(NodeContext &context) {
		const auto *input = FindFlipDomainInput(context);
		if (!input || !input->Data) {
			context.SetValue("domain", UndefinedValue{});
			return true;
		}
		if (!ValidFluidPayload(*input))
			return context.Fail(Status::InvalidValue, "FLIP solid domain is invalid", "domain");
		if (context.Request.ReuseSimulationFrame) return ReuseFlipDomain(context, *input);
		if (!context.ReserveOutput(FluidStorageBytes<true>(*input), "domain")) return false;
		const Image *surface = context.Input("collider");
		if (!surface) return PublishFlipDomain(context, *input);
		const auto layout = FluidDomainLayout(input->Data->Settings);
		if (!layout) return context.Fail(Status::InvalidValue, "FLIP solid grid is invalid", "domain");
		auto temporary = context.ReserveWorkspace(uint64_t(layout->Columns) * layout->Rows * 2, "collider");
		if (!temporary) return false;
		Image mask;
		if (!BuildSourceFlipMask(
				context,
				*surface,
				layout->Columns,
				layout->Rows,
				context.Scalar("threshold", .1),
				context.Integer("expands", 0),
				mask
			))
			return false;
		FluidDomainValue output = *input;
		ApplySourceFlipSolidMask(*output.Data, mask, layout->Columns, layout->Rows);
		return PublishFlipDomain(context, std::move(output));
	}
} // namespace engine::imagegraph::detail
