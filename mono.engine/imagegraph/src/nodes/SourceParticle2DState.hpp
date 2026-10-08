#pragma once

#include "../EvaluationBudget.hpp"

#include <engine/imagegraph/Document.hpp>

#include <array>
#include <span>
#include <vector>

namespace engine::imagegraph::detail {
	class NodeContext;
	class PathRuntime;
	struct SourceParticle2DRotation {
		std::array<double, 6> Values{};
		size_t Count = 6;
	};
	struct SourceParticle2DControls {
		uint32_t Seed = 0, PoolCapacity = 512, TotalFrames = 1;
		int64_t SpawnDelay = 4, BurstDuration = 1, PreRender = -1, LineLife = 4;
		int SpriteSelection = 0, AnimationEnd = 0, SpawnType = 0, SpawnSource = 0, Distribution = 1,
			DirectionDistribution = 0, RotationType = 0, Wrap = 0, RenderType = 0, BlendMode = 0,
			PaletteSelection = 0, GroundOffsetType = 0;
		bool Spawn = true, Trigger = false, StretchAnimation = false, DirectedFromCenter = false,
			 RotateByDirection = false, Render = true, Loop = true, RoundPosition = true, SortY = false,
			 FollowPath = false, PathLoop = true, Physics = false, TurnBothDirections = false, Ground = false,
			 Wiggles = false;
		bool SpriteArray = false, SpriteEmptyArray = false;
		Vector2 AnimationSpeed{1, 1}, SpawnAmount{2, 2}, Lifespan{20, 30}, Speed{1, 2}, AngleRange{0, 360},
			Size{1, 1}, Alpha{1, 1}, RangeShift{}, Friction{}, Acceleration{}, Gravity{}, Turning{},
			GroundOffset{}, DirectionWiggle{}, PositionWiggle{}, RotationWiggle{}, ScaleWiggle{};
		Vector4 Scale{1, 1, 1, 1}, PathRange{0, 0, 1, 1};
		Vector2 Dimension{1, 1};
		Area SpawnArea{};
		SourceParticle2DRotation Direction{{0, 45, 135, 0, 0, 0}, 5}, Rotation{}, RotationSpeed{},
			TargetAngle{};
		std::span<const SourceParticle2DRotation> Directions, Rotations;
		double Distance = 8, UniformPeriod = 4, SnapRotation = 0, Deviation = 1, GravityDirection = -90,
			   TurnScaleWithSpeed = 0, BounceAmount = .5, BounceFriction = .1;
		std::span<const Image> Sprites;
		std::span<const std::optional<Vector4>> SpriteAtlasRects;
		const Image *Background = nullptr, *DistributionMap = nullptr, *SampleSurface = nullptr;
		std::span<const Vector2> SpawnData, PoissonPoints;
		const PathRuntime *SpawnPath = nullptr, *Path = nullptr;
		const Path2D *PathPayload = nullptr;
		const Curve *SpeedCurve = nullptr, *RotationCurve = nullptr, *TargetCurve = nullptr,
					*ScaleCurve = nullptr, *AlphaCurve = nullptr, *PathSpeedCurve = nullptr,
					*PathDeviationCurve = nullptr;
		const Gradient *SpawnColour = nullptr, *LifetimeColour = nullptr;
		std::span<const Colour> Palette;
		uint64_t PathSampleWork = 0;
	};
	struct SourceParticle2DSlot {
		AllocationReservation Charge;
		ParticleData2D Data;
		Gradient LifetimeColour;
		std::optional<Path2D> FollowPath;
		std::array<double, 2> Start{}, Previous{}, Draw{}, Velocity{}, InitialVelocity{}, BaseScale{1, 1},
			DrawScale{1, 1};
		double DrawRotation = 0;
		std::optional<Vector4> AtlasRect;
		std::array<double, 2> PathRange{0, 1};
		double Life = 0, LifeTotal = 0, BaseRotation = 0, RotationSpeed = 0, TargetAngle = 0,
			   SnapRotation = 0, Alpha = 1, Acceleration = 0, Friction = 0, GravityX = 0, GravityY = 0,
			   Turning = 0, TurnScale = 0, GroundY = 0, Bounce = 0, GroundFriction = 1, AnimationSpeed = 1,
			   PathDeviation = 0, Direction = 0, DirectionSpeed = 0;
		uint32_t Seed = 0, Blend = 0xffffffff;
		size_t HistoryIndex = 0, TrailLife = 0;
		int SpriteSelection = 0, AnimationEnd = 0, RotationType = 0, RenderType = 0, Wrap = 0;
		bool PreviousDefined = false, DrawDefined = false, Physics = false, Ground = false, Wiggles = false,
			 RotateByDirection = false, StretchAnimation = false, PathLoop = true;
		std::vector<double> ScaleXHistory, ScaleYHistory, AlphaHistory;
		std::vector<uint32_t> BlendHistory;
	};
	struct SourceParticle2DState {
		AllocationReservation Charge;
		std::vector<SourceParticle2DSlot> Slots;
		std::array<std::vector<double>, 7> Curves;
		std::array<std::array<double, 1000>, 6> WiggleMaps{};
		std::array<double, 6> WiggleAmplitudes{};
		uint32_t Seed = 0;
		uint64_t SpawnTotal = 0;
		size_t Runner = 0, SpawnIndex = 0;
		int64_t Frame = -1;
		bool Initialized = false;
		std::optional<Image> LastSurface;
		AllocationReservation LastSurfaceCharge;
	};
	double SourceParticle2DCurveAt(const SourceParticle2DState &, size_t curve, double ratio);
	bool BeginSourceParticle2DState(NodeContext &, const SourceParticle2DControls &, SourceParticle2DState &);
	bool AdvanceSourceParticle2DState(
		NodeContext &, const SourceParticle2DControls &, SourceParticle2DState &, int64_t frame
	);
	bool ValidateSourceParticle2DState(NodeContext &, const SourceParticle2DState &);
}
