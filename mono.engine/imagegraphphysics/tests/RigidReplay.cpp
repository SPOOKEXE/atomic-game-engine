#include <engine/imagegraphphysics/RigidReplay.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>

TEST_SUITE_ID("engine.imagegraphphysics.rigid-replay")
using namespace engine::imagegraph;
using engine::imagegraphphysics::ReplayRigid;
namespace {
	SourceRigidHistory Stack() {
		SourceRigidHistory history;
		history.World.Dimension = {200, 200};
		history.World.Walls = 2;
		history.World.WallRestitution = 0;
		history.Frames.resize(181);
		for (auto &frame : history.Frames)
			frame.Events.push_back({{"render", 0}, SourceRigidStep{}});
		SourceRigidBody bottom, top;
		bottom.Id = "bottom";
		bottom.Position = {100, 80};
		bottom.Size = {20, 20};
		bottom.Restitution = 0;
		top = bottom;
		top.Id = "top";
		top.Position = {100, 40};
		history.Frames.front().Events.insert(
			history.Frames.front().Events.begin(), {{{"bottom", 0}, bottom}, {{"top", 0}, top}}
		);
		return history;
	}
	SourceRigidBody &Body(SourceRigidFrame &frame, size_t index) {
		return std::get<SourceRigidBody>(frame.Events[index].Command);
	}
	SourceRigidStep &Step(SourceRigidFrame &frame) {
		return std::get<SourceRigidStep>(frame.Events.back().Command);
	}
	void Spawn(SourceRigidFrame &frame, std::initializer_list<SourceRigidBody> bodies) {
		std::erase_if(frame.Events, [](const auto &event) {
			return std::holds_alternative<SourceRigidBody>(event.Command);
		});
		size_t index = 0;
		for (const auto &body : bodies)
			frame.Events.insert(
				frame.Events.begin() + static_cast<std::ptrdiff_t>(index++), {{body.Id, 0}, body}
			);
	}

	SourceRigidSnapshot
	Run(const SourceRigidHistory &history,
		uint64_t tick,
		std::optional<SourceRigidEventPosition> capture = {}) {
		SourceRigidSnapshot result;
		Diagnostic diagnostic;
		const auto status = ReplayRigid(history, tick, result, diagnostic, std::move(capture));
		INFO(diagnostic.Message);
		REQUIRE(status == Status::Ok);
		return result;
	}
}

TEST_CASE("Rigid replay resolves genuine floor and body contacts with repeatable seeks", "[rigid]") {
	const auto history = Stack();
	const auto initial = Run(history, 0), falling = Run(history, 20), settled = Run(history, 180);
	REQUIRE(initial.Bodies.size() == 2);
	REQUIRE(settled.Bodies.size() == 2);
	CHECK(falling.Bodies[0].Position.Y > initial.Bodies[0].Position.Y);
	CHECK(settled.Bodies[0].Position.Y == Catch::Approx(190).margin(1));
	CHECK(settled.Bodies[1].Position.Y == Catch::Approx(170).margin(1));
	bool floorContact = false, bodyContact = false;
	for (const auto &contact : settled.Contacts) {
		floorContact |= contact.A == "__wall/bottom" || contact.B == "__wall/bottom";
		bodyContact |=
			(contact.A == "bottom" && contact.B == "top") || (contact.A == "top" && contact.B == "bottom");
	}
	CHECK(floorContact);
	CHECK(bodyContact);
	CHECK(Run(history, 0) == initial);
	CHECK(Run(history, 20) == falling);
	CHECK(Run(history, 180) == settled);
}

TEST_CASE("Rigid replay preserves captured playing and simulation gates", "[rigid]") {
	auto history = Stack();
	for (auto &frame : history.Frames)
		Step(frame).Playing = false;
	const auto paused = Run(history, 180);
	CHECK(paused == Run(history, 0));
	CHECK(paused.Bodies[0].Position.Y == Catch::Approx(80));
	CHECK(paused.Bodies[1].Position.Y == Catch::Approx(40));
	for (auto &frame : history.Frames) {
		Step(frame).Playing = true;
		Step(frame).Simulate = false;
	}
	CHECK(Run(history, 180) == paused);
	Step(history.Frames.back()).Simulate = true;
	CHECK(Run(history, 180).Bodies[0].Position.Y > paused.Bodies[0].Position.Y);
}

TEST_CASE("Rigid circle fixtures collide and off-centre force applies real torque", "[rigid]") {
	auto circles = Stack();
	Body(circles.Frames[0], 0).Shape = SourceRigidShape::Circle;
	Body(circles.Frames[0], 1).Shape = SourceRigidShape::Circle;
	const auto circleState = Run(circles, 180);
	CHECK(circleState.Bodies[0].Position.Y < 191);
	CHECK_FALSE(circleState.Contacts.empty());

	auto history = Stack();
	history.World.Walls = 0;
	history.Frames.resize(2);
	for (auto &frame : history.Frames)
		frame.Gravity = {0, 0};
	const auto only = Body(history.Frames[0], 0);
	Spawn(history.Frames[0], {only});
	Step(history.Frames[0]).Simulate = false;
	history.Frames[1].Events.insert(
		history.Frames[1].Events.begin(),
		{{"force", 0}, SourceRigidForce{"bottom", {10, 0}, {100, 90}, false, true}}
	);
	const auto torqued = Run(history, 1);
	CHECK(torqued.Bodies[0].LinearVelocity.X == Catch::Approx(1.25));
	CHECK(std::abs(torqued.Bodies[0].AngularVelocity) > 0);
	Body(history.Frames[0], 0).AuthoredMass = 200;
	Body(history.Frames[0], 0).AuthoredCollisionGroup = -99;
	CHECK(Run(history, 1) == torqued);
	Body(history.Frames[0], 0).FixedRotation = true;
	CHECK(Run(history, 1).Bodies[0].AngularVelocity == 0);
}

TEST_CASE("Rigid recording validation preserves previous output and bounds history", "[rigid]") {
	auto history = Stack();
	const auto previous = Run(history, 0);
	auto output = previous;
	Diagnostic diagnostic;
	Spawn(history.Frames.back(), {Body(history.Frames.front(), 0), Body(history.Frames.front(), 1)});
	CHECK(ReplayRigid(history, 0, output, diagnostic) == Status::DuplicateId);
	CHECK(output == previous);
	history = Stack();
	Body(history.Frames[0], 0).Position.X = std::numeric_limits<double>::quiet_NaN();
	CHECK(ReplayRigid(history, 0, output, diagnostic) == Status::InvalidValue);
	CHECK(output == previous);
	history = Stack();
	history.Frames[0].Events.push_back({{"force", 0}, SourceRigidForce{"missing", {1, 0}, {}, true, true}});
	CHECK(ReplayRigid(history, 0, output, diagnostic) == Status::InvalidValue);
	CHECK(output == previous);
	history = Stack();
	CHECK(ReplayRigid(history, 181, output, diagnostic) == Status::InvalidValue);
	CHECK(output == previous);
	history.Frames.resize(4097);
	CHECK(ReplayRigid(history, 0, output, diagnostic) == Status::LimitExceeded);
	CHECK(diagnostic.Code == Status::LimitExceeded);
	CHECK(output == previous);
	history = Stack();
	Body(history.Frames[0], 0).Size.X = std::numeric_limits<double>::denorm_min();
	CHECK(ReplayRigid(history, 0, output, diagnostic) == Status::InvalidValue);
	CHECK(output == previous);
	history = Stack();
	history.World.Dimension.X = .001;
	CHECK(ReplayRigid(history, 0, output, diagnostic) == Status::InvalidValue);
	CHECK(output == previous);
}

TEST_CASE(
	"Rigid copied body controls preserve disabled, static, and unscaled velocity semantics", "[rigid]"
) {
	auto history = Stack();
	history.World.Walls = 0;
	SourceRigidBody moving, fixed, disabled;
	moving.Id = "velocity";
	moving.Position = {40, 80};
	moving.Size = {10, 10};
	moving.GravityScale = 0;
	moving.UseInitialVelocity = true;
	moving.InitialVelocity = {1, 0};
	fixed = moving;
	fixed.Id = "static";
	fixed.Position.X = 100;
	fixed.Dynamic = false;
	disabled = moving;
	disabled.Id = "disabled";
	disabled.Position.X = 160;
	disabled.Enabled = false;
	Spawn(history.Frames[0], {moving, fixed, disabled});
	const auto first = Run(history, 0), later = Run(history, 20);
	CHECK(first.Bodies[0].Position.X == Catch::Approx(41));
	CHECK(later.Bodies[0].Position.Y == Catch::Approx(80));
	CHECK(later.Bodies[0].LinearVelocity.X == Catch::Approx(1));
	CHECK(later.Bodies[1].Position == first.Bodies[1].Position);
	CHECK(later.Bodies[2].Position == first.Bodies[2].Position);
}

TEST_CASE(
	"Rigid shared owner preserves ordered multiple render events and frozen intermediate captures", "[rigid]"
) {
	auto history = Stack();
	history.Frames.resize(2);
	history.World.Walls = 0;
	history.Frames[0].Gravity = {0, 0};
	history.Frames[1].Gravity = {0, 0};
	history.Frames[1].Events = {
		{{"render-a", 0}, SourceRigidStep{}},
		{{"force", 0}, SourceRigidForce{"bottom", {10, 0}, {}, true, true}},
		{{"render-b", 0}, SourceRigidStep{}}
	};
	const auto before = Run(history, 1, SourceRigidEventPosition{"render-a", 0});
	const auto after = Run(history, 1, SourceRigidEventPosition{"render-b", 0});
	CHECK(before.Bodies[0].LinearVelocity.X == 0);
	CHECK(after.Bodies[0].LinearVelocity.X > 0);
	CHECK(after.Bodies[0].Position.X > before.Bodies[0].Position.X);
	CHECK(Run(history, 1) == after);
	CHECK(Run(history, 1, SourceRigidEventPosition{"render-a", 0}) == before);
	CHECK(Run(history, 1, SourceRigidEventPosition{"render-b", 0}) == after);
	CHECK(before.Bodies.size() == 2);
	Diagnostic diagnostic;
	auto preserved = before;
	CHECK(
		ReplayRigid(history, 1, preserved, diagnostic, SourceRigidEventPosition{"missing-render", 0}) ==
		Status::InvalidOutput
	);
	CHECK(preserved == before);
	history.Frames[1].Events.push_back(history.Frames[1].Events.front());
	CHECK(ReplayRigid(history, 1, preserved, diagnostic) == Status::DuplicateId);
	CHECK(preserved == before);
}

TEST_CASE("Rigid owner scale and canvas changes preserve previously spawned physical geometry", "[rigid]") {
	auto history = Stack();
	history.Frames.resize(2);
	for (auto &frame : history.Frames)
		Step(frame).Simulate = false;
	const auto before = Run(history, 0);
	history.Frames[1].SimulationScale = 100;
	history.Frames[1].CanvasDimension = Vector2{400, 300};
	const auto after = Run(history, 1);
	CHECK(after.SimulationScale == 100);
	CHECK(after.CanvasDimension == Vector2{400, 300});
	CHECK(after.Bodies[0].Position.X == before.Bodies[0].Position.X * 2);
	CHECK(after.Bodies[0].Position.Y == before.Bodies[0].Position.Y * 2);
	CHECK(after.Bodies[0].LinearVelocity == before.Bodies[0].LinearVelocity);
	CHECK(Run(history, 0) == before);
}

TEST_CASE("Rigid provider bounds copied snapshots and row checkpoints freeze observations", "[rigid]") {
	auto history = Stack();
	history.Frames.resize(2);
	history.Frames[1].Events = {
		{{"observer", 0, 0}, SourceRigidEvent::Checkpoint{}},
		{{"observer", 0, 1}, SourceRigidEvent::Checkpoint{}},
		{{"render", 0}, SourceRigidStep{}}
	};
	const auto a = Run(history, 1, SourceRigidEventPosition{"observer", 0, 0});
	const auto b = Run(history, 1, SourceRigidEventPosition{"observer", 0, 1});
	CHECK(a == b);
	CHECK(Run(history, 1).Bodies[0].Position.Y > a.Bodies[0].Position.Y);
	engine::imagegraphphysics::RigidProvider native;
	SourceRigidProvider &provider = native;
	Diagnostic diagnostic;
	auto output = a;
	CHECK(
		provider.Replay(history, 1, {}, sizeof(SourceRigidSnapshot), output, diagnostic) ==
		Status::LimitExceeded
	);
	CHECK(diagnostic.Code == Status::LimitExceeded);
	CHECK(output == a);
	CHECK(provider.Replay(history, 1, {}, 4096, output, diagnostic) == Status::Ok);
	CHECK(output == Run(history, 1));
}

TEST_CASE("Rigid impulses and torque use real mass and angular inertia", "[rigid]") {
	SourceRigidHistory history;
	history.Frames.resize(1);
	history.Frames[0].Gravity = {};
	SourceRigidBody body;
	body.Id = "body";
	body.Position = {100, 100};
	body.Size = {20, 20};
	body.AngularDamping = 0;
	SourceRigidForce impulse;
	impulse.BodyId = body.Id;
	impulse.Kind = SourceRigidForceKind::Impulse;
	impulse.Force = {.16, 0};
	SourceRigidForce angular = impulse;
	angular.Kind = SourceRigidForceKind::AngularImpulse;
	angular.Torque = .01;
	history.Frames[0].Events = {{{"spawn", 0}, body}, {{"impulse", 0}, impulse}, {{"angular", 0}, angular}};
	const auto result = Run(history, 0);
	CHECK(result.Bodies[0].Mass == Catch::Approx(.16));
	CHECK(result.Bodies[0].LinearVelocity.X == Catch::Approx(1));
	CHECK(result.Bodies[0].AngularVelocity > 1);
	CHECK(result == Run(history, 0));
	history.Frames[0].Events.erase(history.Frames[0].Events.begin() + 2);
	angular.Kind = SourceRigidForceKind::Torque;
	angular.Torque = .1;
	history.Frames[0].Events.push_back({{"torque", 0}, angular});
	history.Frames[0].Events.push_back({{"render", 0}, SourceRigidStep{}});
	CHECK(Run(history, 0).Bodies[0].AngularVelocity > 0);
}

TEST_CASE("Rigid local wrapper preserves its world to local point quirk", "[rigid]") {
	SourceRigidHistory history;
	history.Frames.resize(1);
	history.Frames[0].Gravity = {};
	SourceRigidBody body;
	body.Id = "body";
	body.Position = {100, 100};
	body.Size = {20, 20};
	SourceRigidForce impulse;
	impulse.BodyId = body.Id;
	impulse.Kind = SourceRigidForceKind::Impulse;
	impulse.AtCentre = false;
	impulse.Point = body.Position;
	impulse.Force = {.16, 0};
	history.Frames[0].Events = {{{"spawn", 0}, body}, {{"impulse", 0}, impulse}};
	CHECK(Run(history, 0).Bodies[0].AngularVelocity == 0);
	std::get<SourceRigidForce>(history.Frames[0].Events[1].Command).SourceLocalPoint = true;
	const auto local = Run(history, 0);
	CHECK(local.Bodies[0].AngularVelocity > 1);
	CHECK(local.Bodies[0].LinearVelocity.X == Catch::Approx(1));
}

TEST_CASE("Rigid overrides and activation replay between ordered observations", "[rigid]") {
	SourceRigidHistory history;
	history.Frames.resize(1);
	history.Frames[0].Gravity = {};
	SourceRigidBody body;
	body.Id = "body";
	body.Size = {20, 20};
	SourceRigidChange change;
	change.BodyId = body.Id;
	change.PositionWorld = Vector2{2, 3};
	change.RotationRadians = SourceRigidScalarChange{.5};
	change.Mass = SourceRigidScalarChange{2};
	change.Friction = SourceRigidScalarChange{2, SourceRigidChangeMode::Multiply};
	change.Restitution = SourceRigidScalarChange{.1, SourceRigidChangeMode::Add};
	change.GravityScale = SourceRigidScalarChange{0};
	change.Enabled = false;
	history.Frames[0].Events = {{{"spawn", 0}, body}, {{"override", 0}, change}};
	const auto first = Run(history, 0);
	CHECK(first.Bodies[0].Position.X == Catch::Approx(100));
	CHECK(first.Bodies[0].WorldCentreOfMass.Y == Catch::Approx(150));
	CHECK(first.Bodies[0].RotationDegrees == Catch::Approx(.5 * 180 / 3.141592653589793).margin(.05));
	CHECK(first.Bodies[0].Mass == Catch::Approx(2));
	CHECK(first.Bodies[0].Friction == Catch::Approx(.4));
	CHECK(first.Bodies[0].Restitution == Catch::Approx(.3));
	CHECK(first.Bodies[0].GravityScale == 0);
	CHECK_FALSE(first.Bodies[0].Enabled);
	SourceRigidChange activate;
	activate.BodyId = body.Id;
	activate.Enabled = true;
	activate.PositionWorld = Vector2{1, 0};
	activate.RelativePosition = true;
	history.Frames[0].Events.push_back({{"activate", 0}, activate});
	const auto second = Run(history, 0);
	CHECK(second.Bodies[0].Enabled);
	CHECK(second.Bodies[0].Position.X == Catch::Approx(150));
	CHECK(Run(history, 0, SourceRigidEventPosition{"override", 0}) == first);
	CHECK(Run(history, 0) == second);
	std::get<SourceRigidChange>(history.Frames[0].Events[1].Command).Mass = SourceRigidScalarChange{-1};
	SourceRigidSnapshot retained = first;
	Diagnostic diagnostic;
	CHECK(ReplayRigid(history, 0, retained, diagnostic) == Status::InvalidValue);
	CHECK(retained == first);
}

TEST_CASE("Rigid custom convex fixtures contact genuine authored segments", "[rigid]") {
	SourceRigidHistory history;
	history.Frames.resize(181);
	for (auto &frame : history.Frames)
		frame.Events.push_back({{"render", 0}, SourceRigidStep{}});
	SourceRigidBody floor;
	floor.Id = "segment";
	floor.Shape = SourceRigidShape::Segment;
	floor.Points = {{0, 200}, {200, 200}};
	floor.Dynamic = false;
	floor.Restitution = 0;
	SourceRigidBody polygon;
	polygon.Id = "custom";
	polygon.Position = {100, 60};
	polygon.Shape = SourceRigidShape::Polygon;
	polygon.Points = {{-10, -10}, {10, -10}, {10, 10}, {-10, 10}};
	polygon.Restitution = 0;
	Spawn(history.Frames[0], {floor, polygon});
	const auto settled = Run(history, 180);
	CHECK(settled.Bodies[1].Position.Y == Catch::Approx(190).margin(1));
	REQUIRE_FALSE(settled.Contacts.empty());
	CHECK(settled.Contacts[0].A == "segment");
	CHECK(settled.Contacts[0].B == "custom");
	REQUIRE(settled.Contacts[0].PointCount > 0);
	CHECK(settled.Contacts[0].PointCount <= 2);
	CHECK(settled.Contacts[0].Points[0].Point.Y * 50 == Catch::Approx(200).margin(1));
	CHECK(settled == Run(history, 180));
	Body(history.Frames[0], 1).Points = {{0, 0}, {1, 0}, {2, 0}};
	SourceRigidSnapshot retained = settled;
	Diagnostic diagnostic;
	CHECK(ReplayRigid(history, 0, retained, diagnostic) == Status::InvalidValue);
	CHECK(retained == settled);
	Body(history.Frames[0], 0).Points[1] = Body(history.Frames[0], 0).Points[0];
	CHECK(ReplayRigid(history, 0, retained, diagnostic) == Status::InvalidValue);
}

TEST_CASE("Rigid sensors report step-owned overlaps without collision impulses", "[rigid]") {
	SourceRigidHistory history;
	history.Frames.resize(2);
	for (auto &frame : history.Frames)
		frame.Gravity = {};
	SourceRigidBody sensor;
	sensor.Id = "sensor";
	sensor.Position = {100, 100};
	sensor.Size = {40, 40};
	sensor.Sensor = true;
	SourceRigidBody visitor;
	visitor.Id = "visitor";
	visitor.Position = sensor.Position;
	visitor.Size = {10, 10};
	history.Frames[0].Events = {
		{{"sensor", 0}, sensor}, {{"visitor", 0}, visitor}, {{"render", 0}, SourceRigidStep{}}
	};
	const auto initial = Run(history, 0);
	REQUIRE(initial.Overlaps.size() == 1);
	CHECK(initial.Overlaps[0].Sensor == "sensor");
	CHECK(initial.Overlaps[0].Visitor == "visitor");
	CHECK(initial.Contacts.empty());
	CHECK(initial.Bodies[1].Position.X == Catch::Approx(100));
	SourceRigidChange move;
	move.BodyId = "sensor";
	move.PositionWorld = Vector2{10, 10};
	history.Frames[1].Events = {{{"move", 0}, move}, {{"render", 0}, SourceRigidStep{}}};
	CHECK(Run(history, 1).Overlaps.empty());
	CHECK(Run(history, 0) == initial);
}

TEST_CASE("Rigid weld and motor joints constrain bodies and break after native steps", "[rigid]") {
	SourceRigidHistory history;
	history.Frames.resize(61);
	for (auto &frame : history.Frames) {
		frame.Gravity = {};
		frame.Events.push_back({{"render", 0}, SourceRigidStep{}});
	}
	SourceRigidBody anchor;
	anchor.Id = "anchor";
	anchor.Position = {100, 100};
	anchor.Size = {10, 10};
	anchor.Dynamic = false;
	SourceRigidBody moving = anchor;
	moving.Id = "moving";
	moving.Position = {130, 100};
	moving.Dynamic = true;
	Spawn(history.Frames[0], {anchor, moving});
	SourceRigidJoint joint;
	joint.Id = "weld";
	joint.BodyA = "anchor";
	joint.BodyB = "moving";
	joint.Stiffness = 0;
	SourceRigidForce impulse;
	impulse.BodyId = "moving";
	impulse.Kind = SourceRigidForceKind::Impulse;
	impulse.Force = {0, .1};
	history.Frames[0].Events.insert(
		history.Frames[0].Events.end() - 1, {{{"joint", 0}, joint}, {{"impulse", 0}, impulse}}
	);
	const auto welded = Run(history, 60);
	CHECK(welded.ActiveJoints == 1);
	CHECK(welded.Bodies[1].Position.Y == Catch::Approx(100).margin(.1));
	CHECK(welded == Run(history, 60));
	std::get<SourceRigidJoint>(history.Frames[0].Events[2].Command).BreakForce = .001;
	const auto broken = Run(history, 60);
	CHECK(broken.ActiveJoints == 0);
	CHECK(broken.BrokenJoints == 1);
	CHECK(std::abs(broken.Bodies[1].Position.Y - 100) > 1);
	CHECK(broken == Run(history, 60));
	std::get<SourceRigidJoint>(history.Frames[0].Events[2].Command) = joint;
	auto &motor = std::get<SourceRigidJoint>(history.Frames[0].Events[2].Command);
	motor.Kind = SourceRigidJointKind::Motor;
	motor.Offset = {30, 0};
	const auto driven = Run(history, 60);
	CHECK(driven.ActiveJoints == 1);
	CHECK(driven.Bodies[1].Position.X == Catch::Approx(130).margin(.1));
	CHECK(driven.Bodies[1].Position.Y == Catch::Approx(100).margin(.1));
}

TEST_CASE("Rigid explosion nodes retain distinct source scale and radius rules", "[rigid]") {
	SourceRigidHistory history;
	history.Frames.resize(1);
	history.Frames[0].Gravity = {};
	SourceRigidBody body;
	body.Id = "body";
	body.Position = {100, 100};
	body.Size = {20, 20};
	body.Enabled = false;
	SourceRigidExplosion explosion;
	explosion.Bodies = {"body"};
	explosion.Position = {50, 100};
	explosion.Radius = 2;
	explosion.Strength = 4;
	history.Frames[0].Events = {{{"spawn", 0}, body}, {{"explode", 0}, explosion}};
	const auto scaled = Run(history, 0);
	CHECK(scaled.Bodies[0].Enabled);
	CHECK(scaled.Bodies[0].LinearVelocity.X == Catch::Approx(.125));
	CHECK(scaled.Bodies[0].LinearVelocity.Y == 0);
	auto &source = std::get<SourceRigidExplosion>(history.Frames[0].Events[1].Command);
	source.DivideImpulseByScale = false;
	const auto forceApplyExplosion = Run(history, 0);
	CHECK(forceApplyExplosion.Bodies[0].LinearVelocity.X == Catch::Approx(6.25));
	source.Radius = 1;
	CHECK_FALSE(Run(history, 0).Bodies[0].Enabled);
	CHECK(Run(history, 0).Bodies[0].LinearVelocity.X == 0);
}

TEST_CASE("Rigid fracture fixture density updates genuine polygon mass", "[rigid]") {
	SourceRigidHistory history;
	history.Frames.resize(1);
	history.Frames[0].Gravity = {};
	SourceRigidBody fragment;
	fragment.Id = "fragment";
	fragment.Shape = SourceRigidShape::Polygon;
	fragment.Points = {{0, 0}, {20, 0}, {20, 20}, {0, 20}};
	fragment.Density = 3;
	history.Frames[0].Events = {{{"spawn", 0}, fragment}};
	const auto dense = Run(history, 0);
	CHECK(dense.Bodies[0].Mass == Catch::Approx(.48));
	CHECK(dense.Bodies[0].WorldCentreOfMass.X == Catch::Approx(10));
	CHECK(dense.Bodies[0].WorldCentreOfMass.Y == Catch::Approx(10));
	Body(history.Frames[0], 0).Density.reset();
	CHECK(Run(history, 0).Bodies[0].Mass == Catch::Approx(.16));
}
