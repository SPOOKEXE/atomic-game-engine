#include <engine/core/Name.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/SharedStores.hpp>
#include <engine/world/Universe.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <string>
#include <vector>

TEST_SUITE_ID("engine.world.sharedstores")

using engine::core::Name;
using engine::world::BusKind;
using engine::world::BusStatus;
using engine::world::Universe;

namespace {
	std::vector<std::byte> Bytes(std::string_view text) {
		const auto *first = reinterpret_cast<const std::byte *>(text.data());
		return {first, first + text.size()};
	}
}

TEST_CASE("shared store administration returns sorted copies", "[world][shared-stores]") {
	Universe universe;
	REQUIRE(universe.SetSharedStoreValue(BusKind::MemoryStore, Name("z"), Bytes("last")) == BusStatus::Ok);
	REQUIRE(universe.SetSharedStoreValue(BusKind::MemoryStore, Name("a"), Bytes("first")) == BusStatus::Ok);

	auto entries = universe.SharedStoreEntries(BusKind::MemoryStore);
	REQUIRE(entries.size() == 2);
	CHECK(entries[0].Key == Name("a"));
	CHECK(entries[1].Key == Name("z"));
	entries[0].Value.clear();

	std::vector<std::byte> held;
	CHECK(universe.Peek(BusKind::MemoryStore, Name("a"), &held) == BusStatus::Ok);
	CHECK(held == Bytes("first"));
}

TEST_CASE("admin datastore edits advance versions and remove by name", "[world][shared-stores]") {
	Universe universe;
	REQUIRE(
		universe.SetSharedStoreValue(BusKind::DataStore, Name("player:7"), Bytes("one")) == BusStatus::Ok
	);
	REQUIRE(
		universe.SetSharedStoreValue(BusKind::DataStore, Name("player:7"), Bytes("two")) == BusStatus::Ok
	);

	const auto entries = universe.SharedStoreEntries(BusKind::DataStore);
	REQUIRE(entries.size() == 1);
	CHECK(entries[0].Version == 2);
	CHECK(entries[0].Value == Bytes("two"));

	CHECK(universe.RemoveSharedStoreValue(BusKind::DataStore, Name("player:7")) == BusStatus::Ok);
	CHECK(universe.RemoveSharedStoreValue(BusKind::DataStore, Name("player:7")) == BusStatus::NotFound);
	CHECK(universe.SharedStoreEntries(BusKind::DataStore).empty());
}

TEST_CASE("only shared key value stores accept administration", "[world][shared-stores]") {
	Universe universe;
	CHECK(
		universe.SetSharedStoreValue(BusKind::Messaging, Name("key"), Bytes("value")) ==
		BusStatus::Unsupported
	);
	CHECK(universe.RemoveSharedStoreValue(BusKind::Teleport, Name("key")) == BusStatus::Unsupported);
	CHECK(universe.SharedStoreEntries(BusKind::Channel).empty());
}
