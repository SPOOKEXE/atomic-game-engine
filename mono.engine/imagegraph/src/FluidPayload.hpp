#pragma once
#include "MeshPayload.hpp"

#include <tuple>

namespace engine::imagegraph::detail {
	struct FluidLayout {
		uint32_t Columns = 0, Rows = 0, HashColumns = 0, HashRows = 0;
		double CellSize = 0, ParticleRadius = 0, InverseCellSize = 0, InverseHashSize = 0;
		std::array<size_t, size_t(FluidBuffer::Count)> Sizes{};
	};
	inline std::optional<FluidLayout> FluidDomainLayout(const FluidDomainSettings &settings) {
		for (const auto value :
			 {settings.Width,
			  settings.Height,
			  settings.Spacing,
			  settings.Density,
			  settings.WallElasticity,
			  settings.Viscosity,
			  settings.Friction,
			  settings.Gravity,
			  settings.GravityDirection,
			  settings.FlipRatio,
			  settings.VelocityDamping,
			  settings.OverRelaxation,
			  settings.TimeStep})
			if (!std::isfinite(value)) return std::nullopt;
		if (settings.Width <= 0 || settings.Height <= 0 || settings.Width > Limits::MaximumDimension ||
			settings.Height > Limits::MaximumDimension || settings.Spacing <= 0 || settings.Density <= 0 ||
			!settings.MaximumParticles || settings.MaximumParticles > FluidDomainLimits::MaximumParticles ||
			settings.Wall > 15 || !settings.GlobalIterations ||
			settings.GlobalIterations > FluidDomainLimits::MaximumIterations ||
			settings.PressureIterations > FluidDomainLimits::MaximumIterations ||
			settings.ParticleIterations > FluidDomainLimits::MaximumIterations || settings.TimeStep < 0)
			return std::nullopt;
		const double columns = std::floor(settings.Width / settings.Spacing) + 1,
					 rows = std::floor(settings.Height / settings.Spacing) + 1;
		if (columns < 3 || rows < 3 || columns * rows > FluidDomainLimits::MaximumCells) return std::nullopt;
		FluidLayout layout;
		layout.Columns = uint32_t(columns);
		layout.Rows = uint32_t(rows);
		layout.CellSize = std::max(settings.Width / columns, settings.Height / rows);
		layout.InverseCellSize = 1 / layout.CellSize;
		layout.ParticleRadius = layout.CellSize * .3;
		layout.InverseHashSize = 1 / (2.2 * layout.ParticleRadius);
		const double hx = std::floor(settings.Width * layout.InverseHashSize) + 1,
					 hy = std::floor(settings.Height * layout.InverseHashSize) + 1;
		if (!std::isfinite(hx) || !std::isfinite(hy) || hx * hy > FluidDomainLimits::MaximumCells)
			return std::nullopt;
		layout.HashColumns = uint32_t(hx);
		layout.HashRows = uint32_t(hy);
		layout.Sizes.fill(size_t(columns * rows));
		layout.Sizes[size_t(FluidBuffer::ParticlePosition)] = settings.MaximumParticles * 2;
		layout.Sizes[size_t(FluidBuffer::ParticleVelocity)] = settings.MaximumParticles * 2;
		layout.Sizes[size_t(FluidBuffer::ParticleLife)] = settings.MaximumParticles;
		layout.Sizes[size_t(FluidBuffer::CellParticleCount)] = size_t(hx * hy);
		layout.Sizes[size_t(FluidBuffer::FirstCellParticle)] = size_t(hx * hy) + 1;
		layout.Sizes[size_t(FluidBuffer::CellParticleIds)] = settings.MaximumParticles;
		return layout;
	}
	template <bool Retained>
	uint64_t FluidObstacleControlBytes(const FluidDomainData::ObstacleControl &control) {
		return MeshAddBytes(
			sizeof(control),
			MeshAddBytes(
				Retained ? control.NodeId.capacity() : control.NodeId.size(),
				control.Texture
					? (Retained ? control.Texture->Pixels.capacity() : control.Texture->Pixels.size())
					: 0
			)
		);
	}
	template <bool Retained> uint64_t FluidDataStorageBytes(const FluidDomainData &data) {
		uint64_t bytes = MeshAddBytes(
			sizeof(FluidDomainData), Retained ? data.OriginNodeId.capacity() : data.OriginNodeId.size()
		);
		bytes = MeshAddBytes(bytes, MeshVectorBytes<Retained>(data.Obstacles));
		bytes = MeshAddBytes(bytes, MeshVectorBytes<Retained>(data.ObstacleControls));
		for (const auto &control : data.ObstacleControls)
			bytes = MeshAddBytes(bytes, FluidObstacleControlBytes<Retained>(control) - sizeof(control));
		bytes = MeshAddBytes(bytes, MeshVectorBytes<Retained>(data.ObstacleVisuals));
		for (const auto &visual : data.ObstacleVisuals)
			bytes = MeshAddBytes(bytes, Retained ? visual.NodeId.capacity() : visual.NodeId.size());
		bytes = MeshAddBytes(bytes, MeshVectorBytes<Retained>(data.Spawners));
		for (const auto &state : data.Spawners)
			bytes = MeshAddBytes(bytes, Retained ? state.NodeId.capacity() : state.NodeId.size());
		for (const auto &buffer : data.Buffers)
			bytes = MeshAddBytes(bytes, MeshVectorBytes<Retained>(buffer));
		bytes = MeshAddBytes(bytes, MeshVectorBytes<Retained>(data.ReadbackPositions));
		bytes = MeshAddBytes(bytes, MeshVectorBytes<Retained>(data.ReadbackVelocities));
		bytes = MeshAddBytes(bytes, MeshVectorBytes<Retained>(data.ReadbackLife));
		bytes = MeshAddBytes(bytes, MeshVectorBytes<Retained>(data.History));
		for (const auto &frame : data.History)
			bytes = MeshAddBytes(bytes, MeshVectorBytes<Retained>(frame.Positions));
		return bytes;
	}
	template <bool Retained> uint64_t FluidStorageBytes(const FluidDomainValue &value) {
		return value.Data ? FluidDataStorageBytes<Retained>(*value.Data) : 0;
	}
	inline uint64_t FlipStepGrowthBytes(const FluidDomainData &data) {
		return sizeof(FluidDomainData::HistoryFrame) * (data.History.size() + 1) +
			   data.ReadbackPositions.size() * sizeof(double) +
			   uint64_t(data.ParticleCount) * 5 * sizeof(double);
	}
	inline double SourceFluidParticleCount(const FluidDomainData &data) {
		return std::visit([](auto count) { return double(count); }, data.SourceParticleCount);
	}
	inline bool ValidSourceFluidParticleCount(const FluidDomainData &data) {
		const double count = SourceFluidParticleCount(data);
		return std::isfinite(count) && count >= data.ParticleCount && count == std::trunc(count) &&
			   count <= FluidDomainLimits::MaximumWork;
	}
	inline bool AddSourceFluidParticles(FluidDomainData &data, std::variant<int64_t, double> amount) {
		const double added = std::visit([](auto count) { return double(count); }, amount);
		const double previous = SourceFluidParticleCount(data);
		if (!std::isfinite(added) || added < 0 || added != std::trunc(added) ||
			added > FluidDomainLimits::MaximumWork - previous)
			return false;
		if (std::holds_alternative<int64_t>(data.SourceParticleCount) &&
			std::holds_alternative<int64_t>(amount))
			data.SourceParticleCount =
				std::get<int64_t>(data.SourceParticleCount) + std::get<int64_t>(amount);
		else
			data.SourceParticleCount = previous + added;
		return true;
	}
	inline bool ValidFluidDomainData(const FluidDomainData &data) {
		const auto layout = FluidDomainLayout(data.Settings);
		if (!layout || !data.Initialized || data.Tick > Limits::MaximumTick ||
			data.ParticleCount > data.Settings.MaximumParticles || !ValidSourceFluidParticleCount(data) ||
			data.Spawners.size() > Limits::MaximumNodes ||
			data.ObstacleControls.size() > Limits::MaximumArrayElements ||
			data.ObstacleVisuals.size() > Limits::MaximumArrayElements ||
			data.OriginNodeId.size() > Limits::MaximumTextBytes ||
			data.Obstacles.size() > Limits::MaximumArrayElements ||
			!std::isfinite(data.ParticleRestDensity) ||
			FluidDataStorageBytes<true>(data) > Limits::MaximumEvaluationBytes)
			return false;
		for (size_t index = 0; index < data.ObstacleControls.size(); ++index) {
			const auto &control = data.ObstacleControls[index];
			if (control.NodeId.empty() || control.NodeId.size() > Limits::MaximumTextBytes ||
				control.ProcessorRow > Limits::MaximumArrayElements ||
				control.Index >= Limits::MaximumArrayElements || !control.Serial ||
				control.Tick > Limits::MaximumTick || !std::isfinite(control.X) ||
				!std::isfinite(control.Y) ||
				(index &&
				 std::tie(
					 data.ObstacleControls[index - 1].NodeId, data.ObstacleControls[index - 1].ProcessorRow
				 ) >= std::tie(control.NodeId, control.ProcessorRow)) ||
				(control.Texture &&
				 (!ValidSurfaceLayout(
					  *control.Texture, Limits::MaximumDimension, Limits::MaximumEvaluationBytes
				  ) ||
				  !FiniteSurfaceSamples(*control.Texture))))
				return false;
		}
		for (const auto &visual : data.ObstacleVisuals) {
			if (visual.NodeId.empty() || visual.NodeId.size() > Limits::MaximumTextBytes ||
				visual.ProcessorRow > Limits::MaximumArrayElements)
				return false;
			const auto control = std::lower_bound(
				data.ObstacleControls.begin(),
				data.ObstacleControls.end(),
				std::tie(visual.NodeId, visual.ProcessorRow),
				[](const auto &item, const auto &key) {
					return std::tie(item.NodeId, item.ProcessorRow) < key;
				}
			);
			if (control == data.ObstacleControls.end() || control->NodeId != visual.NodeId ||
				control->ProcessorRow != visual.ProcessorRow)
				return false;
		}
		for (size_t index = 0; index < data.Spawners.size(); ++index) {
			const auto &state = data.Spawners[index];
			if (state.NodeId.empty() || state.NodeId.size() > Limits::MaximumTextBytes ||
				state.Tick > Limits::MaximumTick || !std::isfinite(state.Accumulator) ||
				std::abs(state.Accumulator) > FluidDomainLimits::MaximumWork ||
				!std::isfinite(state.PreviousPosition[0]) || !std::isfinite(state.PreviousPosition[1]) ||
				(index && std::tie(data.Spawners[index - 1].NodeId, data.Spawners[index - 1].ProcessorRow) >=
							  std::tie(state.NodeId, state.ProcessorRow)))
				return false;
		}
		for (size_t index = 0; index < data.Buffers.size(); ++index) {
			if (data.Buffers[index].size() != layout->Sizes[index]) return false;
			for (const auto value : data.Buffers[index])
				if (!std::isfinite(value)) return false;
		}
		for (const auto &obstacle : data.Obstacles) {
			if (obstacle.Shape > 1) return false;
			for (const auto value :
				 {obstacle.X,
				  obstacle.Y,
				  obstacle.VelocityX,
				  obstacle.VelocityY,
				  obstacle.Radius,
				  obstacle.Width,
				  obstacle.Height})
				if (!std::isfinite(value)) return false;
		}
		if (data.ReadbackPositions.size() != data.ReadbackVelocities.size() ||
			data.ReadbackPositions.size() != data.ReadbackLife.size() * 2 ||
			data.ReadbackLife.size() > data.Settings.MaximumParticles ||
			data.History.size() > Limits::MaximumRangeFrames)
			return false;
		for (const auto *buffer : {&data.ReadbackPositions, &data.ReadbackVelocities, &data.ReadbackLife})
			for (const double value : *buffer)
				if (!std::isfinite(value)) return false;
		for (size_t index = 0; index < data.History.size(); ++index) {
			const auto &frame = data.History[index];
			if (frame.Tick > Limits::MaximumTick || frame.Positions.size() % 2 ||
				frame.Positions.size() > size_t(data.Settings.MaximumParticles) * 2 ||
				(index && data.History[index - 1].Tick >= frame.Tick))
				return false;
			for (const double value : frame.Positions)
				if (!std::isfinite(value)) return false;
		}
		return true;
	}
	inline bool ValidFluidPayload(const FluidDomainValue &value) {
		return !value.Data || ValidFluidDomainData(*value.Data);
	}
}
