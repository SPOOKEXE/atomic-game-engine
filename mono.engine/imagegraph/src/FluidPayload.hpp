#pragma once
#include "MeshPayload.hpp"

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
	template <bool Retained> uint64_t FluidDataStorageBytes(const FluidDomainData &data) {
		uint64_t bytes = MeshAddBytes(
			sizeof(FluidDomainData), Retained ? data.OriginNodeId.capacity() : data.OriginNodeId.size()
		);
		bytes = MeshAddBytes(bytes, MeshVectorBytes<Retained>(data.Obstacles));
		for (const auto &buffer : data.Buffers)
			bytes = MeshAddBytes(bytes, MeshVectorBytes<Retained>(buffer));
		return bytes;
	}
	template <bool Retained> uint64_t FluidStorageBytes(const FluidDomainValue &value) {
		return value.Data ? FluidDataStorageBytes<Retained>(*value.Data) : 0;
	}
	inline bool ValidFluidDomainData(const FluidDomainData &data) {
		const auto layout = FluidDomainLayout(data.Settings);
		if (!layout || !data.Initialized || data.Tick > Limits::MaximumTick ||
			data.ParticleCount > data.Settings.MaximumParticles ||
			data.OriginNodeId.size() > Limits::MaximumTextBytes ||
			data.Obstacles.size() > Limits::MaximumArrayElements ||
			!std::isfinite(data.ParticleRestDensity) ||
			FluidDataStorageBytes<true>(data) > Limits::MaximumEvaluationBytes)
			return false;
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
		return true;
	}
	inline bool ValidFluidPayload(const FluidDomainValue &value) {
		return !value.Data || ValidFluidDomainData(*value.Data);
	}
}
