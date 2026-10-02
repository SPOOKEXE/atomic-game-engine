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
	inline void NormalizeFlipFrameTick(FluidDomainData &data, uint64_t tick) {
		if (tick == 0) {
			const auto temporary =
				std::find_if(data.History.begin(), data.History.end(), [](const auto &frame) {
					return frame.Tick == 1;
				});
			if (temporary != data.History.end()) {
				if (temporary != data.History.begin() && data.History.front().Tick == 0) {
					data.History.front().Positions = std::move(temporary->Positions);
					data.History.erase(temporary);
				} else
					temporary->Tick = 0;
			}
		}
		data.Tick = tick;
	}
	bool FlipParticleForce(NodeContext &context);
	bool FlipToVfx(NodeContext &context);
	bool FlipRender(NodeContext &context);
}
