// Attribute resource export and application over the moving-world population.
// Empty tables remain real resource messages so clears still arrive.

#include <engine/core/Bytes.hpp>
#include <engine/ecs/Attributes.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/replication/Submission.hpp>
#include <engine/testing/Bench.hpp>

#include <vector>

TEST_SUITE_ID("engine.replication.bench.attributes")

namespace attributes_bench {
	constexpr size_t ENTITIES = 20'000;

	struct Fixture {
		engine::ecs::Store World{"attribute-resource-bench"};
		std::vector<engine::ecs::Entity> Owners;
		engine::replication::Delta Incoming;

		explicit Fixture(bool sparse) {
			using namespace engine;
			ecs::RegisterAttributeComponents();
			World.SetResource(ecs::AttributeTable{});
			Owners.reserve(ENTITIES);
			for (size_t index = 0; index < ENTITIES; index++)
				Owners.push_back(World.Create());
			if (sparse) {
				ecs::AttributeValue value;
				value.Type = ecs::PropertyType::Double;
				value.Double = 19;
				for (size_t index = 0; index < 4; index++) {
					ecs::SetAttribute(World, Owners[index * 4096], core::Name("BenchValue"), value);
				}
			}
			core::ByteWriter bytes;
			bytes.WriteUInt32(0);
			Incoming.Components.push_back(
				{core::Name("ecs.AttributeTable"), {ecs::NULL_ENTITY}, bytes.TakeBytes()}
			);
		}
	};

	Fixture &Empty() {
		static Fixture fixture(false);
		return fixture;
	}

	Fixture &Sparse() {
		static Fixture fixture(true);
		return fixture;
	}
}

BENCH("SelectAttributes · 20k visible · empty table", 1) {
	auto &fixture = attributes_bench::Empty();
	const auto selected = engine::ecs::SelectAttributes(fixture.World, fixture.Owners);
	engine::testing::Consume(selected.Entities.size());
}

BENCH("SelectAttributes · 20k visible · four owners", 1) {
	auto &fixture = attributes_bench::Sparse();
	const auto selected = engine::ecs::SelectAttributes(fixture.World, fixture.Owners);
	engine::testing::Consume(selected.Entities.size());
}

BENCH("WriteComponents · 20k local rows · empty attributes", 1) {
	auto &fixture = attributes_bench::Empty();
	const auto result = engine::replication::WriteComponents(fixture.World, fixture.Incoming);
	engine::testing::Consume(result.Status);
}
