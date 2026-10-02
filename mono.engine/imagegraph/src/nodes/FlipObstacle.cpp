#include "FlipNodes.hpp"
#include "SourceFlipObstacle.hpp"
namespace engine::imagegraph::detail {
	bool FlipObstacle(NodeContext &context) {
		const auto *input = FindFlipDomainInput(context);
		if (!input || !input->Data) {
			context.SetValue("domain", UndefinedValue{});
			return true;
		}
		if (!ValidFluidPayload(*input))
			return context.Fail(Status::InvalidValue, "FLIP obstacle domain is invalid", "domain");
		if (context.Request.ReuseSimulationFrame) return ReuseFlipDomain(context, *input);
		const int64_t shape = context.Integer("shape", 0);
		const auto position = FlipPosition(context, *input->Data), size = context.Vec2("size", {4, 4});
		const double radius = context.Scalar("radius", 4);
		if (shape < 0 || shape > 1 || !std::isfinite(position.X) || !std::isfinite(position.Y) ||
			!std::isfinite(size.X) || !std::isfinite(size.Y) || !std::isfinite(radius) ||
			!std::isfinite(radius * radius))
			return context.Fail(Status::InvalidValue, "FLIP obstacle controls are invalid");
		uint64_t work = 0;
		const auto *prior = FindSourceFlipObstacleControl(
			context, *input->Data, context.Authored.Id, context.ProcessorRow, work
		);
		if (context.FailureCode != Status::Ok) return false;
		const bool create = !prior || (context.Request.Tick == 0 && context.Request.Subframe == 0 &&
									   !context.Request.NegativeFrame);
		const auto local = std::lower_bound(
			input->Data->ObstacleControls.begin(),
			input->Data->ObstacleControls.end(),
			std::tie(context.Authored.Id, context.ProcessorRow),
			[](const auto &item, const auto &key) { return std::tie(item.NodeId, item.ProcessorRow) < key; }
		);
		const bool newControl = local == input->Data->ObstacleControls.end() ||
								local->NodeId != context.Authored.Id ||
								local->ProcessorRow != context.ProcessorRow;
		if ((create && (input->Data->Obstacles.size() >= Limits::MaximumArrayElements ||
						input->Data->ObstacleVisuals.size() >= Limits::MaximumArrayElements)) ||
			(newControl && input->Data->ObstacleControls.size() >= Limits::MaximumArrayElements) ||
			(prior && prior->Serial == UINT64_MAX))
			return context.Fail(Status::LimitExceeded, "FLIP obstacle lifecycle exceeds bounded slots");
		const Image *texture = context.Input("texture");
		if (texture &&
			(!ValidSurfaceLayout(*texture, Limits::MaximumDimension, Limits::MaximumEvaluationBytes) ||
			 !FiniteSurfaceSamples(*texture)))
			return context.Fail(Status::InvalidValue, "FLIP obstacle texture is invalid", "texture");
		uint64_t bytes = FluidStorageBytes<true>(*input);
		const uint64_t name = std::max(context.Authored.Id.size(), std::string{}.capacity());
		if (newControl) bytes = MeshAddBytes(bytes, sizeof(FluidDomainData::ObstacleControl) + name);
		if (create)
			bytes =
				MeshAddBytes(bytes, sizeof(FluidObstacle) + sizeof(FluidDomainData::ObstacleVisual) + name);
		if (texture) bytes = MeshAddBytes(bytes, texture->Pixels.size());
		if (!context.ReserveOutput(bytes, "domain")) return false;
		// Copied vectors initially retain their size, so explicit growth overlaps the old slots.
		const uint64_t growth =
			(newControl ? input->Data->ObstacleControls.size() * sizeof(FluidDomainData::ObstacleControl)
						: name) +
			(create ? input->Data->Obstacles.size() * sizeof(FluidObstacle) +
						  input->Data->ObstacleVisuals.size() * sizeof(FluidDomainData::ObstacleVisual)
					: 0);
		auto overlap = context.ReserveWorkspace(growth, "domain");
		if (!overlap) return false;
		FluidDomainValue output = *input;
		auto &data = *output.Data;
		data.ObstacleControls.reserve(data.ObstacleControls.size() + size_t(newControl));
		data.Obstacles.reserve(data.Obstacles.size() + size_t(create));
		data.ObstacleVisuals.reserve(data.ObstacleVisuals.size() + size_t(create));
		const uint32_t nativeIndex = create ? uint32_t(data.Obstacles.size()) : prior->Index;
		if (create) {
			data.Obstacles.push_back({});
			data.ObstacleVisuals.push_back({context.Authored.Id, context.ProcessorRow});
		}
		FluidDomainData::ObstacleControl state;
		state.NodeId = context.Authored.Id;
		state.ProcessorRow = context.ProcessorRow;
		state.Index = nativeIndex;
		state.Serial = prior ? prior->Serial + 1 : 1;
		state.Tick = context.Request.Tick;
		state.X = position.X;
		state.Y = position.Y;
		if (texture) state.Texture = *texture;
		auto found = std::lower_bound(
			data.ObstacleControls.begin(),
			data.ObstacleControls.end(),
			std::tie(state.NodeId, state.ProcessorRow),
			[](const auto &item, const auto &key) { return std::tie(item.NodeId, item.ProcessorRow) < key; }
		);
		if (newControl)
			data.ObstacleControls.insert(found, std::move(state));
		else
			*found = std::move(state);
		if (!ApplySourceFlipObstacle(context, data, nativeIndex, shape, position, radius, size)) return false;
		return PublishFlipDomain(context, std::move(output));
	}
}
