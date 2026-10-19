#include <engine/core/Bytes.hpp>
#include <engine/ecs/Attributes.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/replication/Submission.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>

TEST_SUITE_ID("engine.replication.attribute-resources")
TEST_DEPENDS("engine.ecs.attributes")

using namespace engine;

TEST_CASE("empty attribute exports preserve clears across table transitions", "[replication][attributes]") {
	ecs::RegisterAttributeComponents();
	ecs::Store source("empty-attribute-exports");
	const auto first = source.Create();
	const auto second = source.Create();
	const std::array owners{first, second, first};
	CHECK(ecs::SelectAttributes(source, owners).Entities.empty());
	source.SetResource(ecs::AttributeTable{});
	CHECK(ecs::SelectAttributes(source, owners).Entities.empty());
	ecs::AttributeValue value;
	value.Type = ecs::PropertyType::Double;
	value.Double = 29;
	REQUIRE(ecs::SetAttribute(source, first, core::Name("Value"), value));
	CHECK(ecs::SelectAttributes(source, owners).Entities.size() == 1);
	REQUIRE(ecs::ClearAttributes(source, first) == 1);
	const auto selected = ecs::SelectAttributes(source, owners);
	CHECK(selected.Entities.empty());
	CHECK(selected.Revisions.empty());
	CHECK(selected.NextRevision == 1);
	core::ByteWriter encoded;
	ecs::Components::Describe(ecs::Components::Assigned<ecs::AttributeTable>()).Write(encoded, &selected, 1);
	REQUIRE(encoded.Size() == sizeof(uint32_t));
	core::ByteReader reader(encoded.Bytes());
	CHECK(reader.ReadUInt32() == 0);
	CHECK(reader.AtEnd());
	REQUIRE(ecs::SetAttribute(source, second, core::Name("Value"), value));
	CHECK(ecs::SelectAttributes(source, std::array{first}).Entities.empty());
}

TEST_CASE("empty attribute resource writes retain predicted local state", "[replication][attributes]") {
	ecs::RegisterAttributeComponents();
	ecs::Store store("empty-attribute-writes");
	const auto authority = store.Create();
	const auto predicted = store.CreatePredicted();
	const auto key = core::Name("LocalValue");
	bool present = true;
	bool revisions = false;
	bool attributes = false;
	SECTION("no preceding resource") {
		present = false;
	}
	SECTION("empty maps with a live revision counter") {}
	SECTION("predicted revisions without attributes") {
		revisions = true;
	}
	SECTION("predicted attributes and revisions") {
		revisions = attributes = true;
	}
	ecs::AttributeTable preceding;
	preceding.NextRevision = 99;
	if (revisions) {
		preceding.Revisions[static_cast<uint32_t>(predicted.Id)][key.Id()] = 71;
		preceding.Revisions[static_cast<uint32_t>(authority.Id)][key.Id()] = 72;
	}
	if (attributes) {
		ecs::AttributeValue value;
		value.Type = ecs::PropertyType::Double;
		value.Double = 43;
		preceding.Entities[static_cast<uint32_t>(predicted.Id)][key.Id()] = value;
		preceding.Entities[static_cast<uint32_t>(authority.Id)][key.Id()] = value;
	}
	if (present) store.SetResource(preceding);
	core::ByteWriter bytes;
	bytes.WriteUInt32(0);
	replication::Delta delta;
	delta.Components.push_back({core::Name("ecs.AttributeTable"), {ecs::NULL_ENTITY}, bytes.TakeBytes()});
	const auto outcome = replication::WriteComponents(store, delta);
	REQUIRE(outcome.Status == replication::ApplyStatus::Ok);
	REQUIRE(outcome.Whole);
	const auto *written = store.Resource<ecs::AttributeTable>();
	REQUIRE(written != nullptr);
	CHECK(written->NextRevision == (present ? 99 : 1));
	CHECK(written->Entities.size() == (attributes ? 1 : 0));
	CHECK(written->Revisions.size() == (revisions ? 1 : 0));
	if (attributes)
		CHECK(written->Entities.at(static_cast<uint32_t>(predicted.Id)).at(key.Id()).Double == 43);
	if (revisions) CHECK(written->Revisions.at(static_cast<uint32_t>(predicted.Id)).at(key.Id()) == 71);
}
