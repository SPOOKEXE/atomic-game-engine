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
	Keyframe AxisScalar(std::string_view node, int axis, uint64_t tick, double value, std::string id) {
		Keyframe key{std::string(node), "center", tick, value, "source", KeyframeEase{}};
		key.SourceKeyId = std::move(id);
		if (axis == 0 && tick == 0) key.SourceDriver = KeyframeLinearDriver{.125};
		return key;
	}
	Document SharedAxes() {
		Document document;
		document.FormatVersion = 10;
		document.Nodes = {
			{"owner", "pc.mirror_polar", {}, {}, {{"center", Vector2{.2, .3}}}},
			{"alias", "pc.mirror_polar", {}, {}, {{"center", Vector2{.2, .3}}}},
			{"sibling", "pc.mirror_polar", {}, {}, {{"center", Vector2{.2, .3}}}}
		};
		std::array<std::vector<Keyframe>, 2> physical;
		physical[0] = {AxisScalar("owner", 0, 0, .25, "x0"), AxisScalar("owner", 0, 5, .75, "x5")};
		physical[1] = {AxisScalar("owner", 1, 0, 2, "y0"), AxisScalar("owner", 1, 5, 4, "y5")};
		for (auto &node : document.Nodes) {
			node.SourceAnimatedInputs = {"center"};
			if (node.Id != "owner") node.InstanceBase = "owner";
			SourceSeparatedVec2Animator axes;
			axes.Port = "center";
			axes.Separated = node.Id != "alias";
			axes.Initialized = true;
			for (int axis = 0; axis < 2; ++axis) {
				axes.Axes[size_t(axis)].Keys = physical[size_t(axis)];
				if (node.Id != "owner")
					for (auto &key : axes.Axes[size_t(axis)].Keys) {
						key.NodeId = node.Id;
						key.SourceKeyId.clear();
					}
			}
			node.SourceSeparatedVec2Animators.emplace().Inputs.push_back(std::move(axes));
		}
		auto combined0 = Keyframe{"owner", "center", 0, Vector2{.2, .3}, "source", KeyframeEase{}};
		combined0.SourceKeyId = "combined0";
		auto combined5 = Keyframe{"owner", "center", 5, Vector2{.7, .8}, "source", KeyframeEase{}};
		combined5.SourceKeyId = "combined5";
		for (const char *id : {"owner", "alias", "sibling"}) {
			auto first = combined0, second = combined5;
			first.NodeId = second.NodeId = id;
			if (id != std::string_view("owner")) first.SourceKeyId = second.SourceKeyId = {};
			document.Keyframes.push_back(std::move(first));
			document.Keyframes.push_back(std::move(second));
			document.Tracks.push_back({id, "center", "hold", -1});
		}
		document.SourceAnimators.emplace();
		for (const char *id : {"alias", "sibling"}) {
			GroupSubtypeBinding binding{
				id, "owner", GroupSubtypeAnimator::Animated, GroupSubtypeAnimator::Animated, "center"
			};
			binding.Axes = {
				GroupAxisStorage::Shared, "owner", "center", "owner", GroupSubtypeAnimator::Animated
			};
			document.SourceAnimators->Bindings.push_back(std::move(binding));
		}
		document.Outputs = {{"result", "owner", "surface_out"}};
		return document;
	}
	const Keyframe *FindAxisKey(const Document &document, std::string_view node, int axis, FrameTime time) {
		const auto foundNode =
			std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const auto &item) {
				return item.Id == node;
			});
		if (foundNode == document.Nodes.end() || !foundNode->SourceSeparatedVec2Animators) return nullptr;
		const auto input = std::find_if(
			foundNode->SourceSeparatedVec2Animators->Inputs.begin(),
			foundNode->SourceSeparatedVec2Animators->Inputs.end(),
			[](const auto &item) { return item.Port == "center"; }
		);
		if (input == foundNode->SourceSeparatedVec2Animators->Inputs.end()) return nullptr;
		const auto &keys = input->Axes[size_t(axis)].Keys;
		const auto found = std::find_if(keys.begin(), keys.end(), [&](const auto &key) {
			return GetFrameTime(key) == time;
		});
		return found == keys.end() ? nullptr : &*found;
	}
	const Keyframe &AxisKey(const Document &document, std::string_view node, int axis, FrameTime time) {
		const auto *key = FindAxisKey(document, node, axis, time);
		REQUIRE(key);
		return *key;
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

TEST_CASE("Scalar axis moves fan out while combined source state stays fixed", "[imagegraph][source_keys]") {
	auto document = SharedAxes();
	Valid(document);
	const auto original = AxisKey(document, "alias", 0, {0});
	auto replacement = original;
	REQUIRE(SetFrameTime(replacement, {2, .5, true}));
	const SourceKeyframeEdit edit{&original, &replacement, false, 0};
	Document result;
	Diagnostic diagnostic;
	REQUIRE(ApplySourceKeyframeEdits(document, {&edit, 1}, result, diagnostic) == Status::Ok);
	CHECK(FindAxisKey(result, "owner", 0, {0}) == nullptr);
	CHECK(AxisKey(result, "owner", 0, {2, .5, true}).SourceKeyId == "x0");
	CHECK(AxisKey(result, "alias", 0, {2, .5, true}).SourceKeyId.empty());
	CHECK(AxisKey(result, "sibling", 0, {2, .5, true}).SourceKeyId.empty());
	CHECK(AxisKey(result, "owner", 1, {0}).Data == Value{double{2}});
	CHECK(AxisKey(result, "alias", 1, {5}).Data == Value{double{4}});
	CHECK(Key(result, "owner", {0}).Data == Value{Vector2{.2, .3}});
	CHECK(Key(result, "alias", {0}).Data == Value{Vector2{.2, .3}});
	CHECK(result.Keyframes == document.Keyframes);
	CHECK(result.Tracks == document.Tracks);
	CHECK(result.SourceAnimators->Bindings == document.SourceAnimators->Bindings);
	for (const auto &node : result.Nodes) {
		const auto &input = node.SourceSeparatedVec2Animators->Inputs.front();
		CHECK(input.Separated == (node.Id != "alias"));
	}
	Valid(result);
}

TEST_CASE("Scalar axis copies and mixed axis edits preserve writer identity", "[imagegraph][source_keys]") {
	const auto document = SharedAxes();
	const auto original = AxisKey(document, "alias", 0, {0});
	auto copied = original;
	REQUIRE(SetFrameTime(copied, {3}));
	const SourceKeyframeEdit copy{&original, &copied, true, 0};
	Document result;
	Diagnostic diagnostic;
	REQUIRE(ApplySourceKeyframeEdits(document, {&copy, 1}, result, diagnostic) == Status::Ok);
	for (const auto node : {"owner", "alias", "sibling"}) {
		const auto &key = AxisKey(result, node, 0, {3});
		CHECK(key.SourceKeyId.empty());
		CHECK_FALSE(key.SourceDriver);
	}
	CHECK(AxisKey(result, "owner", 0, {0}).SourceKeyId == "x0");
	CHECK(
		AxisKey(result, "owner", 0, {0}).SourceDriver ==
		std::optional<KeyframeSourceDriver>{KeyframeLinearDriver{.125}}
	);
	Valid(result);

	const auto x = AxisKey(document, "alias", 0, {0});
	const auto y = AxisKey(document, "sibling", 1, {5});
	const auto combined = Key(document, "alias", {0});
	auto movedX = x, movedY = y, movedCombined = combined;
	REQUIRE(SetFrameTime(movedX, {2, .25, false}));
	REQUIRE(SetFrameTime(movedY, {3, .75, true}));
	REQUIRE(SetFrameTime(movedCombined, {4, .5, false}));
	const SourceKeyframeEdit edits[] = {
		{&x, &movedX, false, 0}, {&y, &movedY, false, 1}, {&combined, &movedCombined}
	};
	REQUIRE(ApplySourceKeyframeEdits(document, edits, result, diagnostic) == Status::Ok);
	CHECK(AxisKey(result, "owner", 0, {2, .25, false}).SourceKeyId == "x0");
	CHECK(AxisKey(result, "alias", 1, {3, .75, true}).SourceKeyId.empty());
	CHECK(Key(result, "owner", {4, .5, false}).SourceKeyId == "combined0");
	CHECK(Key(result, "sibling", {4, .5, false}).SourceKeyId.empty());
	Valid(result);
}

TEST_CASE(
	"Local and cold scalar axis edits use captured storage and fail atomically", "[imagegraph][source_keys]"
) {
	auto local = SharedAxes();
	auto &alias = local.Nodes[1];
	for (size_t axis = 0; axis < 2; ++axis)
		for (auto &key : alias.SourceSeparatedVec2Animators->Inputs.front().Axes[axis].Keys) {
			key.Data = std::get<double>(key.Data) + 10;
			key.SourceKeyId = "local-" + std::to_string(axis) + "-" + std::to_string(key.Tick);
		}
	auto &localBinding = local.SourceAnimators->Bindings.front();
	localBinding.Axes = {GroupAxisStorage::Local, "alias", "center", "owner", GroupSubtypeAnimator::Animated};
	const auto original = AxisKey(local, "alias", 0, {0});
	auto replacement = original;
	replacement.Data = .5;
	REQUIRE(SetFrameTime(replacement, {2}));
	const SourceKeyframeEdit edit{&original, &replacement, false, 0};
	Document result;
	Diagnostic diagnostic;
	REQUIRE(ApplySourceKeyframeEdits(local, {&edit, 1}, result, diagnostic) == Status::Ok);
	CHECK(AxisKey(result, "owner", 0, {0}).Data == Value{double{.25}});
	CHECK(AxisKey(result, "sibling", 0, {0}).Data == Value{double{.25}});
	CHECK(AxisKey(result, "alias", 0, {2}).Data == Value{double{.5}});
	CHECK(result.SourceAnimators->Bindings.front().OwnerId == "owner");
	CHECK(result.SourceAnimators->Bindings.front().Axes.OwnerId == "alias");
	Valid(result);

	auto cold = SharedAxes();
	const auto warmPin = AxisKey(cold, "alias", 0, {0});
	const auto aliasIndex = size_t(1);
	auto &coldInput = cold.Nodes[aliasIndex].SourceSeparatedVec2Animators->Inputs.front();
	coldInput.Initialized = false;
	for (auto &axis : coldInput.Axes)
		axis.Keys.clear();
	cold.SourceAnimators->Bindings.front().Axes = {
		GroupAxisStorage::Uninitialized, "alias", "center", "owner", GroupSubtypeAnimator::Animated
	};
	auto movedPin = warmPin;
	REQUIRE(SetFrameTime(movedPin, {2}));
	const SourceKeyframeEdit coldEdit{&warmPin, &movedPin, false, 0};
	result = SharedAxes();
	const auto prior = result;
	CHECK(
		ApplySourceKeyframeEdits(cold, {&coldEdit, 1}, result, diagnostic) ==
		Status::SourceAxisInitializationRequired
	);
	CHECK(result == prior);
	const auto warm = SharedAxes();
	const auto capPin = AxisKey(warm, "alias", 0, {0});
	auto capMove = capPin;
	REQUIRE(SetFrameTime(capMove, {2}));
	const SourceKeyframeEdit capEdit{&capPin, &capMove, false, 0};
	CHECK(ApplySourceKeyframeEdits(warm, {&capEdit, 1}, result, diagnostic, 1) == Status::LimitExceeded);
	CHECK(result == prior);
}

TEST_CASE(
	"Retired scalar axes keep their captured owner when the property disappears", "[imagegraph][source_keys]"
) {
	auto document = SharedAxes();
	auto retiredAxes = document.Nodes.front().SourceSeparatedVec2Animators->Inputs.front();
	retiredAxes.Port = "native:animator:0";
	for (int axis = 0; axis < 2; ++axis)
		for (auto &key : retiredAxes.Axes[size_t(axis)].Keys)
			key.Port = retiredAxes.Port;
	DetachedSourceAnimator metadata;
	metadata.Id = "native:animator:0";
	metadata.OwnerId = "owner";
	metadata.OriginalPort = "center";
	metadata.Writer = GroupSubtypeAnimator::Animated;
	metadata.Type = ValueType::Vector2;
	metadata.Track = AnimationTrack{"owner", metadata.Id, "hold", -1};
	GroupSubtypeOverlay overlay;
	overlay.NodeId = "owner";
	overlay.Port = metadata.Id;
	overlay.Fixed = Vector2{.1, .2};
	overlay.SeparatedVec2.emplace() = retiredAxes;
	document.SourceAnimators->Detached.push_back(metadata);
	document.SourceAnimators->DetachedValues.push_back(overlay);
	for (auto &binding : document.SourceAnimators->Bindings)
		binding.Axes.Port = metadata.Id;
	auto &currentAxes = document.Nodes.front().SourceSeparatedVec2Animators->Inputs.front();
	for (auto &axis : currentAxes.Axes)
		for (auto &key : axis.Keys) {
			key.Data = std::get<double>(key.Data) + 20;
			key.SourceKeyId.clear();
		}
	Valid(document);
	const auto original = AxisKey(document, "alias", 0, {0});
	auto replacement = original;
	REQUIRE(SetFrameTime(replacement, {2, .25, false}));
	const SourceKeyframeEdit edit{&original, &replacement, false, 0};
	Document result;
	Diagnostic diagnostic;
	REQUIRE(ApplySourceKeyframeEdits(document, {&edit, 1}, result, diagnostic) == Status::Ok);
	const auto &storedAxes = *result.SourceAnimators->DetachedValues.front().SeparatedVec2;
	const auto storedMoved =
		std::find_if(storedAxes.Axes[0].Keys.begin(), storedAxes.Axes[0].Keys.end(), [](const auto &key) {
			return GetFrameTime(key) == FrameTime{2, .25, false};
		});
	REQUIRE(storedMoved != storedAxes.Axes[0].Keys.end());
	CHECK(storedMoved->SourceKeyId == "x0");
	CHECK(AxisKey(result, "alias", 0, {2, .25, false}).SourceKeyId.empty());
	CHECK(AxisKey(result, "sibling", 0, {2, .25, false}).SourceKeyId.empty());
	CHECK(AxisKey(result, "owner", 0, {0}).Data == Value{double{20.25}});
	CHECK(AxisKey(result, "owner", 1, {0}).Data == Value{double{22}});
	CHECK(result.SourceAnimators->Bindings == document.SourceAnimators->Bindings);
	CHECK(result.SourceAnimators->Detached == document.SourceAnimators->Detached);
	CHECK(result.Tracks == document.Tracks);
	Valid(result);
}

TEST_CASE(
	"Keyframe capture preserves requested order and resolves canonical shared axes",
	"[imagegraph][source_keys]"
) {
	const auto document = SharedAxes();
	const SourceKeyframeIdentity requested[] = {
		{"alias", "center", {5}, -1}, {"alias", "center", {5}, 0}, {"owner", "center", {0}, 1}
	};
	std::vector<Keyframe> captured;
	Diagnostic diagnostic;
	REQUIRE(CaptureSourceKeyframes(document, requested, captured, diagnostic) == Status::Ok);
	REQUIRE(captured.size() == 3);
	CHECK(captured[0].Data == Value{Vector2{.7, .8}});
	CHECK(captured[0].NodeId == "alias");
	CHECK(captured[0].SourceKeyId.empty());
	CHECK(captured[1].Data == Value{double{.75}});
	CHECK(captured[1].SourceKeyId.empty());
	CHECK(captured[2].Data == Value{double{2}});
	CHECK(captured[2].SourceKeyId == "y0");

	auto projectionMissing = document;
	projectionMissing.Nodes[1].SourceSeparatedVec2Animators->Inputs.front().Axes[0].Keys.clear();
	REQUIRE(
		CaptureSourceKeyframes(
			projectionMissing, std::span{requested + 1, size_t{1}}, captured, diagnostic
		) == Status::Ok
	);
	REQUIRE(captured.size() == 1);
	CHECK(captured.front().Data == Value{double{.75}});
	CHECK(captured.front().NodeId == "alias");
	CHECK(captured.front().SourceKeyId.empty());

	auto local = document;
	auto &localAxes = local.Nodes[1].SourceSeparatedVec2Animators->Inputs.front();
	localAxes.Axes[0].Keys[0].Data = .45;
	localAxes.Axes[0].Keys[0].SourceKeyId = "local-x0";
	local.SourceAnimators->Bindings.front().Axes = {
		GroupAxisStorage::Local, "alias", "center", "owner", GroupSubtypeAnimator::Animated
	};
	const SourceKeyframeIdentity localRequest{"alias", "center", {0}, 0};
	REQUIRE(CaptureSourceKeyframes(local, {&localRequest, 1}, captured, diagnostic) == Status::Ok);
	REQUIRE(captured.size() == 1);
	CHECK(captured.front().Data == Value{double{.45}});
	CHECK(captured.front().SourceKeyId == "local-x0");
}

TEST_CASE(
	"Keyframe capture refuses cold, duplicate and invalid requests without replacing output",
	"[imagegraph][source_keys]"
) {
	const auto warm = SharedAxes();
	const SourceKeyframeIdentity valid{"alias", "center", {0}, 0};
	std::vector<Keyframe> captured{Keyframe{"sentinel", "port", 7, .125, "step", std::nullopt}};
	const auto prior = captured;
	Diagnostic diagnostic;
	const SourceKeyframeIdentity duplicate[] = {valid, valid};
	CHECK(CaptureSourceKeyframes(warm, duplicate, captured, diagnostic) == Status::InvalidValue);
	CHECK(captured == prior);
	const SourceKeyframeIdentity invalidAxis{"alias", "center", {0}, 2};
	CHECK(CaptureSourceKeyframes(warm, {&invalidAxis, 1}, captured, diagnostic) == Status::InvalidValue);
	CHECK(captured == prior);
	const SourceKeyframeIdentity missing{"alias", "center", {99}, 0};
	CHECK(CaptureSourceKeyframes(warm, {&missing, 1}, captured, diagnostic) == Status::InvalidValue);
	CHECK(captured == prior);
	CHECK(CaptureSourceKeyframes(warm, {&valid, 1}, captured, diagnostic, 1) == Status::LimitExceeded);
	CHECK(captured == prior);

	auto cold = warm;
	auto &input = cold.Nodes[1].SourceSeparatedVec2Animators->Inputs.front();
	input.Initialized = false;
	for (auto &axis : input.Axes)
		axis.Keys.clear();
	cold.SourceAnimators->Bindings.front().Axes = {
		GroupAxisStorage::Uninitialized, "alias", "center", "owner", GroupSubtypeAnimator::Animated
	};
	CHECK(
		CaptureSourceKeyframes(cold, {&valid, 1}, captured, diagnostic) ==
		Status::SourceAxisInitializationRequired
	);
	CHECK(captured == prior);
	// grug validate uncaptured arrays too; malformed scalar payloads cannot become trusted pins.
	auto uncaptured = warm;
	uncaptured.SourceAnimators = {};
	uncaptured.Nodes.resize(1);
	std::erase_if(uncaptured.Keyframes, [](const auto &key) { return key.NodeId != "owner"; });
	std::erase_if(uncaptured.Tracks, [](const auto &track) { return track.NodeId != "owner"; });
	const SourceKeyframeIdentity owner{"owner", "center", {0}, 0};
	auto &malformed =
		uncaptured.Nodes.front().SourceSeparatedVec2Animators->Inputs.front().Axes[0].Keys.front();
	malformed.Data = std::string{"not scalar"};
	CHECK(CaptureSourceKeyframes(uncaptured, {&owner, 1}, captured, diagnostic) != Status::Ok);
	CHECK(captured == prior);
	malformed.Data = .25;
	malformed.Subframe = 2;
	CHECK(CaptureSourceKeyframes(uncaptured, {&owner, 1}, captured, diagnostic) != Status::Ok);
	CHECK(captured == prior);
}

TEST_CASE("Keyframe capture follows retired scalar axis ownership", "[imagegraph][source_keys]") {
	auto document = SharedAxes();
	auto retiredAxes = document.Nodes.front().SourceSeparatedVec2Animators->Inputs.front();
	retiredAxes.Port = "native:animator:0";
	for (auto &axis : retiredAxes.Axes)
		for (auto &key : axis.Keys)
			key.Port = retiredAxes.Port;
	DetachedSourceAnimator metadata;
	metadata.Id = "native:animator:0";
	metadata.OwnerId = "owner";
	metadata.OriginalPort = "center";
	metadata.Writer = GroupSubtypeAnimator::Animated;
	metadata.Type = ValueType::Vector2;
	metadata.Track = AnimationTrack{"owner", metadata.Id, "hold", -1};
	GroupSubtypeOverlay overlay;
	overlay.NodeId = "owner";
	overlay.Port = metadata.Id;
	overlay.Fixed = Vector2{.1, .2};
	overlay.SeparatedVec2.emplace() = retiredAxes;
	document.SourceAnimators->Detached.push_back(metadata);
	document.SourceAnimators->DetachedValues.push_back(overlay);
	for (auto &binding : document.SourceAnimators->Bindings)
		binding.Axes.Port = metadata.Id;
	auto &currentAxes = document.Nodes.front().SourceSeparatedVec2Animators->Inputs.front();
	for (auto &axis : currentAxes.Axes)
		for (auto &key : axis.Keys) {
			key.Data = std::get<double>(key.Data) + 20;
			key.SourceKeyId.clear();
		}
	Valid(document);
	const SourceKeyframeIdentity identity{"alias", "center", {0}, 0};
	std::vector<Keyframe> captured;
	Diagnostic diagnostic;
	REQUIRE(CaptureSourceKeyframes(document, {&identity, 1}, captured, diagnostic) == Status::Ok);
	REQUIRE(captured.size() == 1);
	CHECK(captured.front().Data == Value{double{.25}});
	CHECK(captured.front().NodeId == "alias");
	CHECK(captured.front().Port == "center");
	CHECK(captured.front().SourceKeyId.empty());
}

TEST_CASE("Axis alias selections deduplicate and preserve results on conflict", "[imagegraph][source_keys]") {
	const auto document = SharedAxes();
	const auto a = AxisKey(document, "alias", 0, {}), b = AxisKey(document, "sibling", 0, {});
	auto movedA = a, movedB = b;
	REQUIRE(SetFrameTime(movedA, {2}));
	REQUIRE(SetFrameTime(movedB, {2}));
	SourceKeyframeEdit edits[] = {{&a, &movedA, false, 0}, {&b, &movedB, false, 0}};
	Document result;
	Diagnostic error;
	const auto status = ApplySourceKeyframeEdits(document, edits, result, error);
	INFO(error.Message);
	REQUIRE(status == Status::Ok);
	CHECK(result.Nodes.front().SourceSeparatedVec2Animators->Inputs.front().Axes[0].Keys.size() == 2);
	Valid(result);
	const auto accepted = result;
	REQUIRE(SetFrameTime(movedB, {3}));
	CHECK(ApplySourceKeyframeEdits(document, edits, result, error) == Status::InvalidValue);
	CHECK(result == accepted);
	auto stale = a;
	stale.Data = .9;
	edits[0].Original = &stale;
	CHECK(ApplySourceKeyframeEdits(document, {edits, 1}, result, error) == Status::InvalidValue);
	CHECK(result == accepted);
	edits[0].Original = &a;
	movedA.Data = Vector2{.1, .2};
	CHECK(ApplySourceKeyframeEdits(document, {edits, 1}, result, error) == Status::TypeMismatch);
	CHECK(result == accepted);
	movedA = a;
	edits[0].Axis = 2;
	CHECK(ApplySourceKeyframeEdits(document, {edits, 1}, result, error) == Status::InvalidValue);
	CHECK(result == accepted);
	edits[0].Axis = edits[1].Axis = 0;
	edits[0].Replacement = edits[1].Replacement = nullptr;
	REQUIRE(ApplySourceKeyframeEdits(document, edits, result, error) == Status::Ok);
	for (const char *node : {"owner", "alias", "sibling"}) {
		CHECK_FALSE(FindAxisKey(result, node, 0, {}));
		CHECK(AxisKey(result, node, 1, {}).Data == Value{2.0});
	}
	Valid(result);
}

TEST_CASE("Axis resolver charges long names across repeated edits", "[imagegraph][source_keys]") {
	Document document;
	document.FormatVersion = 9;
	const auto prototype = SharedAxes().Nodes.front();
	for (size_t index = 0; index < 128; ++index) {
		auto node = prototype;
		node.Id = std::string(16384, 'n') + std::to_string(index);
		for (auto &axis : node.SourceSeparatedVec2Animators->Inputs.front().Axes)
			for (auto &key : axis.Keys)
				key.NodeId = node.Id;
		document.Nodes.push_back(std::move(node));
	}
	document.Outputs = {{"result", document.Nodes.front().Id, "surface_out"}};
	const auto original =
		document.Nodes.front().SourceSeparatedVec2Animators->Inputs.front().Axes[0].Keys.front();
	auto moved = original;
	REQUIRE(SetFrameTime(moved, {2}));
	std::array<SourceKeyframeEdit, 64> edits;
	for (auto &edit : edits)
		edit = {&original, &moved, false, 0};
	Document result = SharedAxes();
	const auto before = result;
	Diagnostic error;
	CHECK(ApplySourceKeyframeEdits(document, edits, result, error) == Status::LimitExceeded);
	CHECK(error.Message == "source key edit comparison work exceeds bounds");
	CHECK(result == before);
	std::vector<SourceKeyframeIdentity> selected;
	for (size_t index = 0; index < 16; ++index)
		selected.push_back({document.Nodes[index].Id, "center", {0}, 0});
	std::vector<Keyframe> snapshot{original};
	const auto priorSnapshot = snapshot;
	const auto captureStatus = CaptureSourceKeyframes(document, selected, snapshot, error);
	INFO(error.Message);
	CHECK(captureStatus == Status::LimitExceeded);
	CHECK(snapshot == priorSnapshot);
}
