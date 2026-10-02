#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace engine::imagegraph {
	struct FluidDomainLimits {
		static constexpr uint32_t MaximumParticles = 65536, MaximumCells = 262144, MaximumIterations = 256;
		static constexpr uint64_t MaximumWork = 16777216;
	};
	struct FluidDomainSettings {
		double Width = 32, Height = 32, Spacing = 4, Density = 10;
		uint32_t MaximumParticles = 4096;
		uint32_t Wall = 15, GlobalIterations = 8, PressureIterations = 2, ParticleIterations = 2;
		double WallElasticity = 0, Viscosity = 0, Friction = 0, Gravity = 5, GravityDirection = -90,
			   FlipRatio = .8, VelocityDamping = .8, OverRelaxation = 1.5, TimeStep = .05;
		bool SkipIncompressible = false;
		bool operator==(const FluidDomainSettings &) const = default;
	};
	struct FluidObstacle {
		double X = 0, Y = 0, VelocityX = 0, VelocityY = 0;
		uint32_t Shape = 0;
		double Radius = 0, Width = 0, Height = 0;
		bool operator==(const FluidObstacle &) const = default;
	};
	// These indices describe the source solver's in-memory arrays, never a durable file identity.
	enum class FluidBuffer : size_t {
		U,
		V,
		DU,
		DV,
		PreviousU,
		PreviousV,
		Pressure,
		Open,
		Solid,
		CellType,
		ParticlePosition,
		ParticleVelocity,
		ParticleDensity,
		ParticleLife,
		CellParticleCount,
		FirstCellParticle,
		CellParticleIds,
		Count
	};
	struct FluidDomainData {
		FluidDomainSettings Settings;
		std::array<std::vector<double>, size_t(FluidBuffer::Count)> Buffers;
		std::vector<FluidObstacle> Obstacles;
		uint32_t ParticleCount = 0;
		double ParticleRestDensity = 0;
		std::string OriginNodeId;
		size_t OriginProcessorRow = 0;
		uint64_t Tick = 0, AuthoringRevision = 0;
		bool Initialized = false;
		bool operator==(const FluidDomainData &) const = default;
	};
}
