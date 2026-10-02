#pragma once
#include <engine/imagegraph/Surface.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
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
		struct HistoryFrame {
			uint64_t Tick = 0;
			std::vector<double> Positions;
			bool operator==(const HistoryFrame &) const = default;
		};
		struct SpawnerState {
			std::string NodeId;
			size_t ProcessorRow = 0;
			double Accumulator = 0;
			std::array<double, 2> PreviousPosition{};
			uint64_t Tick = 0;
			bool operator==(const SpawnerState &) const = default;
		};
		// Source node objects retain an index separately from each domain's native obstacles.
		struct ObstacleControl {
			std::string NodeId;
			size_t ProcessorRow = 0;
			uint32_t Index = 0;
			uint64_t Serial = 0, Tick = 0;
			double X = 0, Y = 0;
			std::optional<Image> Texture;
			bool operator==(const ObstacleControl &) const = default;
		};
		struct ObstacleVisual {
			std::string NodeId;
			size_t ProcessorRow = 0;
			bool operator==(const ObstacleVisual &) const = default;
		};
		FluidDomainSettings Settings;
		std::array<std::vector<double>, size_t(FluidBuffer::Count)> Buffers;
		std::vector<FluidObstacle> Obstacles;
		std::vector<ObstacleControl> ObstacleControls;
		// Ordered source obstracles entries reference a mutable source node's visual object.
		std::vector<ObstacleVisual> ObstacleVisuals;
		uint32_t ParticleCount = 0;
		// The source object increments before the native solver capacity-clamps a spawn.
		std::variant<int64_t, double> SourceParticleCount{int64_t{0}};
		// Sorted by durable producer ID and processor row; controls retain their last update tick.
		std::vector<SpawnerState> Spawners;

		// Object readback remains unchanged by native spawn/force calls until a source Step.
		std::vector<double> ReadbackPositions, ReadbackVelocities, ReadbackLife;
		// Missing slots in these sparse rows are the source's zero-initialized array entries.
		std::vector<HistoryFrame> History;
		double ParticleRestDensity = 0;
		std::string OriginNodeId;
		size_t OriginProcessorRow = 0;
		uint64_t Tick = 0, AuthoringRevision = 0;
		bool Initialized = false;
		bool operator==(const FluidDomainData &) const = default;
	};
}
