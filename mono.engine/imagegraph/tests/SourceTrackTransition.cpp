#include <engine/imagegraph/GroupReplay.hpp>
#include <engine/imagegraph/SourceTrackTransition.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>

TEST_SUITE_ID("engine.imagegraph.source_track_transition")
namespace {
	using namespace engine::imagegraph;
	Document TrackWriters(bool retired = true, bool quaternion = false) {
		Document document;
		document.FormatVersion = 10;
		const std::string input = quaternion ? "rotation" : "mix";
		const std::string type = quaternion ? "pc.quarternion_to_euler" : "pc.invert";
		const Value first = quaternion ? Value{Quaternion{0, 0, 0, 0}} : Value{.25};
		const Value second = quaternion ? Value{Quaternion{90, 0, 0, 0}} : Value{.75};
		document.SourceAnimators.emplace();
		for (const char *id : {"owner", "alias", "sibling"}) {
			Node node{id, type, {}, {}, {{input, first}}};
			node.SourceAnimatedInputs = {input};
			if (node.Id != "owner") {
				node.InstanceBase = "owner";
				node.InstanceOverrides = {input};
			}
			document.Nodes.push_back(node);
			document.Tracks.push_back({id, input, "hold", -1});
		}
		const std::string physical = retired ? "native:animator:0" : input;
		std::vector<Keyframe> canonical{
			{"owner", physical, 0, first, "linear"}, {"owner", physical, 4, second, "linear"}
		};
		canonical[0].SourceKeyId = "first-key";
		canonical[1].SourceKeyId = "second-key";
		for (const char *id : {"alias", "sibling"}) {
			GroupSubtypeBinding binding{
				id, "owner", GroupSubtypeAnimator::Animated, GroupSubtypeAnimator::Animated, input
			};
			if (retired) binding.AnimatorPort = physical;
			document.SourceAnimators->Bindings.push_back(binding);
			for (auto key : canonical) {
				key.NodeId = id;
				key.Port = input;
				key.SourceKeyId.clear();
				document.Keyframes.push_back(key);
			}
		}
		if (retired) {
			DetachedSourceAnimator metadata;
			metadata.Id = physical;
			metadata.OwnerId = "owner";
			metadata.OriginalPort = input;
			metadata.Writer = GroupSubtypeAnimator::Animated;
			metadata.Type = quaternion ? ValueType::Quaternion : ValueType::Scalar;
			metadata.Track = AnimationTrack{"owner", physical, "hold", -1};
			document.SourceAnimators->Detached = {metadata};
			document.SourceAnimators->DetachedValues = {{"owner", std::nullopt, canonical, physical}};
			document.Keyframes.push_back({"owner", input, 0, first, "linear"});
		} else
			document.Keyframes.insert(document.Keyframes.end(), canonical.begin(), canonical.end());
		document.Outputs = {{"result", "alias", quaternion ? "euler_angles" : "surface_out"}};
		return document;
	}
	void TrackSaved(const Document &document) {
		Diagnostic diagnostic;
		Document restored;
		REQUIRE(Read(Write(document), restored, diagnostic) == Status::Ok);
		CHECK(restored == document);
		GroupReplayState replay;
		REQUIRE(RestoreSourceAnimatorBindings(restored, {}, 2, replay, diagnostic) == Status::Ok);
		Document projected;
		REQUIRE(ProjectGroupReplay(restored, replay, 2, projected, diagnostic) == Status::Ok);
		CHECK(projected.SourceAnimators == document.SourceAnimators);
	}
}
TEST_CASE(
	"Captured track policy edits share a generation through save and restore", "[imagegraph][source_tracks]"
) {
	for (bool retired : {false, true}) {
		const auto document = TrackWriters(retired);
		AnimationTrack replacement{"alias", "mix", "ping", 0};
		SourceTrackTransition transition{"alias", "mix", &replacement};
		Document result;
		Diagnostic diagnostic;
		const auto status = ApplySourceTrackTransition(document, transition, result, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(status == Status::Ok);
		CHECK(result.Nodes == document.Nodes);
		CHECK(result.Keyframes == document.Keyframes);
		CHECK(result.SourceAnimators->Bindings == document.SourceAnimators->Bindings);
		for (const auto &track : result.Tracks) {
			if (retired && track.NodeId == "owner")
				CHECK(track.End == "hold");
			else {
				CHECK(track.End == "ping");
				CHECK(track.LoopRange == 0);
			}
		}
		if (retired) {
			const auto &metadata = result.SourceAnimators->Detached[0];
			CHECK(metadata.Id == "native:animator:0");
			CHECK(metadata.Track->End == "ping");
			CHECK(metadata.Track->Port == metadata.Id);
			CHECK(metadata.Track->LoopRange == 0);
		}
		TrackSaved(result);
		transition.Replacement = nullptr;
		REQUIRE(ApplySourceTrackTransition(result, transition, result, diagnostic) == Status::Ok);
		CHECK(result.Tracks.size() == size_t(retired));
		if (retired) CHECK_FALSE(result.SourceAnimators->Detached[0].Track);
		TrackSaved(result);
	}
}
TEST_CASE(
	"Captured quaternion policy normalizes combined keys and drives restored sampling",
	"[imagegraph][source_tracks]"
) {
	for (bool retired : {false, true}) {
		INFO("retired=" << retired);
		const auto document = TrackWriters(retired, true);
		AnimationTrack replacement{"alias", "rotation", "hold", -1};
		replacement.QuaternionMode = 1;
		Document result;
		Diagnostic diagnostic;
		REQUIRE(
			ApplySourceTrackTransition(
				document, {"alias", "rotation", &replacement, true}, result, diagnostic
			) == Status::Ok
		);
		for (const auto &key : result.Keyframes)
			if (!retired || key.NodeId != "owner") {
				CHECK(key.Interpolation == "source");
				CHECK(key.Ease == KeyframeEase{});
			}
		if (retired) {
			CHECK(result.SourceAnimators->Detached[0].Track->QuaternionMode == 1);
			CHECK(result.SourceAnimators->DetachedValues[0].Keys[0].SourceKeyId == "first-key");
		}
		TrackSaved(result);
		Document restored;
		REQUIRE(Read(Write(result), restored, diagnostic) == Status::Ok);
		Plan plan;
		REQUIRE(Compile(restored, plan, diagnostic) == Status::Ok);
		GroupReplayState replay;
		REQUIRE(RestoreSourceAnimatorBindings(restored, {}, 4, replay, diagnostic) == Status::Ok);
		EvaluationRequest request;
		request.Tick = 2;
		request.GroupReplay = &replay;
		request.GroupAuthoringRevision = 4;
		EvaluatedValue sampled;
		const auto status = EvaluateValue(restored, plan, "result", request, sampled, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(status == Status::Ok);
		const auto angles = std::get<Vector3>(sampled.Data);
		CHECK(angles.X == Catch::Approx(-45));
		CHECK(angles.Y == Catch::Approx(0).margin(1e-10));
		CHECK(angles.Z == Catch::Approx(0).margin(1e-10));
		if (retired) {
			// Inherited getters follow the current property, independently of the captured writer.
			for (auto &node : restored.Nodes)
				if (node.Id == "alias") node.InstanceOverrides.clear();
			REQUIRE(Compile(restored, plan, diagnostic) == Status::Ok);
			GroupReplayState inheritedReplay;
			REQUIRE(
				RestoreSourceAnimatorBindings(restored, {}, 5, inheritedReplay, diagnostic) == Status::Ok
			);
			request.GroupReplay = &inheritedReplay;
			request.GroupAuthoringRevision = 5;
			REQUIRE(EvaluateValue(restored, plan, "result", request, sampled, diagnostic) == Status::Ok);
			CHECK(std::get<Vector3>(sampled.Data).X == Catch::Approx(0).margin(1e-10));
		}
	}
}
TEST_CASE(
	"Invalid captured track settings and byte bounds preserve publication", "[imagegraph][source_tracks]"
) {
	const auto document = TrackWriters();
	Document result = document;
	Diagnostic diagnostic;
	AnimationTrack replacement{"alias", "mix", "bad", -1};
	SourceTrackTransition transition{"alias", "mix", &replacement};
	CHECK(ApplySourceTrackTransition(document, transition, result, diagnostic) == Status::InvalidValue);
	CHECK(result == document);
	replacement.End = "hold";
	CHECK(ApplySourceTrackTransition(document, transition, result, diagnostic, 1) == Status::LimitExceeded);
	CHECK(result == document);
	replacement.End = "wrap";
	CHECK(ApplySourceTrackTransition(document, transition, result, diagnostic) != Status::Ok);
	CHECK(result == document);
	replacement.End = "hold";
	replacement.QuaternionMode = 1;
	CHECK(ApplySourceTrackTransition(document, transition, result, diagnostic) != Status::Ok);
	CHECK(result == document);
	transition.NodeId = replacement.NodeId = "absent";
	CHECK(ApplySourceTrackTransition(document, transition, result, diagnostic) == Status::UnknownNode);
	CHECK(result == document);
}
