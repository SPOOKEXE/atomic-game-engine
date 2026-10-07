#pragma once

#include "../EvaluationBudget.hpp"

#include <engine/imagegraph/Document.hpp>
#include <engine/imagegraph/Particle3D.hpp>

#include <array>
#include <span>
#include <vector>

namespace engine::imagegraph::detail {
	class NodeContext;
	class PathRuntime3D;
	enum class SourceParticle3DSpawnType { Stream, Burst, Trigger };
	enum class SourceParticle3DSpawnSource { Shape, Path, MeshVertices, DirectData };
	enum class SourceParticle3DSpawnShape { Box, Sphere, Circle };
	struct SourceParticle3DControls {
		uint32_t Seed = 0;
		bool Spawn = true, Trigger = false, Billboard = false, FollowVelocity = false;
		SourceParticle3DSpawnType SpawnType = SourceParticle3DSpawnType::Stream;
		SourceParticle3DSpawnSource SpawnSource = SourceParticle3DSpawnSource::Shape;
		SourceParticle3DSpawnShape SpawnShape = SourceParticle3DSpawnShape::Box;
		int64_t SpawnDelay = 4, BurstDuration = 1;
		Vector2 SpawnAmount{2, 2}, Lifespan{20, 30};
		Vector3 SpawnOrigin{}, SpawnSpan{1, 1, 1};
		Quaternion SpawnRotation{};
		double QuaternionEpsilon = .00001;
		std::array<double, 6> Velocity{}, Acceleration{}, Rotation{}, RotationSpeed{};
		std::array<double, 6> Scale{1, 1, 1, 1, 1, 1};
		Vector2 ShapeVelocity{}, Size{1, 1}, Alpha{1, 1}, Gravity{}, GroundOffset{};
		bool Physics = false, Ground = false, Follow = false, Loop = true;
		double BounceAmount = .5, BounceFriction = .1;
		Vector4 PathRange{0, 0, 1, 1};
		const PathRuntime3D *SpawnPath = nullptr, *FollowPath = nullptr;
		// Caller derives this quote from its admitted path payload before constructing the runtimes.
		uint64_t PathSampleWork = 0;
		std::span<const std::span<const Vector3>> SpawnMeshVertices;
		std::span<const Vector3> SpawnData;
		const Curve *SpeedCurve = nullptr, *RotationCurve = nullptr, *SizeCurve = nullptr,
					*AlphaCurve = nullptr, *PathCurve = nullptr;
		const Gradient *LifetimeColour = nullptr, *RandomColour = nullptr;
		std::span<const Colour> Palette;
		uint32_t PaletteSelection = 0;
		uint32_t TotalFrames = 1;
		int64_t PreRender = -1;
		// Source Windows pool is 1024; its other-platform pool is 500. Lower native caps are explicit.
		uint32_t PoolCapacity = 1024;
	};
	struct SourceParticle3DSlot {
		MeshInstance3D Transform;
		ParticleRecord3D Particle;
		std::array<float, 4> StartPosition{};
		bool operator==(const SourceParticle3DSlot &) const = default;
	};
	struct SourceParticle3DState {
		AllocationReservation Charge;
		std::array<std::vector<SourceParticle3DSlot>, 2> Buffers;
		std::array<std::array<double, 33>, 5> CurveMaps{};
		SourcePathPointBuffer PathTemporary{SourcePathPointClass::Spatial, {}, 0, 1};
		uint64_t SpawnIndex = 0;
		size_t BufferIndex = 0, MaximumBufferIndex = 0;
		int64_t Frame = 0;
		bool Initialized = false;
	};
	// Allocates zeroed source ping-pong state and caches curveMap's 33 samples. No particles step.
	bool
	InitializeSourceParticle3DState(NodeContext &, const SourceParticle3DControls &, SourceParticle3DState &);
	// Initial loop prerender visits [TotalFrames-PreRender,TotalFrames), then resets only SpawnIndex.
	// FirstFrame is stepped afterward. Controls are borrowed only for this call.
	bool BeginSourceParticle3DState(
		NodeContext &, const SourceParticle3DControls &, int64_t firstFrame, SourceParticle3DState &
	);
	// Pure transactional snapshot step. Duplicate frames retain the snapshot; gaps refuse explicit replay.
	// SourceRandom's documented HTML5 RNG profile is used, without asserting Windows RNG parity.
	bool AdvanceSourceParticle3DState(
		NodeContext &,
		const SourceParticle3DControls &,
		const SourceParticle3DState &,
		int64_t frame,
		SourceParticle3DState &
	);
	// Validates a decoded snapshot without allocation, after bounded work admission.
	bool ValidateSourceParticle3DState(NodeContext &, const SourceParticle3DState &);
	// Source passes highest occupied slot index as draw count, excluding that final slot.
	size_t SourceParticle3DDrawCount(const SourceParticle3DState &) noexcept;
}
