#pragma once

#include <engine/imagegraph/Document.hpp>

namespace engine::imagegraph {
	// Copied source controls. Pixel coordinates become Box2D metres only in the host.
	enum class SourceRigidShape : uint8_t { Box, Circle, Polygon, Segment };
	struct SourceRigidBody {
		std::string Id;
		SourceRigidShape Shape = SourceRigidShape::Box;
		Vector2 Position{}, Size{1, 1}, InitialVelocity{};
		std::vector<Vector2> Points;
		bool Sensor = false;
		// Fracture explicitly sets density; ordinary object spawn retains the fixture default.
		std::optional<double> Density;
		double RotationDegrees = 0, Friction = .2, Restitution = .2;
		double LinearDamping = 0, AngularDamping = .1, GravityScale = 1;
		// Source spawn reads these authored controls but never applies them to the fixture.
		double AuthoredMass = 10;
		int64_t AuthoredCollisionGroup = 1;
		bool Dynamic = true, Enabled = true, FixedRotation = false, Sleepable = true, Bullet = false;
		bool UseInitialVelocity = false;
		bool operator==(const SourceRigidBody &) const = default;
	};
	struct SourceRigidWorld {
		Vector2 Dimension{1, 1};
		double Scale = 50, WallFriction = .2, WallRestitution = .2;
		uint8_t Walls = 0;
		bool operator==(const SourceRigidWorld &) const = default;
	};
	enum class SourceRigidForceKind : uint8_t { Force, Impulse, Torque, AngularImpulse };
	struct SourceRigidForce {
		std::string BodyId;
		Vector2 Force{}, Point{};
		bool AtCentre = true, Wake = true;
		// The pinned local wrapper passes GetLocalPoint directly to the world-point force API.
		bool SourceLocalPoint = false;
		SourceRigidForceKind Kind = SourceRigidForceKind::Force;
		double Torque = 0;
		bool operator==(const SourceRigidForce &) const = default;
	};
	enum class SourceRigidChangeMode : uint8_t { Absolute, Add, Multiply };
	struct SourceRigidScalarChange {
		double Value = 0;
		SourceRigidChangeMode Mode = SourceRigidChangeMode::Absolute;
		bool operator==(const SourceRigidScalarChange &) const = default;
	};
	struct SourceRigidChange {
		std::string BodyId;
		// Source override scalar positions already contain world units, unlike spawn positions.
		std::optional<Vector2> PositionWorld, LinearVelocity;
		bool RelativePosition = false;
		std::optional<SourceRigidScalarChange> RotationRadians, Mass, Friction, Restitution, GravityScale;
		std::optional<bool> Enabled, Awake;
		bool operator==(const SourceRigidChange &) const = default;
	};
	enum class SourceRigidJointKind : uint8_t { Weld, Motor };
	struct SourceRigidJoint {
		std::string Id, BodyA, BodyB;
		SourceRigidJointKind Kind = SourceRigidJointKind::Weld;
		std::optional<Vector2> Anchor;
		Vector2 Offset{};
		double Stiffness = 10, Damping = .5, MaximumForce = 100, MaximumTorque = 100, BreakForce = 0;
		bool operator==(const SourceRigidJoint &) const = default;
	};
	struct SourceRigidExplosion {
		std::vector<std::string> Bodies;
		Vector2 Position{};
		// Both source explosion nodes compare world-space distance with the unscaled authored radius.
		double Radius = 1, Strength = 1, Torque = 0;
		bool Activate = true, DivideImpulseByScale = true;
		bool operator==(const SourceRigidExplosion &) const = default;
	};
	struct SourceRigidStep {
		double TimeStepMilliseconds = 20;
		uint32_t Quality = 8;
		bool Simulate = true, Playing = true;
		bool operator==(const SourceRigidStep &) const = default;
	};
	struct SourceRigidEventPosition {
		std::string ConsumerId;
		uint32_t Ordinal = 0;
		uint32_t ProcessorRow = 0;
		bool operator==(const SourceRigidEventPosition &) const = default;
	};
	struct SourceRigidEvent {
		SourceRigidEventPosition Position;
		struct Checkpoint {
			bool operator==(const Checkpoint &) const = default;
		};
		std::variant<
			SourceRigidBody,
			SourceRigidForce,
			SourceRigidStep,
			SourceRigidChange,
			SourceRigidJoint,
			SourceRigidExplosion,
			Checkpoint>
			Command;
		bool operator==(const SourceRigidEvent &) const = default;
	};
	// One source tick carries every consumer's ordered world operations.
	struct SourceRigidFrame {
		Vector2 Gravity{0, 10};
		std::optional<double> SimulationScale;
		std::optional<Vector2> CanvasDimension;
		bool Sleepable = true, Continuous = true;
		std::vector<SourceRigidEvent> Events;
		bool operator==(const SourceRigidFrame &) const = default;
	};
	struct SourceRigidHistory {
		std::string OwnerId = "rigid-world";
		SourceRigidWorld World;
		std::vector<SourceRigidFrame> Frames;
		bool operator==(const SourceRigidHistory &) const = default;
	};
	struct SourceRigidBodyState {
		std::string Id;
		Vector2 Position{}, LinearVelocity{};
		double RotationDegrees = 0, AngularVelocity = 0;
		bool Awake = false;
		Vector2 WorldCentreOfMass{};
		double Mass = 0, Friction = 0, Restitution = 0, GravityScale = 1;
		bool Enabled = true, Sensor = false;
		bool operator==(const SourceRigidBodyState &) const = default;
	};
	struct SourceRigidContactPoint {
		Vector2 Point{}, AnchorA{}, AnchorB{};
		double Separation = 0, NormalImpulse = 0, TangentImpulse = 0, TotalNormalImpulse = 0,
			   NormalVelocity = 0;
		uint16_t Id = 0;
		bool Persisted = false;
		bool operator==(const SourceRigidContactPoint &) const = default;
	};
	struct SourceRigidContact {
		std::string A, B;
		Vector2 Normal{};
		double NormalImpulse = 0, RollingImpulse = 0;
		// Manifold point and anchors remain in native world units.
		uint32_t PointCount = 0;
		std::array<SourceRigidContactPoint, 2> Points{};
		bool operator==(const SourceRigidContact &) const = default;
	};
	struct SourceRigidOverlap {
		std::string Sensor, Visitor;
		bool operator==(const SourceRigidOverlap &) const = default;
	};
	struct SourceRigidSnapshot {
		Vector2 CanvasDimension{};
		double SimulationScale = 50;
		uint32_t ActiveJoints = 0, BrokenJoints = 0;
		std::vector<SourceRigidBodyState> Bodies;
		std::vector<SourceRigidContact> Contacts;
		std::vector<SourceRigidOverlap> Overlaps;
		bool operator==(const SourceRigidSnapshot &) const = default;
	};
	// Borrowed only for synchronous evaluation. Copied histories and handles retain no provider.
	class SourceRigidProvider {
	  public:
		virtual ~SourceRigidProvider() = default;
		virtual Status Replay(
			const SourceRigidHistory &history,
			uint64_t tick,
			std::optional<SourceRigidEventPosition> captureAt,
			uint64_t maximumSnapshotBytes,
			SourceRigidSnapshot &output,
			Diagnostic &diagnostic
		) = 0;
	};
}
