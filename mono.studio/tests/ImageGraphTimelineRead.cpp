#include "../src/ImageGraphTimelineRead.hpp"

#include "../src/ImageGraphSourceKeyEdit.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>

TEST_SUITE_ID("studio.imagegraph.timeline_read")
TEST_DEPENDS("engine.imagegraph.source_keyframe_transition")
TEST_DEPENDS("studio.imagegraph.group_host")

using namespace engine::imagegraph;

namespace {
	Keyframe Axis(std::string node, std::string id, uint64_t tick, double value) {
		Keyframe key{std::move(node), "center", tick, value, "source", KeyframeEase{}};
		key.SourceKeyId = std::move(id);
		return key;
	}
	Document MirrorWithAlias() {
		Document document;
		document.FormatVersion = 9;
		Node mirror{"mirror", "pc.mirror_polar", {}, {}, {{"center", Vector2{.9, .9}}}};
		mirror.SourceAnimatedInputs = {"center"};
		SourceSeparatedVec2Animator axes;
		axes.Port = "center";
		axes.Axes[0].Keys = {Axis("mirror", "x0", 0, .25), Axis("mirror", "x5", 5, .75)};
		axes.Axes[1].Keys = {Axis("mirror", "y0", 0, 2), Axis("mirror", "y5", 5, 4)};
		mirror.SourceSeparatedVec2Animators.emplace().Inputs.push_back(std::move(axes));
		Node alias = mirror;
		alias.Id = "alias";
		alias.InstanceBase = "mirror";
		alias.SourceSeparatedVec2Animators = {};
		document.Nodes = {
			{"source",
			 "image.solid",
			 {},
			 {},
			 {{"width", int64_t{1}}, {"height", int64_t{1}}, {"colour", Colour{}}}},
			{"alias_source",
			 "image.solid",
			 {},
			 {},
			 {{"width", int64_t{1}}, {"height", int64_t{1}}, {"colour", Colour{}}}},
			std::move(mirror),
			std::move(alias)
		};
		document.Links = {
			{"source", "image", "mirror", "surface_in"}, {"alias_source", "image", "alias", "surface_in"}
		};
		document.Outputs = {{"out", "mirror", "surface_out"}};
		document.Keyframes = {
			{"mirror", "center", 0, Vector2{.1, .2}, "source", KeyframeEase{}},
			{"mirror", "center", 5, Vector2{.9, .8}, "source", KeyframeEase{}}
		};
		document.Tracks = {{"mirror", "center", "hold", -1}};
		return document;
	}
	Document ColdGetter() {
		Document document;
		document.FormatVersion = 9;
		Node gradient{
			"gradient",
			"pc.gradient_points_n",
			{},
			{},
			{{"dimension", Vector2{32, 32}}, {"dimension_unit", EnumValue{0}}, {"blend_mode", EnumValue{0}}}
		};
		gradient.DynamicInputs = {
			{"point_i_0", ValueType::Vector2, Value{Vector2{99, 88}}},
			{"point_i_1", ValueType::Vector2, Value{Vector2{30, 3}}}
		};
		gradient.SourceStaticInputs = {"point_i_0"};
		gradient.SourceVec2Defaults.emplace().Inputs = {{"point_i_0", Vector2{7, 8}}};
		gradient.SourceSeparatedVec2Animators.emplace().Inputs = {
			SourceSeparatedVec2Animator{"point_i_0", {}, true, false}
		};
		document.Nodes.push_back(std::move(gradient));
		document.Outputs = {{"out", "gradient", "surface_out"}};
		return document;
	}
}

TEST_CASE("Timeline read snapshots are cached and observe same-revision replay publications", "[studio]") {
	auto authored = MirrorWithAlias();
	const auto before = authored;
	studio::ImageGraphGroupHost host;
	studio::ImageGraphTimelineRead read;
	EvaluationRequest request;
	Diagnostic error;
	REQUIRE(studio::PrepareImageGraphTimelineRead(authored, host, 1, request, read, error));
	INFO(error.Message << " node=" << error.NodeId << " port=" << error.Port);
	CHECK(authored == before);
	REQUIRE(read.Snapshot);
	CHECK(read.Matches(host, 1));
	CHECK(host.Replay.InstancesBound());
	const auto display = read.DisplayRevision;
	const auto *snapshot = &*read.Snapshot;
	const SourceKeyframeIdentity aliasPin{"alias", "center", {0}, 0};
	std::vector<Keyframe> pins;
	REQUIRE(CaptureSourceKeyframes(*read.Snapshot, {&aliasPin, 1}, pins, error) == Status::Ok);
	REQUIRE(pins.size() == 1);
	CHECK(pins.front().NodeId == "alias");
	CHECK(pins.front().Data == Value{.25});

	REQUIRE(studio::PrepareImageGraphTimelineRead(authored, host, 1, request, read, error));
	CHECK(&*read.Snapshot == snapshot);
	CHECK(read.DisplayRevision == display);

	const auto snapshotRevision = read.ReplayRevision;
	const auto priorSnapshot = *read.Snapshot;
	const auto priorHostRevision = host.Replay.ObservationRevision();
	CHECK_FALSE(read.Refresh(authored, host, 1, error, 1));
	CHECK(read.Snapshot == priorSnapshot);
	CHECK(read.ReplayRevision == snapshotRevision);
	CHECK(read.DisplayRevision == display);
	CHECK(host.Replay.ObservationRevision() == priorHostRevision);

	GroupReplayState republished;
	REQUIRE(RebindGroupReplay(authored, host.Replay, 1, republished, error) == Status::Ok);
	host.Replay = std::move(republished);
	CHECK(host.Replay.AuthoringRevision() == 1);
	CHECK(host.Replay.ObservationRevision() != priorHostRevision);
	CHECK_FALSE(read.Matches(host, 1));
	REQUIRE(studio::PrepareImageGraphTimelineRead(authored, host, 1, request, read, error));
	CHECK(read.Matches(host, 1));
	CHECK(read.DisplayRevision > display);
}

TEST_CASE("Timeline source pin copy move and delete publish one undoable edit", "[studio]") {
	auto authored = MirrorWithAlias();
	studio::ImageGraphGroupHost host;
	studio::ImageGraphTimelineRead read;
	EvaluationRequest request;
	Diagnostic error;
	REQUIRE(studio::PrepareImageGraphTimelineRead(authored, host, 1, request, read, error));
	REQUIRE(read.Snapshot);
	const Document baseline = *read.Snapshot;
	const SourceKeyframeIdentity identities[] = {
		{"alias", "center", {0}, 0}, {"alias", "center", {5}, 0}, {"alias", "center", {0}, 1}
	};
	std::vector<Keyframe> pins;
	REQUIRE(CaptureSourceKeyframes(baseline, identities, pins, error) == Status::Ok);
	REQUIRE(pins.size() == 3);
	auto copied = pins[0];
	auto moved = pins[1];
	REQUIRE(SetFrameTime(copied, {2}));
	REQUIRE(SetFrameTime(moved, {7}));
	const std::array<int8_t, 3> axes{0, 0, 1};
	studio::ImageGraphHistory history;
	const bool edited = studio::ApplyImageGraphSourceKeyEdit(
		authored,
		history,
		host,
		1,
		request,
		[&](Document &candidate, uint64_t available) {
			return studio::WithImageGraphProjectedKeyPins(
				baseline,
				candidate,
				pins,
				available,
				[&](std::span<const Keyframe> mapped, uint64_t remaining) {
					const SourceKeyframeEdit edits[] = {
						{&mapped[0], &copied, true, 0},
						{&mapped[1], &moved, false, 0},
						{&mapped[2], nullptr, false, 1}
					};
					Document changed;
					if (ApplySourceKeyframeEdits(candidate, edits, changed, error, remaining) != Status::Ok)
						return false;
					candidate = std::move(changed);
					return true;
				},
				error,
				axes
			);
		},
		error
	);
	INFO(error.Message << " node=" << error.NodeId << " port=" << error.Port);
	REQUIRE(edited);
	CHECK(history.CanUndo());
	CHECK(host.Revision == 2);
	CHECK(authored != baseline);
	std::vector<Keyframe> resultingPins;
	const SourceKeyframeIdentity resultIdentities[] = {
		{"alias", "center", {0}, 0}, {"alias", "center", {2}, 0}, {"alias", "center", {7}, 0}
	};
	REQUIRE(CaptureSourceKeyframes(authored, resultIdentities, resultingPins, error) == Status::Ok);
	CHECK(resultingPins.size() == 3);
	CHECK(resultingPins[0].Data == pins[0].Data);
	CHECK(resultingPins[1].SourceKeyId.empty());
	const SourceKeyframeIdentity movedOwner{"mirror", "center", {7}, 0};
	std::vector<Keyframe> physicalMove;
	REQUIRE(CaptureSourceKeyframes(authored, {&movedOwner, 1}, physicalMove, error) == Status::Ok);
	REQUIRE(physicalMove.size() == 1);
	CHECK(physicalMove.front().SourceKeyId == "x5");
	const SourceKeyframeIdentity deletedY{"alias", "center", {0}, 1};
	const std::vector<Keyframe> previousPin{pins[2]};
	std::vector<Keyframe> deletedPin = previousPin;
	CHECK(CaptureSourceKeyframes(authored, {&deletedY, 1}, deletedPin, error) == Status::InvalidValue);
	CHECK(deletedPin == previousPin);

	const auto editedDocument = authored;
	REQUIRE(history.Undo(authored));
	CHECK(authored == baseline);
	Document reopened;
	REQUIRE(Read(Write(authored), reopened, error) == Status::Ok);
	CHECK(reopened == baseline);
	REQUIRE(history.Redo(authored));
	CHECK(authored == editedDocument);
}

TEST_CASE("Timeline read refreshes after a cold getter receipt at the same revision", "[studio]") {
	auto authored = ColdGetter();
	const auto before = authored;
	studio::ImageGraphGroupHost host;
	studio::ImageGraphTimelineRead read;
	EvaluationRequest request;
	Diagnostic error;
	REQUIRE(studio::PrepareImageGraphTimelineRead(authored, host, 1, request, read, error));
	REQUIRE(read.Snapshot);
	REQUIRE(read.Snapshot->Nodes.front().SourceSeparatedVec2Animators);
	CHECK_FALSE(read.Snapshot->Nodes.front().SourceSeparatedVec2Animators->Inputs.front().Initialized);
	CHECK(authored == before);
	const auto oldDisplayRevision = read.DisplayRevision;
	const auto oldReplayRevision = read.ReplayRevision;
	request.GroupReplay = &host.Replay;
	request.GroupAuthoringRevision = 1;
	Plan plan;
	REQUIRE(Compile(authored, plan, error) == Status::Ok);
	EvaluationSnapshot inputs;
	REQUIRE(
		EvaluateNodeInputs(authored, plan, "gradient", request, inputs, error) ==
		Status::SourceAxisInitializationRequired
	);
	CHECK(error.NodeId == "gradient");
	CHECK(error.Port == "point_i_0");
	const auto receipt = error;
	REQUIRE(host.RetainAxisRead(authored, 1, request, receipt, error));
	CHECK(host.Replay.AuthoringRevision() == 1);
	CHECK(host.Replay.ObservationRevision() != oldReplayRevision);
	CHECK_FALSE(read.Matches(host, 1));
	CHECK(authored == before);
	REQUIRE(studio::PrepareImageGraphTimelineRead(authored, host, 1, request, read, error));
	CHECK(read.Matches(host, 1));
	CHECK(read.DisplayRevision > oldDisplayRevision);
	REQUIRE(read.Snapshot);
	const auto &initialized = read.Snapshot->Nodes.front().SourceSeparatedVec2Animators->Inputs.front();
	CHECK(initialized.Initialized);
	REQUIRE(initialized.Axes[0].Keys.size() == 1);
	REQUIRE(initialized.Axes[1].Keys.size() == 1);
	CHECK(initialized.Axes[0].Keys.front().Data == Value{7.0});
	CHECK(initialized.Axes[1].Keys.front().Data == Value{8.0});
}

TEST_CASE("Timeline prepares an empty host at authoring revision zero", "[studio]") {
	const auto authored = MirrorWithAlias();
	studio::ImageGraphGroupHost host;
	studio::ImageGraphTimelineRead read;
	Diagnostic error;
	REQUIRE(studio::PrepareImageGraphTimelineRead(authored, host, 0, {}, read, error));
	CHECK(read.Matches(host, 0));
	const SourceKeyframeIdentity identity{"alias", "center", {0}, 0};
	std::vector<Keyframe> pins;
	REQUIRE(CaptureSourceKeyframes(*read.Snapshot, {&identity, 1}, pins, error) == Status::Ok);
	REQUIRE(pins.size() == 1);
	CHECK(pins.front().Data == Value{.25});
}
