#include "AccessibilityOrigins.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <limits>

TEST_SUITE_ID("client.accessibilityorigins")

using namespace client;
using namespace engine;

TEST_CASE("native accessibility actions retain their displayed-frame origin", "[client][accessibility]") {
	AccessibilityOrigins origins;
	gui::SemanticNode node;
	node.Instance = ecs::Entity(3);
	node.Collector = ecs::Entity(2);
	REQUIRE(origins.Publish(std::span(&node, 1), 7, 10, false));
	const uint64_t first = origins.CurrentIds().front();
	node.Instance = ecs::Entity(4);
	node.Collector = ecs::Entity(5);
	REQUIRE(origins.Publish(std::span(&node, 1), 7, 11, false));
	const uint64_t second = origins.CurrentIds().front();
	CHECK(first != second);
	const auto late = origins.Resolve(first, gui::SemanticAction::Activate);
	CHECK(late.Status == AccessibilityActionStatus::Ready);
	CHECK(late.Target == ecs::Entity(3));
	CHECK(late.Collector == ecs::Entity(2));
	CHECK(late.FrameSequence == 10);
	const auto current = origins.Resolve(second, gui::SemanticAction::Focus);
	CHECK(current.Target == ecs::Entity(4));
	CHECK(current.FrameSequence == 11);
	CHECK(current.Action == gui::SemanticAction::Focus);
}

TEST_CASE("native node reuse needs the same world and ordered targets", "[client][accessibility]") {
	AccessibilityOrigins origins;
	gui::SemanticNode node;
	node.Instance = ecs::Entity(3);
	node.Collector = ecs::Entity(2);
	REQUIRE(origins.Publish(std::span(&node, 1), 7, 10, false));
	const uint64_t id = origins.CurrentIds().front();
	REQUIRE(origins.Publish(std::span(&node, 1), 7, 11, true));
	CHECK(origins.CurrentIds().front() == id);
	CHECK(origins.Resolve(id, gui::SemanticAction::Focus).FrameSequence == 11);
	CHECK_FALSE(origins.Publish(std::span(&node, 1), 8, 12, true));
	node.Instance = ecs::Entity(9);
	CHECK_FALSE(origins.Publish(std::span(&node, 1), 7, 12, true));
	CHECK(origins.Resolve(id, gui::SemanticAction::Focus).FrameSequence == 11);
}

TEST_CASE("native tree IDs remain stable only for proven equivalent frames", "[client][accessibility]") {
	AccessibilityTree tree;
	gui::SemanticSnapshot snapshot;
	auto &node = snapshot.Nodes.emplace_back();
	node.Instance = ecs::Entity(3);
	node.Collector = ecs::Entity(2);
	node.Name = "button";
	REQUIRE(tree.Publish(snapshot, 7, 10, false));
	CHECK_FALSE(tree.HasLegacyTree(7));
	const uint64_t first = tree.Origins().CurrentIds().front();
	REQUIRE(tree.Publish(snapshot, 7, 10, false));
	CHECK(tree.Origins().CurrentIds().front() == first);
	REQUIRE(tree.Publish(snapshot, 7, 11, false));
	const uint64_t second = tree.Origins().CurrentIds().front();
	CHECK(second != first);
	REQUIRE(tree.Publish(snapshot, 7, 12, true));
	CHECK(tree.Origins().CurrentIds().front() == second);
	CHECK(tree.Origins().Resolve(second, gui::SemanticAction::Activate).FrameSequence == 12);
	REQUIRE(tree.Publish(snapshot, 7, 0, true));
	CHECK(tree.HasLegacyTree(7));
	CHECK_FALSE(tree.HasLegacyTree(8));
	const uint64_t legacy = tree.Origins().CurrentIds().front();
	CHECK(legacy != second);
	REQUIRE(tree.Publish(snapshot, 7, 0, true));
	CHECK(tree.Origins().CurrentIds().front() == legacy);
	node.Name = "changed";
	REQUIRE(tree.Publish(snapshot, 7, 13, true));
	const uint64_t third = tree.Origins().CurrentIds().front();
	CHECK(third != second);
	snapshot.Truncated = true;
	REQUIRE(tree.Publish(snapshot, 7, 14, true));
	const uint64_t fourth = tree.Origins().CurrentIds().front();
	CHECK(fourth != third);
	REQUIRE(tree.Publish(snapshot, 7, 15, true));
	CHECK(tree.Origins().CurrentIds().front() != fourth);
	const uint64_t preserved = tree.Origins().CurrentIds().front();
	node.Bounds.Min.X = std::numeric_limits<float>::infinity();
	CHECK_FALSE(tree.Publish(snapshot, 7, 16, true));
	CHECK(tree.Origins().CurrentIds().front() == preserved);
}

TEST_CASE("expired native IDs and callback overflow are reported", "[client][accessibility]") {
	AccessibilityOrigins origins;
	gui::SemanticNode node;
	node.Instance = ecs::Entity(3);
	REQUIRE(origins.Publish(std::span(&node, 1), 7, 1, false));
	const uint64_t first = origins.CurrentIds().front();
	for (size_t i = 0; i <= AccessibilityOrigins::MAX_TREES; ++i)
		REQUIRE(origins.Publish(std::span(&node, 1), 7, i + 2, false));
	CHECK(origins.Resolve(first, gui::SemanticAction::Activate).Status == AccessibilityActionStatus::Expired);

	AccessibilityRequests requests;
	const auto valid = origins.Resolve(origins.CurrentIds().front(), gui::SemanticAction::Focus);
	AccessibilityAction unsupported;
	unsupported.Status = AccessibilityActionStatus::Unsupported;
	unsupported.NativeAction = 91;
	requests.Push(unsupported);
	const auto reported = requests.Drain();
	REQUIRE(reported.size() == 1);
	CHECK(reported.front().Status == AccessibilityActionStatus::Unsupported);
	CHECK(reported.front().NativeAction == 91);
	for (size_t i = 0; i < AccessibilityRequests::MAX_PENDING + 2; ++i)
		requests.Push(valid);
	const auto drained = requests.Drain();
	REQUIRE(drained.size() == AccessibilityRequests::MAX_PENDING + 1);
	CHECK(drained.front().Status == AccessibilityActionStatus::Ready);
	CHECK(drained.back().Status == AccessibilityActionStatus::Overflow);
	CHECK(drained.back().RejectedCount == 2);
	CHECK(requests.Drain().empty());
}

TEST_CASE("native tree retirement expires pending and later callbacks", "[client][accessibility]") {
	AccessibilityTree tree;
	gui::SemanticSnapshot snapshot;
	auto &node = snapshot.Nodes.emplace_back();
	node.Instance = ecs::Entity(3);
	node.Collector = ecs::Entity(2);
	REQUIRE(tree.Publish(snapshot, 7, 9, false));
	const uint64_t oldId = tree.Origins().CurrentIds().front();
	AccessibilityRequests requests;
	requests.Push(tree.Origins().Resolve(oldId, gui::SemanticAction::Focus));
	tree.Retire();
	CHECK_FALSE(tree.HasLegacyTree(7));
	requests.Retire();
	CHECK(tree.Snapshot().Nodes.empty());
	CHECK(
		tree.Origins().Resolve(oldId, gui::SemanticAction::Focus).Status == AccessibilityActionStatus::Expired
	);
	const auto retired = requests.Drain();
	REQUIRE(retired.size() == 1);
	CHECK(retired.front().Status == AccessibilityActionStatus::Expired);
	REQUIRE(tree.Publish(snapshot, 7, 10, true));
	CHECK(tree.Origins().CurrentIds().front() != oldId);
}

TEST_CASE("native IDs do not cross direct and retained routing modes", "[client][accessibility]") {
	AccessibilityTree tree;
	gui::SemanticSnapshot snapshot;
	auto &node = snapshot.Nodes.emplace_back();
	node.Instance = ecs::Entity(3);
	node.Collector = ecs::Entity(2);
	REQUIRE(tree.Publish(snapshot, 7, 0, true));
	const uint64_t direct = tree.Origins().CurrentIds().front();
	auto invalid = snapshot;
	invalid.Nodes.front().Bounds.Min.X = std::numeric_limits<float>::infinity();
	CHECK_FALSE(tree.Publish(invalid, 7, 10, false));
	CHECK(tree.HasLegacyTree(7));
	CHECK(
		tree.Origins().Resolve(direct, gui::SemanticAction::Focus).Status == AccessibilityActionStatus::Ready
	);
	REQUIRE(tree.Publish(snapshot, 7, 10, true));
	const uint64_t retained = tree.Origins().CurrentIds().front();
	CHECK(retained != direct);
	CHECK(
		tree.Origins().Resolve(direct, gui::SemanticAction::Focus).Status ==
		AccessibilityActionStatus::Expired
	);
	REQUIRE(tree.Publish(snapshot, 7, 0, true));
	CHECK(tree.Origins().CurrentIds().front() != retained);
	CHECK(
		tree.Origins().Resolve(retained, gui::SemanticAction::Focus).Status ==
		AccessibilityActionStatus::Expired
	);
}
