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
	inline const FluidDomainValue *
	CapturedFlipFrame(NodeContext &context, std::string_view origin, size_t row) {
		const SimulationReplayEntry *captured = nullptr;
		if (context.Request.SimulationReplay)
			for (const auto &entry : context.Request.SimulationReplay->Entries)
				if (entry.NodeId == origin && entry.ProcessorRow == row && entry.Fluid.Data) {
					if (captured) {
						context.Fail(Status::DuplicateId, "FLIP refresh repeats captured origin", "domain");
						return nullptr;
					}
					captured = &entry;
				}
		if (!captured || !captured->State.Initialized || captured->State.Tick != context.Request.Tick ||
			captured->State.AuthoringRevision != context.Request.SimulationAuthoringRevision ||
			!ValidFluidPayload(captured->Fluid) || captured->Fluid.Data->Tick != context.Request.Tick ||
			captured->Fluid.Data->AuthoringRevision != context.Request.SimulationAuthoringRevision ||
			captured->Fluid.Data->OriginNodeId != origin || captured->Fluid.Data->OriginProcessorRow != row) {
			context.Fail(Status::InvalidValue, "FLIP refresh requires a matching captured frame", "domain");
			return nullptr;
		}
		return &captured->Fluid;
	}
	inline bool ReuseFlipDomain(NodeContext &context, const FluidDomainValue &input) {
		const auto *captured =
			CapturedFlipFrame(context, input.Data->OriginNodeId, input.Data->OriginProcessorRow);
		if (!captured || !context.ReserveOutput(FluidStorageBytes<true>(*captured), "domain")) return false;
		context.SetValue("domain", *captured);
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
	bool FlipObstacle(NodeContext &context);
	bool FlipSolid(NodeContext &context);
	bool FlipFillSurface(NodeContext &context, const FluidDomainValue &input);
	bool FlipSpawner(NodeContext &context);
	bool FlipDestroy(NodeContext &context);
	bool FlipParticleForce(NodeContext &context);
	bool FlipToVfx(NodeContext &context);
	bool FlipRender(NodeContext &context);
}
