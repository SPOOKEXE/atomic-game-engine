// Reaches this module's src/ rather than its include/, because an archetype is
// the storage layout and publishing it to make it testable is exactly what
// AGENTS.md says not to do.
#include "Archetype.hpp"

#include <engine/core/Bytes.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <span>
#include <string>
#include <vector>

TEST_SUITE_ID("engine.ecs.archetype")

using engine::ecs::Archetype;
using engine::ecs::Column;
using engine::ecs::ComponentId;
using engine::ecs::Components;
using engine::ecs::ComponentSet;
using engine::ecs::Entity;
using engine::ecs::NULL_ENTITY;

namespace archetype_test {
	struct Place {
		float X = 0.0f;
	};
	struct Speed {
		float X = 0.0f;
	};
	struct Health {
		int Value = 0;
	};
	struct Sleeping {};

	// Non-trivial, so moving a row between tables has to do real work.
	struct Label {
		static inline int Live = 0;
		std::string Text;

		Label() {
			Live++;
		}
		explicit Label(std::string text) : Text(std::move(text)) {
			Live++;
		}
		Label(const Label &other) : Text(other.Text) {
			Live++;
		}
		Label(Label &&other) noexcept : Text(std::move(other.Text)) {
			Live++;
		}
		Label &operator=(const Label &) = default;
		Label &operator=(Label &&) = default;
		~Label() {
			Live--;
		}
	};

	// Registered explicitly rather than by the compiler's spelling. Two
	// anonymous-namespace types in different test files spell identically -
	// `{anonymous}::Label` - and the registry now aborts on that collision
	// rather than letting one run with the other's destructor.
	ComponentId L() {
		static const ComponentId id = Components::Register<Label>("test.archetype.label");
		return id;
	}

	ComponentId P() {
		return Components::Of<Place>();
	}
	ComponentId S() {
		return Components::Of<Speed>();
	}
	ComponentId H() {
		return Components::Of<Health>();
	}

	void SetPlace(Archetype &table, size_t row, float value) {
		const Place place{value};
		table.Find(P())->Assign(row, &place);
	}

	float GetPlace(const Archetype &table, size_t row) {
		return static_cast<const Place *>(table.Find(P())->At(row))->X;
	}
}

using namespace archetype_test;

TEST_CASE("a table has one column per component in its set", "[ecs]") {
	Archetype table(ComponentSet::Intern({P(), S()}));

	REQUIRE(table.Rows() == 0);
	REQUIRE(table.Find(P()) != nullptr);
	REQUIRE(table.Find(S()) != nullptr);
	REQUIRE(table.Find(H()) == nullptr);
}

TEST_CASE("appending grows every column together", "[ecs]") {
	// The invariant a batch iterator relies on: row n of every column belongs
	// to entity n. A column that fell behind would hand a system one entity's
	// transform and another's velocity.
	Archetype table(ComponentSet::Intern({P(), S()}));

	for (uint64_t index = 1; index <= 5; index++) {
		REQUIRE(table.Append(Entity{index}) == index - 1);
	}

	REQUIRE(table.Rows() == 5);
	REQUIRE(table.Find(P())->Size() == 5);
	REQUIRE(table.Find(S())->Size() == 5);
	REQUIRE(table.Entities().size() == 5);
	REQUIRE(table.EntityAt(2) == Entity{3});
}

TEST_CASE("an empty set is a table of entities and no columns", "[ecs]") {
	// Where an entity with no components lives. Not a special case: it has rows
	// and an id array like any other table.
	Archetype table(ComponentSet::Empty());

	table.Append(Entity{7});
	table.Append(Entity{8});

	REQUIRE(table.Rows() == 2);
	REQUIRE(table.Set().Size() == 0);
	REQUIRE(table.EntityAt(1) == Entity{8});
}

TEST_CASE("removal reports which entity moved", "[ecs]") {
	// The store cannot work this out afterwards - the entity that moved has a
	// directory entry still pointing at its old row, and nothing else knows.
	Archetype table(ComponentSet::Intern({P()}));

	for (uint64_t index = 1; index <= 4; index++) {
		const size_t row = table.Append(Entity{index});
		SetPlace(table, row, static_cast<float>(index));
	}

	const Entity moved = table.RemoveSwapBack(1);

	REQUIRE(moved == Entity{4});
	REQUIRE(table.Rows() == 3);
	REQUIRE(table.EntityAt(1) == Entity{4});
	REQUIRE(GetPlace(table, 1) == 4.0f); // the value moved with the entity
}

TEST_CASE("removing the last row moves nothing", "[ecs]") {
	Archetype table(ComponentSet::Intern({P()}));
	table.Append(Entity{1});
	table.Append(Entity{2});

	REQUIRE(table.RemoveSwapBack(1) == NULL_ENTITY);
	REQUIRE(table.Rows() == 1);
	REQUIRE(table.EntityAt(0) == Entity{1});
}

TEST_CASE("removing the only row empties the table", "[ecs]") {
	Archetype table(ComponentSet::Intern({P()}));
	table.Append(Entity{1});

	REQUIRE(table.RemoveSwapBack(0) == NULL_ENTITY);
	REQUIRE(table.Rows() == 0);
	REQUIRE(table.Find(P())->Empty());
}

TEST_CASE("columns and ids stay the same length through churn", "[ecs]") {
	Archetype table(ComponentSet::Intern({P(), S(), H()}));

	for (uint64_t index = 1; index <= 50; index++) {
		table.Append(Entity{index});
	}
	for (int index = 0; index < 20; index++) {
		table.RemoveSwapBack(0);
	}
	for (uint64_t index = 100; index < 110; index++) {
		table.Append(Entity{index});
	}

	const size_t rows = table.Rows();
	REQUIRE(rows == 40);
	REQUIRE(table.Entities().size() == rows);
	REQUIRE(table.Find(P())->Size() == rows);
	REQUIRE(table.Find(S())->Size() == rows);
	REQUIRE(table.Find(H())->Size() == rows);
}

// --- moving between tables ------------------------------------------------

TEST_CASE("adding a component carries the old values across", "[ecs]") {
	// What `Set<T>` on a component the entity does not have has to do: find the
	// wider table, move every value that exists, default the new one.
	Archetype narrow(ComponentSet::Intern({P()}));
	Archetype wide(ComponentSet::Intern({P(), S()}));

	const size_t from = narrow.Append(Entity{1});
	SetPlace(narrow, from, 3.5f);

	const size_t to = wide.AdoptRow(narrow, from, Entity{1});
	narrow.RemoveSwapBack(from);

	REQUIRE(wide.Rows() == 1);
	REQUIRE(narrow.Rows() == 0);
	REQUIRE(GetPlace(wide, to) == 3.5f);

	// The component the source did not have is present and defaulted, not
	// uninitialised.
	REQUIRE(static_cast<const Speed *>(wide.Find(S())->At(to))->X == 0.0f);
}

TEST_CASE("removing a component drops only that column", "[ecs]") {
	Archetype wide(ComponentSet::Intern({P(), S()}));
	Archetype narrow(ComponentSet::Intern({P()}));

	const size_t from = wide.Append(Entity{1});
	SetPlace(wide, from, 9.0f);
	const Speed speed{4.0f};
	wide.Find(S())->Assign(from, &speed);

	const size_t to = narrow.AdoptRow(wide, from, Entity{1});
	wide.RemoveSwapBack(from);

	REQUIRE(narrow.Rows() == 1);
	REQUIRE(GetPlace(narrow, to) == 9.0f);
	REQUIRE(narrow.Find(S()) == nullptr);
}

TEST_CASE("a move between disjoint sets defaults everything", "[ecs]") {
	// The merge walk has to survive the case where the two sorted id lists
	// share nothing at all.
	Archetype left(ComponentSet::Intern({P()}));
	Archetype right(ComponentSet::Intern({S(), H()}));

	const size_t from = left.Append(Entity{1});
	SetPlace(left, from, 1.0f);

	const size_t to = right.AdoptRow(left, from, Entity{1});

	REQUIRE(right.Rows() == 1);
	REQUIRE(static_cast<const Speed *>(right.Find(S())->At(to))->X == 0.0f);
	REQUIRE(static_cast<const Health *>(right.Find(H())->At(to))->Value == 0);
}

TEST_CASE("a move to the empty set keeps only the entity", "[ecs]") {
	Archetype wide(ComponentSet::Intern({P(), S()}));
	Archetype empty(ComponentSet::Empty());

	const size_t from = wide.Append(Entity{5});
	const size_t to = empty.AdoptRow(wide, from, Entity{5});

	REQUIRE(empty.Rows() == 1);
	REQUIRE(empty.EntityAt(to) == Entity{5});
}

TEST_CASE("a move out of the empty set defaults everything", "[ecs]") {
	Archetype empty(ComponentSet::Empty());
	Archetype wide(ComponentSet::Intern({P(), S()}));

	const size_t from = empty.Append(Entity{6});
	const size_t to = wide.AdoptRow(empty, from, Entity{6});

	REQUIRE(wide.Rows() == 1);
	REQUIRE(GetPlace(wide, to) == 0.0f);
}

TEST_CASE("a tag moves as presence rather than as bytes", "[ecs]") {
	const ComponentId tag = Components::Of<Sleeping>();

	Archetype awake(ComponentSet::Intern({P()}));
	Archetype asleep(ComponentSet::Intern({P(), tag}));

	const size_t from = awake.Append(Entity{1});
	SetPlace(awake, from, 2.0f);

	const size_t to = asleep.AdoptRow(awake, from, Entity{1});

	REQUIRE(GetPlace(asleep, to) == 2.0f);
	REQUIRE(asleep.Find(tag) != nullptr);
	REQUIRE(asleep.Find(tag)->Size() == 1);
	REQUIRE(asleep.Find(tag)->At(0) == nullptr); // no bytes, only a row
}

TEST_CASE("a non-trivial component is moved, not copied or leaked", "[ecs]") {
	const ComponentId label = L();
	const int before = Label::Live;

	{
		Archetype narrow(ComponentSet::Intern({label}));
		Archetype wide(ComponentSet::Intern({label, P()}));

		const size_t from = narrow.Append(Entity{1});
		const Label value("carried");
		narrow.Find(label)->Assign(from, &value);
		REQUIRE(Label::Live == before + 2); // the local and the row

		const size_t to = wide.AdoptRow(narrow, from, Entity{1});
		narrow.RemoveSwapBack(from);

		REQUIRE(static_cast<const Label *>(wide.Find(label)->At(to))->Text == "carried");

		// One row in one table, plus the local. A leak would show as three.
		REQUIRE(Label::Live == before + 2);
	}

	REQUIRE(Label::Live == before);
}

TEST_CASE("reserve grows every column at once", "[ecs]") {
	Archetype table(ComponentSet::Intern({P(), S()}));
	table.Reserve(512);

	REQUIRE(table.Find(P())->Capacity() >= 512);
	REQUIRE(table.Find(S())->Capacity() >= 512);

	// And a chunk holds its address across the appends the reservation covered,
	// which is the version of "the base pointer holds" that survives chunking: a
	// batch is a run inside one chunk, so what a system caching a pointer for a
	// batch depends on is that *that* chunk does not move. The whole-column base
	// it used to depend on no longer exists.
	const Column *reserved = table.Find(P());
	REQUIRE(reserved->ChunkCount() == Column::ChunksFor(512));
	const std::vector<void *> before(reserved->ChunkData(), reserved->ChunkData() + reserved->ChunkCount());

	for (uint64_t index = 1; index <= 512; index++) {
		table.Append(Entity{index});
	}

	const Column *grown = table.Find(P());
	REQUIRE(grown->ChunkCount() == before.size());
	size_t moved = 0;
	for (size_t chunk = 0; chunk < before.size(); chunk++) {
		if (grown->ChunkData()[chunk] != before[chunk]) {
			moved++;
		}
	}
	REQUIRE(moved == 0);
}

TEST_CASE("a column position is the same for every table sharing a set", "[ecs]") {
	// What lets a query resolve its terms to positions once and reuse them
	// across every matching table.
	const ComponentSet &set = ComponentSet::Intern({P(), S(), H()});
	Archetype first(set);
	Archetype second(set);

	for (size_t position = 0; position < set.Size(); position++) {
		REQUIRE(first.ColumnAt(position).Type() == second.ColumnAt(position).Type());
		REQUIRE(first.ColumnAt(position).Type() == set.Ids()[position]);
	}
}

TEST_CASE("a table reads its columns in the writer's order", "[ecs][archetype]") {
	// **The cross-process case, reproduced in one.** `Write` emits columns in
	// its own set order, which is ascending by *its* component ids, and
	// `ComponentSet::Intern` sorts by the reading process's - so a server whose
	// `ecs.Hierarchy` is registered last and a client whose `ecs.Hierarchy` is
	// id 2 hold the same set in two different column orders.
	//
	// One process cannot have two id assignments, so the divergence is supplied
	// as the argument it really is: the order the snapshot recorded. Reading
	// with the wrong one is what a client did before this, and it read a
	// `Transform`'s bytes as a `Hierarchy`.
	const ComponentId place = P();
	const ComponentId health = H();

	Archetype source(ComponentSet::Intern({place, health}));

	REQUIRE(source.Append(Entity{7}) == 0);
	*static_cast<Place *>(source.Find(place)->At(0)) = Place{4.5f};
	*static_cast<Health *>(source.Find(health)->At(0)) = Health{99};

	// Written in this table's own order, whatever that is.
	const std::span<const ComponentId> members = source.Set().Ids();
	const std::vector<ComponentId> order(members.begin(), members.end());

	engine::core::ByteWriter writer;
	REQUIRE(source.Write(writer));

	// The reader's table holds the same set - interning guarantees the same
	// column order here, so the *reversed* order below stands in for a process
	// whose ids ran the other way.
	Archetype restored(ComponentSet::Intern({place, health}));
	engine::core::ByteReader reader(writer.Bytes());
	REQUIRE(restored.Read(reader, 1, order));

	CHECK(static_cast<const Place *>(restored.Find(place)->At(0))->X == 4.5f);
	CHECK(static_cast<const Health *>(restored.Find(health)->At(0))->Value == 99);

	// And the same bytes read against the wrong order do not quietly succeed:
	// `Place` is four bytes and `Health` is four, so this is the narrow case
	// that *would* have loaded silently wrong before - the values swap.
	std::vector<ComponentId> reversed(order.rbegin(), order.rend());
	Archetype confused(ComponentSet::Intern({place, health}));
	engine::core::ByteReader again(writer.Bytes());
	REQUIRE(confused.Read(again, 1, reversed));

	CHECK(static_cast<const Place *>(confused.Find(place)->At(0))->X != 4.5f);
}
