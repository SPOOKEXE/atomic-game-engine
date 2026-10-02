#include <engine/imagegraph/RigidReplay.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <limits>
TEST_SUITE_ID("engine.imagegraph.rigid_replay")
using namespace engine::imagegraph;
namespace {
	RigidReplayState RecordedOwner() {
		RigidReplayState state;
		RigidOwnerReplayState owner;
		owner.History.OwnerId = "owner";
		SourceRigidFrame frame;
		frame.Events.push_back({{"body", 0, 0}, SourceRigidBody{"body/0"}});
		frame.Events.push_back({{"render", 0, 0}, SourceRigidStep{}});
		owner.History.Frames.push_back(std::move(frame));
		RigidVisualFrame visualFrame;
		RigidVisualMutation mutation;
		mutation.Position = {"body", 0, 0};
		mutation.Data.BodyId = "body/0";
		mutation.Data.Texture = SurfaceValue{Image{1, 1, {10, 20, 30, 255}}};
		visualFrame.Mutations.push_back(std::move(mutation));
		owner.VisualFrames.push_back(std::move(visualFrame));
		RigidNodeFrame nodes;
		RigidNodeReplayState node;
		node.NodeId = "body";
		node.SpawnIndex = 1;
		node.OutputBodyIds = {"body/0"};
		nodes.Nodes.push_back(std::move(node));
		owner.NodeFrames.push_back(std::move(nodes));
		state.Owners.push_back(std::move(owner));
		return state;
	}
}
TEST_CASE("Rigid journal owns event-time visuals and source node histories", "[imagegraph][rigid]") {
	auto state = RecordedOwner();
	Diagnostic diagnostic;
	REQUIRE(ValidateRigidReplay(state, Limits::MaximumEvaluationBytes, diagnostic) == Status::Ok);
	auto copied = state;
	copied.Owners[0].NodeFrames[0].Nodes[0].OutputBodyIds[0] = "body/1";
	copied.Owners[0].VisualFrames[0].Mutations[0].Data.Texture->Data.Pixels[0] = 99;
	CHECK(state.Owners[0].NodeFrames[0].Nodes[0].OutputBodyIds[0] == "body/0");
	CHECK(state.Owners[0].VisualFrames[0].Mutations[0].Data.Texture->Data.Pixels[0] == 10);
	const auto before = RetainedRigidReplayBytes(state);
	state.Owners[0].History.Frames[0].Events.reserve(64);
	state.Owners[0].VisualFrames[0].Mutations[0].Data.Texture->Data.Pixels.reserve(256);
	CHECK(RetainedRigidReplayBytes(state) > before);
	CHECK(
		ValidateRigidReplay(state, RetainedRigidReplayBytes(state) - 1, diagnostic) == Status::LimitExceeded
	);
}
TEST_CASE("Rigid journal rejects invalid event aliases and source controls", "[imagegraph][rigid]") {
	auto state = RecordedOwner();
	Diagnostic diagnostic;
	state.Owners[0].History.Frames[0].Events.push_back(state.Owners[0].History.Frames[0].Events.front());
	CHECK(ValidateRigidReplay(state, Limits::MaximumEvaluationBytes, diagnostic) == Status::DuplicateId);
	state = RecordedOwner();
	std::get<SourceRigidBody>(state.Owners[0].History.Frames[0].Events.front().Command).Position.X =
		std::numeric_limits<double>::quiet_NaN();
	CHECK(ValidateRigidReplay(state, Limits::MaximumEvaluationBytes, diagnostic) == Status::InvalidValue);
	state = RecordedOwner();
	state.Owners[0].VisualFrames[0].Mutations[0].Position.ConsumerId = "absent";
	CHECK(ValidateRigidReplay(state, Limits::MaximumEvaluationBytes, diagnostic) == Status::InvalidValue);
	state = RecordedOwner();
	state.Owners.push_back(state.Owners[0]);
	CHECK(ValidateRigidReplay(state, Limits::MaximumEvaluationBytes, diagnostic) == Status::DuplicateId);
}

TEST_CASE(
	"Empty rigid collider preserves identity while refusing physical shape controls", "[imagegraph][rigid]"
) {
	auto state = RecordedOwner();
	auto &body = std::get<SourceRigidBody>(state.Owners[0].History.Frames[0].Events[0].Command);
	body.Shape = SourceRigidShape::Empty;
	body.Dynamic = false;
	body.Sensor = false;
	body.Points.clear();
	Diagnostic diagnostic;
	REQUIRE(ValidateRigidReplay(state, Limits::MaximumEvaluationBytes, diagnostic) == Status::Ok);
	const auto valid = state;
	body.Dynamic = true;
	CHECK(ValidateRigidReplay(state, Limits::MaximumEvaluationBytes, diagnostic) == Status::InvalidValue);
	state = valid;
	std::get<SourceRigidBody>(state.Owners[0].History.Frames[0].Events[0].Command).Sensor = true;
	CHECK(ValidateRigidReplay(state, Limits::MaximumEvaluationBytes, diagnostic) == Status::InvalidValue);
	state = valid;
	std::get<SourceRigidBody>(state.Owners[0].History.Frames[0].Events[0].Command).Points = {{0, 0}};
	CHECK(ValidateRigidReplay(state, Limits::MaximumEvaluationBytes, diagnostic) == Status::InvalidValue);
	state = valid;
	std::get<SourceRigidBody>(state.Owners[0].History.Frames[0].Events[0].Command).Shape =
		static_cast<SourceRigidShape>(255);
	CHECK(ValidateRigidReplay(state, Limits::MaximumEvaluationBytes, diagnostic) == Status::InvalidValue);
	CHECK(valid.Owners[0].History.Frames[0].Events[0].Position.ConsumerId == "body");
}
