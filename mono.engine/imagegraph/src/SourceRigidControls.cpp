#include "SourceRigidControls.hpp"

#include <cmath>
#include <numbers>

namespace engine::imagegraph::detail {
	Vector2 RigidControlReader::Pixels(std::string_view port, Vector2 fallback) const {
		const auto value = Context.Vec2(port, fallback);
		if (Context.Input(port)) return value;
		const auto unit = Context.Integer(std::string(port) + "_unit", 1);
		return unit == 1 ? Vector2{value.X * Dimension.X, value.Y * Dimension.Y} : value;
	}
	double RigidControlReader::PixelScalar(std::string_view port, double fallback) const {
		const double value = Context.Scalar(port, fallback);
		return Context.Integer(std::string(port) + "_unit", 1) == 1 ? value * Dimension.X : value;
	}
	SourceRigidWorld RigidControlReader::World() const {
		SourceRigidWorld world;
		world.Dimension = Dimension;
		world.Scale = Scale;
		world.Walls = Context.Boolean("use_wall") ? static_cast<uint8_t>(Context.Integer("walls", 2)) : 0;
		world.WallFriction = Context.Scalar("wall_friction", .2);
		world.WallRestitution = Context.Scalar("wall_bounciness", .2);
		return world;
	}
	SourceRigidFrame RigidControlReader::Frame() const {
		SourceRigidFrame frame;
		const double direction = Context.Scalar("direction", -90) * std::numbers::pi / 180;
		const double strength = Context.Scalar("strength", 10);
		frame.Gravity = {std::cos(direction) * strength, -std::sin(direction) * strength};
		frame.CanvasDimension = Dimension;
		frame.SimulationScale = Scale;
		frame.Sleepable = Context.Boolean("sleepable", true);
		frame.Continuous = Context.Boolean("continuous", true);
		return frame;
	}
	SourceRigidJoint
	RigidControlReader::Joint(std::string id, std::string bodyA, std::string bodyB, bool motor) const {
		SourceRigidJoint joint;
		joint.Id = std::move(id);
		joint.BodyA = std::move(bodyA);
		joint.BodyB = std::move(bodyB);
		joint.BreakForce = Context.Scalar("breaking_force");
		if (motor) {
			joint.Kind = SourceRigidJointKind::Motor;
			joint.Offset = Pixels("offset");
			joint.MaximumForce = Context.Scalar("max_force", 100);
			joint.MaximumTorque = Context.Scalar("max_torque", 100);
		} else {
			if (Context.Boolean("custom_anchor")) joint.Anchor = Pixels("anchor");
			joint.Stiffness = Context.Scalar("stiffness", 10);
			joint.Damping = Context.Scalar("damping", .5);
		}
		return joint;
	}
	SourceRigidBody RigidControlReader::Wall(std::string id, uint32_t side) const {
		Vector4 offsets{};
		if (const auto *value = Context.Find("offsets"))
			if (const auto *vector = std::get_if<Vector4>(value)) offsets = *vector;
		if (side > 3) {
			Context.Fail(Status::InvalidValue, "Rigid wall side is outside its four source slots");
			return {};
		}
		const double values[] = {offsets.X, offsets.Y, offsets.Z, offsets.W};
		const double offset = values[side];
		Vector2 a{}, b{};
		if (side == 0) {
			a = {0, offset};
			b = {Dimension.X, offset};
		}
		if (side == 1) {
			a = {0, Dimension.Y - offset};
			b = {Dimension.X, Dimension.Y - offset};
		}
		if (side == 2) {
			a = {offset, 0};
			b = {offset, Dimension.Y};
		}
		if (side == 3) {
			a = {Dimension.X - offset, 0};
			b = {Dimension.X - offset, Dimension.Y};
		}
		auto body = Segment(std::move(id), a, b);
		body.Friction = Context.Scalar("contact_friction", .2);
		return body;
	}
	SourceRigidBody RigidControlReader::Fragment(
		std::string id, Vector2 sourcePosition, std::span<const Vector2> points
	) const {
		SourceRigidBody body;
		body.Id = std::move(id);
		body.Position = sourcePosition;
		body.Shape = SourceRigidShape::Polygon;
		body.Points.assign(points.begin(), points.end());
		body.Density = Context.Scalar("density", 1);
		body.Friction = Context.Scalar("friction", .2);
		body.Restitution = Context.Scalar("bounciness", .2);
		body.LinearDamping = Context.Scalar("air_resistance");
		body.AngularDamping = Context.Scalar("rotation_resistance", .1);
		body.GravityScale = Context.Scalar("gravity_scale", 1);
		body.Enabled = Context.Boolean("activate_on_spawn", true);
		return body;
	}
	SourceRigidBody RigidControlReader::Object(std::string id, uint32_t width, uint32_t height) const {
		SourceRigidBody body;
		body.Id = std::move(id);
		body.Position = Pixels("spawn_position", {.5, .5});
		body.Size = {double(width), double(height)};
		body.RotationDegrees = Context.Scalar("spawn_rotation");
		const auto shape = Context.Integer("shape");
		body.Shape = shape == 0	  ? SourceRigidShape::Box
					 : shape == 1 ? SourceRigidShape::Circle
								  : SourceRigidShape::Polygon;
		body.Dynamic = Context.Boolean("affect_by_force", true);
		body.AuthoredMass = Context.Scalar("mass", 10);
		body.AuthoredCollisionGroup = Context.Integer("collision_group", 1);
		body.Friction = Context.Scalar("friction", .2);
		body.Restitution = Context.Scalar("bounciness", .2);
		body.LinearDamping = Context.Scalar("air_resistance");
		body.AngularDamping = Context.Scalar("rotation_resistance", .1);
		body.GravityScale = Context.Scalar("gravity_scale", 1);
		body.Enabled = Context.Boolean("activate_on_spawn", true);
		body.FixedRotation = Context.Boolean("fix_rotation");
		body.Sleepable = Context.Boolean("sleepable", true);
		body.Bullet = Context.Boolean("continuous");
		body.UseInitialVelocity = Context.Boolean("use_initial_velocity");
		body.InitialVelocity = Context.Vec2("initial_velocity");
		return body;
	}
	SourceRigidForce RigidControlReader::Force(std::string bodyId) const {
		SourceRigidForce force;
		force.BodyId = std::move(bodyId);
		force.AtCentre = false;
		force.Point = Pixels("position");
		force.SourceLocalPoint = Context.Integer("scope") != 0;
		force.Kind = static_cast<SourceRigidForceKind>(Context.Integer("force_type"));
		const double strength = Context.Scalar("strength", 1);
		const auto vector = Context.Vec2("force", {.1, 0});
		force.Force = {vector.X * strength / Scale, vector.Y * strength / Scale};
		force.Torque = Context.Scalar("torque") * strength;
		return force;
	}
	SourceRigidExplosion
	RigidControlReader::Explosion(std::span<const std::string> bodyIds, bool forceApply) const {
		SourceRigidExplosion explosion;
		explosion.Bodies.assign(bodyIds.begin(), bodyIds.end());
		explosion.Position = Pixels("position", forceApply ? Vector2{} : Vector2{.5, .5});
		explosion.Radius = PixelScalar("range", .25);
		explosion.Strength = Context.Scalar("strength", 1);
		explosion.Torque = forceApply ? 0 : Context.Scalar("torqe");
		explosion.Activate = !forceApply && Context.Boolean("activate_physics", true);
		explosion.DivideImpulseByScale = !forceApply;
		return explosion;
	}
	SourceRigidChange RigidControlReader::Override(std::string bodyId) const {
		SourceRigidChange change;
		change.BodyId = std::move(bodyId);
		if (Context.Boolean("set_positions")) {
			change.PositionWorld = Context.Vec2("positions");
			change.RelativePosition = Context.Integer("mode") == 1;
		}
		if (Context.Boolean("set_rotations"))
			change.RotationRadians = SourceRigidScalarChange{
				Context.Scalar("rotations"), static_cast<SourceRigidChangeMode>(Context.Integer("mode_2"))
			};
		const auto scalar = [&](std::string_view flag,
								std::string_view value,
								std::string_view mode) -> std::optional<SourceRigidScalarChange> {
			if (!Context.Boolean(flag)) return {};
			return SourceRigidScalarChange{
				Context.Scalar(value, 1), static_cast<SourceRigidChangeMode>(Context.Integer(mode))
			};
		};
		change.Mass = scalar("set_mass", "mass", "mode_5");
		change.Friction = scalar("set_friction", "friction", "mode_6");
		change.Restitution = scalar("set_bounciness", "bounciness", "mode_7");
		change.GravityScale = scalar("set_gravity_scale", "gravity_scale", "mode_8");
		return change;
	}
	SourceRigidStep RigidControlReader::Step(bool playing) const {
		return {
			Context.Scalar("timestep", 20),
			static_cast<uint32_t>(Context.Integer("quality", 8)),
			Context.Boolean("simulate", Context.Authored.Type != "pc.rigid_render_id"),
			playing
		};
	}
	SourceRigidBody RigidControlReader::Segment(std::string id, Vector2 start, Vector2 end) const {
		SourceRigidBody body;
		body.Id = std::move(id);
		body.Shape = SourceRigidShape::Segment;
		body.Points = {start, end};
		body.Dynamic = false;
		body.Friction = Context.Scalar("friction", .2);
		body.Restitution = Context.Scalar("bounciness", .2);
		return body;
	}
	SourceRigidBody RigidControlReader::Sensor(std::string id) const {
		SourceRigidBody body;
		body.Id = std::move(id);
		body.Sensor = true;
		body.Position = Pixels("position", {.5, .5});
		if (Context.Integer("shape") == 0) {
			const auto half = Pixels("span", {.5, .5});
			body.Size = {half.X * 2, half.Y * 2};
		} else {
			const auto radius = PixelScalar("radius", .5);
			body.Size = {radius * 2, radius * 2};
			body.Shape = SourceRigidShape::Circle;
		}
		return body;
	}
} // namespace engine::imagegraph::detail
