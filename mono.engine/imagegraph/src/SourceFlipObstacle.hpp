#pragma once
#include "FluidPayload.hpp"
#include "SimulationAliases.hpp"
namespace engine::imagegraph::detail {
	inline const FluidDomainData::ObstacleControl *FindSourceFlipObstacleControl(
		const NodeContext &context,
		const FluidDomainData &input,
		std::string_view node,
		size_t row,
		uint64_t &work
	) {
		const FluidDomainData::ObstacleControl *result = nullptr;
		uint64_t revision = context.Request.SimulationAuthoringRevision;
		if (context.CurrentSimulation) {
			if (context.CurrentSimulation->Entries.size() > FluidDomainLimits::MaximumWork - work) {
				context.Fail(Status::LimitExceeded, "FLIP obstacle owner lookup exceeds bounded work");
				return nullptr;
			}
			work += context.CurrentSimulation->Entries.size();
		}
		if (context.CurrentSimulation)
			for (const auto &entry : context.CurrentSimulation->Entries)
				if (entry.Fluid.Data && entry.NodeId == input.OriginNodeId &&
					entry.ProcessorRow == input.OriginProcessorRow)
					revision = entry.State.AuthoringRevision;
		const auto consider = [&](const FluidDomainData &data) {
			if (data.ObstacleControls.size() + 1 > FluidDomainLimits::MaximumWork - work) {
				context.Fail(Status::LimitExceeded, "FLIP obstacle control lookup exceeds bounded work");
				return;
			}
			work += data.ObstacleControls.size() + 1;
			const auto found = std::lower_bound(
				data.ObstacleControls.begin(),
				data.ObstacleControls.end(),
				std::tie(node, row),
				[](const auto &item, const auto &key) {
					return std::tie(item.NodeId, item.ProcessorRow) < key;
				}
			);
			if (found != data.ObstacleControls.end() && found->NodeId == node && found->ProcessorRow == row &&
				(!result || found->Serial > result->Serial))
				result = &*found;
		};
		consider(input);
		const auto inspect = [&](const auto &entries) {
			for (const auto &entry : entries)
				if (entry.Fluid.Data && entry.State.AuthoringRevision == revision)
					consider(*entry.Fluid.Data);
		};
		if (context.CurrentSimulation) inspect(context.CurrentSimulation->Entries);
		inspect(context.PendingSimulationRows);
		return result;
	}
	inline uint64_t ResolvedSourceFlipFluidBytes(const NodeContext &context, const FluidDomainData &data) {
		uint64_t bytes = FluidDataStorageBytes<true>(data), work = 0;
		for (const auto &control : data.ObstacleControls) {
			if (data.ObstacleVisuals.size() > FluidDomainLimits::MaximumWork - work) {
				context.Fail(Status::LimitExceeded, "FLIP obstacle visual lookup exceeds bounded work");
				return UINT64_MAX;
			}
			work += data.ObstacleVisuals.size();
			const bool visual =
				std::any_of(data.ObstacleVisuals.begin(), data.ObstacleVisuals.end(), [&](const auto &item) {
					return item.NodeId == control.NodeId && item.ProcessorRow == control.ProcessorRow;
				});
			if (!visual) continue;
			const auto *latest =
				FindSourceFlipObstacleControl(context, data, control.NodeId, control.ProcessorRow, work);
			if (context.FailureCode != Status::Ok) return UINT64_MAX;
			if (latest && latest->Serial > control.Serial)
				bytes = MeshAddBytes(bytes, FluidObstacleControlBytes<true>(*latest));
		}
		return bytes;
	}
	// The caller admits retained old/new overlap with ResolvedSourceFlipFluidBytes.
	inline void ResolveSourceFlipFluidVisuals(NodeContext &context, FluidDomainData &data) {
		uint64_t work = 0;
		for (auto &control : data.ObstacleControls) {
			const bool visual =
				std::any_of(data.ObstacleVisuals.begin(), data.ObstacleVisuals.end(), [&](const auto &item) {
					return item.NodeId == control.NodeId && item.ProcessorRow == control.ProcessorRow;
				});
			if (!visual) continue;
			const auto *latest =
				FindSourceFlipObstacleControl(context, data, control.NodeId, control.ProcessorRow, work);
			if (latest && latest->Serial > control.Serial) control = *latest;
		}
	}
	inline bool ApplySourceFlipObstacle(
		NodeContext &context,
		FluidDomainData &data,
		uint32_t index,
		int64_t shape,
		Vector2 position,
		double radius,
		Vector2 size
	) {
		if (index >= data.Obstacles.size())
			return true; // Native setter returns -1; the source node ignores it.
		const auto layout = FluidDomainLayout(data.Settings);
		if (!layout) return context.Fail(Status::InvalidValue, "FLIP obstacle grid is invalid");
		auto &obstacle = data.Obstacles[index];
		const double vx = (position.X - obstacle.X) * 5 * data.Settings.TimeStep,
					 vy = (position.Y - obstacle.Y) * 5 * data.Settings.TimeStep;
		if (!std::isfinite(vx) || !std::isfinite(vy))
			return context.Fail(Status::InvalidValue, "FLIP obstacle velocity is nonfinite");
		obstacle.X = position.X;
		obstacle.Y = position.Y;
		obstacle.Shape = uint32_t(shape);
		if (shape == 0)
			obstacle.Radius = radius;
		else {
			obstacle.Width = size.X;
			obstacle.Height = size.Y;
		}
		auto &open = data.Buffers[size_t(FluidBuffer::Open)], &u = data.Buffers[size_t(FluidBuffer::U)],
			 &v = data.Buffers[size_t(FluidBuffer::V)];
		const auto write = [&](uint32_t x, uint32_t y) {
			open[size_t(x) * layout->Rows + y] = 0;
			u[size_t(x) * layout->Rows + y] = vx;
			u[size_t(x + 1) * layout->Rows + y] = vx;
			v[size_t(x) * layout->Rows + y] = vy;
			v[size_t(x) * layout->Rows + y + 1] = vy;
		};
		if (shape == 0) {
			const double squared = radius * radius;
			for (uint32_t x = 1; x + 1 < layout->Columns; ++x)
				for (uint32_t y = 1; y + 1 < layout->Rows; ++y) {
					const double dx = (x + .5) * layout->CellSize - position.X,
								 dy = (y + .5) * layout->CellSize - position.Y;
					if (dx * dx + dy * dy < squared) write(x, y);
				}
		} else {
			const double x0 = std::floor((position.X - size.X) / layout->CellSize),
						 x1 = std::floor((position.X + size.X) / layout->CellSize),
						 y0 = std::floor((position.Y - size.Y) / layout->CellSize),
						 y1 = std::floor((position.Y + size.Y) / layout->CellSize);
			for (const double value : {x0, x1, y0, y1})
				if (std::isnan(value))
					return context.Fail(
						Status::UnsupportedExecution, "FLIP rectangle obstacle source clamp is undefined"
					);
			const auto lowX = uint32_t(std::clamp(x0, 1., double(layout->Columns - 2))),
					   highX = uint32_t(std::clamp(x1, 1., double(layout->Columns - 2))),
					   lowY = uint32_t(std::clamp(y0, 1., double(layout->Rows - 2))),
					   highY = uint32_t(std::clamp(y1, 1., double(layout->Rows - 2)));
			for (uint32_t x = lowX; x < highX; ++x)
				for (uint32_t y = lowY; y < highY; ++y)
					write(x, y);
		}
		obstacle.VelocityX = vx;
		obstacle.VelocityY = vy;
		return true;
	}
}
