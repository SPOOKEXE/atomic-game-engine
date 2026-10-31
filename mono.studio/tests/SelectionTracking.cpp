#include <engine/core/HeapProfile.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <span>
#include <studio/SelectionTracking.hpp>
#include <type_traits>
#include <vector>

TEST_SUITE_ID("studio.selection-tracking")

using engine::ecs::Entity;
using engine::world::WorldId;

TEST_CASE("selection values track actual changes and do not expose mutable aliases", "[studio][selection]") {
	static_assert(!std::is_convertible_v<studio::SelectionList &, std::vector<Entity> &>);
	static_assert(!std::is_assignable_v<decltype(*std::declval<studio::SelectionList &>().begin()), Entity>);
	uint64_t revision = 0;
	studio::SelectionList selected(&revision);
	studio::SelectionWorldHandle world(&revision);
	studio::SelectionRootFlag root(&revision);
	selected.clear();
	world = {};
	root = false;
	CHECK(revision == 0);
	selected = {Entity{1}};
	world = WorldId{2};
	root = true;
	CHECK(revision == 3);
	selected = {Entity{1}};
	world = WorldId{2};
	world.Index = 2;
	root = true;
	CHECK(revision == 3);
	world.Index = 3;
	selected = {Entity{2}};
	root = false;
	CHECK(revision == 6);
	CHECK(std::span<const Entity>(selected).front() == Entity{2});

	auto detached = selected;
	detached.clear();
	CHECK(revision == 6);
	CHECK(selected.front() == Entity{2});
}
TEST_CASE("repeating an unchanged selection allocates nothing", "[studio][selection][heap]") {
	if (!engine::core::HeapProfile::IsCompiledIn()) SKIP("allocator hooks are not compiled in");
	uint64_t revision = 0;
	studio::SelectionList selected(&revision);
	selected = {Entity{1}};
	const auto before = engine::core::HeapProfile::Totals();
	for (int count = 0; count < 100; count++)
		selected = {Entity{1}};
	const auto after = engine::core::HeapProfile::Totals();
	CHECK(after.TotalBlocks == before.TotalBlocks);
	CHECK(after.TotalBytes == before.TotalBytes);
	CHECK(revision == 1);
}
