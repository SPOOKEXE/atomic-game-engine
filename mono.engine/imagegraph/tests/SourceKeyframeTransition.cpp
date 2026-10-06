#include <engine/imagegraph/GroupReplay.hpp>
#include <engine/imagegraph/SourceKeyframeTransition.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

TEST_SUITE_ID("engine.imagegraph.source_keyframe_transition")
namespace {
	using namespace engine::imagegraph;
	Document Shared(bool detached = false) {
		Document document;
		document.FormatVersion = 10;
		document.Nodes = {
			{"owner", "pc.invert", {}, {}, {{"mix", .25}}},
			{"alias", "pc.invert", {}, {}, {{"mix", .25}}},
			{"sibling", "pc.invert", {}, {}, {{"mix", .25}}}
		};
		for (auto &node : document.Nodes) {
			node.SourceAnimatedInputs = {"mix"};
			if (node.Id != "owner") node.InstanceBase = "owner";
		}
		Keyframe first{"owner", "mix", 1, .25, "source", KeyframeEase{}};
		first.SourceKeyId = "original-key";
		first.SourceDriver = KeyframeLinearDriver{.125};
		first.Kind = KeyframeKind::Adder;
		Keyframe second{"owner", "mix", 5, .75, "source", KeyframeEase{}};
		second.SourceKeyId = "second-key";
		document.SourceAnimators.emplace();
		for (const char *id : {"alias", "sibling"}) {
			GroupSubtypeBinding binding{
				id, "owner", GroupSubtypeAnimator::Animated, GroupSubtypeAnimator::Animated, "mix"
			};
			if (detached) binding.AnimatorPort = "native:animator:0";
			document.SourceAnimators->Bindings.push_back(binding);
			for (auto key : {first, second}) {
				key.NodeId = id;
				key.SourceKeyId.clear();
				document.Keyframes.push_back(key);
			}
			document.Tracks.push_back({id, "mix", "hold", -1});
		}
		if (detached) {
			DetachedSourceAnimator metadata;
			metadata.Id = "native:animator:0";
			metadata.OwnerId = "owner";
			metadata.OriginalPort = "mix";
			metadata.Writer = GroupSubtypeAnimator::Animated;
			metadata.Type = ValueType::Scalar;
			metadata.Track = AnimationTrack{"owner", metadata.Id, "hold", -1};
			document.SourceAnimators->Detached.push_back(metadata);
			first.Port = metadata.Id;
			second.Port = metadata.Id;
			document.SourceAnimators->DetachedValues.push_back(
				{"owner", std::nullopt, {first, second}, metadata.Id}
			);
			document.Keyframes.push_back({"owner", "mix", 0, .9, "source", KeyframeEase{}});
		} else {
			document.Keyframes.push_back(first);
			document.Keyframes.push_back(second);
		}
		document.Tracks.push_back({"owner", "mix", "hold", -1});
		document.Outputs = {{"result", "owner", "surface_out"}};
		return document;
	}
	const Keyframe &Key(const Document &doc, std::string_view node, FrameTime time) {
		const auto found = std::find_if(doc.Keyframes.begin(), doc.Keyframes.end(), [&](const auto &key) {
			return key.NodeId == node && GetFrameTime(key) == time;
		});
		REQUIRE(found != doc.Keyframes.end());
		return *found;
	}
	void Valid(const Document &doc) {
		Diagnostic diagnostic;
		Plan plan;
		const auto compiled = Compile(doc, plan, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(compiled == Status::Ok);
		Document restored;
		REQUIRE(Read(Write(doc), restored, diagnostic) == Status::Ok);
		CHECK(restored == doc);
		GroupReplayState replay;
		REQUIRE(RestoreSourceAnimatorBindings(restored, {}, 1, replay, diagnostic) == Status::Ok);
	}
}
TEST_CASE(
	"Captured combined key moves update every alias and preserve generations", "[imagegraph][source_keys]"
) {
	for (bool detached : {false, true}) {
		auto document = Shared(detached);
		Valid(document);
		const auto bindings = document.SourceAnimators->Bindings;
		const auto metadata = document.SourceAnimators->Detached;
		const auto ownerCurrent = detached ? std::optional{document.Keyframes.back()} : std::nullopt;
		const auto original = Key(document, "alias", {1});
		auto replacement = original;
		REQUIRE(SetFrameTime(replacement, {2, .5, true}));
		const SourceKeyframeEdit edit{&original, &replacement};
		Document result;
		Diagnostic diagnostic;
		const auto status = ApplySourceKeyframeEdits(document, {&edit, 1}, result, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(status == Status::Ok);
		CHECK(result.SourceAnimators->Bindings == bindings);
		CHECK(result.SourceAnimators->Detached == metadata);
		CHECK(Key(result, "alias", {2, .5, true}).Data == original.Data);
		CHECK(Key(result, "sibling", {2, .5, true}).SourceDriver == original.SourceDriver);
		const auto &physical = detached ? result.SourceAnimators->DetachedValues[0].Keys.front()
										: Key(result, "owner", {2, .5, true});
		CHECK(physical.SourceKeyId == "original-key");
		CHECK(physical.Kind == KeyframeKind::Adder);
		if (ownerCurrent) CHECK(Key(result, "owner", {}).Data == ownerCurrent->Data);
		Valid(result);
	}
}
TEST_CASE("Shared key batch swaps, collisions, copies and deletion are atomic", "[imagegraph][source_keys]") {
	for (bool detached : {false, true}) {
		const auto document = Shared(detached);
		const auto first = Key(document, "alias", {1});
		const auto second = Key(document, "sibling", {5});
		auto movedFirst = first, movedSecond = second;
		REQUIRE(SetFrameTime(movedFirst, {5}));
		REQUIRE(SetFrameTime(movedSecond, {1}));
		SourceKeyframeEdit edits[] = {{&first, &movedFirst}, {&second, &movedSecond}};
		Document result;
		Diagnostic diagnostic;
		REQUIRE(ApplySourceKeyframeEdits(document, edits, result, diagnostic) == Status::Ok);
		CHECK(Key(result, "sibling", {5}).Data == first.Data);
		CHECK(Key(result, "alias", {1}).Data == second.Data);
		Valid(result);
		REQUIRE(SetFrameTime(movedSecond, {5}));
		REQUIRE(ApplySourceKeyframeEdits(document, edits, result, diagnostic) == Status::Ok);
		CHECK(Key(result, "alias", {5}).Data == first.Data);
		CHECK(std::count_if(result.Keyframes.begin(), result.Keyframes.end(), [](const auto &key) {
				  return key.NodeId == "alias";
			  }) == 1);
		Valid(result);
		const SourceKeyframeEdit copy{&first, &movedFirst, true};
		REQUIRE(ApplySourceKeyframeEdits(document, {&copy, 1}, result, diagnostic) == Status::Ok);
		CHECK_FALSE(Key(result, "sibling", {5}).SourceDriver);
		CHECK(Key(result, "sibling", {5}).SourceKeyId.empty());
		Valid(result);
		edits[0].Replacement = nullptr;
		edits[1].Replacement = nullptr;
		REQUIRE(ApplySourceKeyframeEdits(document, edits, result, diagnostic) == Status::Ok);
		CHECK(std::none_of(result.Keyframes.begin(), result.Keyframes.end(), [](const auto &key) {
			return key.NodeId == "alias" || key.NodeId == "sibling";
		}));
		if (detached) CHECK(result.SourceAnimators->DetachedValues[0].Keys.empty());
		Valid(result);
	}
}
TEST_CASE("Repeated alias pins agree and failed key edits preserve the output", "[imagegraph][source_keys]") {
	const auto document = Shared(true);
	const auto original = Key(document, "alias", {1}), other = Key(document, "sibling", {1});
	auto replacement = original, otherReplacement = other;
	REQUIRE(SetFrameTime(replacement, {3}));
	REQUIRE(SetFrameTime(otherReplacement, {3}));
	SourceKeyframeEdit edits[] = {{&original, &replacement}, {&other, &otherReplacement}};
	Document result;
	Diagnostic diagnostic;
	REQUIRE(ApplySourceKeyframeEdits(document, edits, result, diagnostic) == Status::Ok);
	CHECK(result.SourceAnimators->DetachedValues[0].Keys.size() == 2);
	const auto accepted = result;
	REQUIRE(SetFrameTime(otherReplacement, {4}));
	CHECK(ApplySourceKeyframeEdits(document, edits, result, diagnostic) == Status::InvalidValue);
	CHECK(result == accepted);
	CHECK(ApplySourceKeyframeEdits(document, {edits, 1}, result, diagnostic, 1) == Status::LimitExceeded);
	CHECK(result == accepted);
	auto stale = original;
	stale.Data = .333;
	edits[0].Original = &stale;
	CHECK(ApplySourceKeyframeEdits(document, {edits, 1}, result, diagnostic) == Status::InvalidValue);
	CHECK(result == accepted);
	edits[0].Original = &original;
	replacement.Port = "unrelated";
	CHECK(ApplySourceKeyframeEdits(document, {edits, 1}, result, diagnostic) == Status::InvalidValue);
	CHECK(result == accepted);
}
TEST_CASE("Source copies validate socket types and retained borrowed capacity", "[imagegraph][source_keys]") {
	const auto document = Shared(true);
	auto original = Key(document, "alias", {1}), replacement = original;
	SourceKeyframeEdit edit{&original, &replacement, true};
	Document result = document;
	Diagnostic diagnostic;
	replacement.Data = std::string{"wrong scalar type"};
	CHECK(ApplySourceKeyframeEdits(document, {&edit, 1}, result, diagnostic) != Status::Ok);
	CHECK(result == document);
	replacement = original;
	original.NodeId = replacement.NodeId = "absent";
	CHECK(ApplySourceKeyframeEdits(document, {&edit, 1}, result, diagnostic) != Status::Ok);
	CHECK(result == document);
	original = Key(document, "alias", {1});
	ArrayValue array{ValueType::Scalar, {1.0}};
	array.Elements.reserve(Limits::MaximumArrayElements);
	original.Data = std::move(array);
	replacement = original;
	const uint64_t cap = *DocumentRetainedPayloadBytes(document) * 4 + 8192;
	CHECK(ApplySourceKeyframeEdits(document, {&edit, 1}, result, diagnostic, cap) == Status::LimitExceeded);
	CHECK(result == document);
}
