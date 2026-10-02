#pragma once
#include "../FluidPayload.hpp"
#include "../SimulationAliases.hpp"
namespace engine::imagegraph::detail {
	inline const FluidDomainValue *FindFlipDomainInput(NodeContext &context) {
		const Value *input = context.Find("domain");
		return input ? std::get_if<FluidDomainValue>(input) : nullptr;
	}
	inline bool PublishFlipDomain(NodeContext &context, FluidDomainValue value) {
		if (!PublishSimulationFluidUpdate(context, value)) return false;
		context.SetValue("domain", std::move(value));
		return context.FailureCode == Status::Ok;
	}
	inline Vector2 FlipPosition(NodeContext &context, const FluidDomainData &domain, Vector2 fallback = {}) {
		Vector2 value = context.Vec2("position", fallback);
		if (!context.IsLinked("position") && context.Integer("position_unit", 1) == 1) {
			value.X *= domain.Settings.Width;
			value.Y *= domain.Settings.Height;
		}
		return value;
	}
	bool FlipParticleForce(NodeContext &context);
}
