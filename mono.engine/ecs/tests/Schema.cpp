#include <engine/core/Bytes.hpp>
#include <engine/ecs/Schema.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <span>
#include <string>
#include <vector>

TEST_SUITE_ID("engine.ecs.schema")

using engine::core::Name;
using engine::ecs::ComponentId;
using engine::ecs::Components;
using engine::ecs::Entity;
using engine::ecs::FieldSpec;
using engine::ecs::PropertyType;
using engine::ecs::Schema;
using engine::ecs::Schemas;
using engine::ecs::Store;

namespace {
	// The component table is process-wide and nothing unregisters, so every
	// case here has to name something no other case and no engine module will.
	// A counter is not enough on its own — the suites in one binary share the
	// table — so the prefix is this file's.
	std::string Unique(const char *what) {
		static int counter = 0;
		return std::string("engine.ecs.schema.test.") + what + "." + std::to_string(counter++);
	}

	ComponentId Describe(const std::string &name, std::span<const FieldSpec> fields) {
		const Schemas::Result result = Schemas::Register(name, fields);
		REQUIRE(result.Why == Schemas::Status::Ok);
		REQUIRE(result.Id.IsValid());
		return result.Id;
	}
}

TEST_CASE("a described component registers and is reachable by name", "[schema]") {
	const std::string name = Unique("health");
	const FieldSpec fields[] = {
		{"Current", PropertyType::Double},
		{"Max", PropertyType::Double},
	};

	const Schemas::Result first = Schemas::Register(name, fields);
	REQUIRE(first.Why == Schemas::Status::Ok);
	REQUIRE(first.Created);

	// The id comes from the same counter every declared component uses, which
	// is what lets a described one sort into an archetype beside a declared one.
	REQUIRE(Components::Find(Name(name)) == first.Id);
	REQUIRE(Components::Describe(first.Id).Name == Name(name));

	const Schema *schema = Schemas::Of(first.Id);
	REQUIRE(schema != nullptr);
	REQUIRE(schema->Name() == Name(name));
	REQUIRE(schema->Fields().size() == 2);
	REQUIRE(schema->Find("Current") != nullptr);
	REQUIRE(schema->Find("Missing") == nullptr);
	REQUIRE(Schemas::Find(Name(name)) == schema);
}

TEST_CASE("registering the same fields again agrees rather than conflicts", "[schema]") {
	const std::string name = Unique("agree");
	const FieldSpec declared[] = {
		{"A", PropertyType::Float},
		{"B", PropertyType::Int32},
	};

	const Schemas::Result first = Schemas::Register(name, declared);
	REQUIRE(first.Created);

	// **Order is not part of the identity**, because the layout is derived
	// rather than taken from the caller. Two scripts declaring one component
	// are agreeing.
	const FieldSpec reversed[] = {
		{"B", PropertyType::Int32},
		{"A", PropertyType::Float},
	};

	const Schemas::Result second = Schemas::Register(name, reversed);
	REQUIRE(second.Why == Schemas::Status::Ok);
	REQUIRE_FALSE(second.Created);
	REQUIRE(second.Id == first.Id);
}

TEST_CASE("a different field set under one name is refused", "[schema]") {
	const std::string name = Unique("conflict");
	const FieldSpec declared[] = {{"A", PropertyType::Float}};
	REQUIRE(Schemas::Register(name, declared).Why == Schemas::Status::Ok);

	const FieldSpec widened[] = {{"A", PropertyType::Float}, {"B", PropertyType::Float}};
	const Schemas::Result second = Schemas::Register(name, widened);
	REQUIRE(second.Why == Schemas::Status::Conflict);
	REQUIRE_FALSE(second.Id.IsValid());

	const FieldSpec retyped[] = {{"A", PropertyType::Int32}};
	REQUIRE(Schemas::Register(name, retyped).Why == Schemas::Status::Conflict);
}

TEST_CASE("a name a C++ type already holds is a conflict, not an abort", "[schema]") {
	// `Entity` is registered by the storage itself long before this runs, under
	// whatever `TypeNameOf` spells — so instead this registers a type here and
	// then tries to describe over it. Aborting would be right for two C++ types
	// and wrong for a script that mistyped a name.
	struct Occupied {
		float Value = 0.0f;
	};

	const std::string name = Unique("occupied");
	Components::Register<Occupied>(name);

	const FieldSpec fields[] = {{"Value", PropertyType::Float}};
	REQUIRE(Schemas::Register(name, fields).Why == Schemas::Status::Conflict);
}

TEST_CASE("a field the storage cannot hold is refused", "[schema]") {
	const std::string name = Unique("bad");

	const FieldSpec opaque[] = {{"A", PropertyType::Opaque}};
	REQUIRE(Schemas::Register(name, opaque).Why == Schemas::Status::BadField);

	const FieldSpec unnamed[] = {{"", PropertyType::Float}};
	REQUIRE(Schemas::Register(name, unnamed).Why == Schemas::Status::BadField);

	const FieldSpec twice[] = {{"A", PropertyType::Float}, {"A", PropertyType::Int32}};
	REQUIRE(Schemas::Register(name, twice).Why == Schemas::Status::DuplicateField);

	REQUIRE(Schemas::Register("", opaque).Why == Schemas::Status::Unnamed);

	// None of the refusals above registered anything, so the name is still free.
	const FieldSpec good[] = {{"A", PropertyType::Float}};
	REQUIRE(Schemas::Register(name, good).Why == Schemas::Status::Ok);
}

TEST_CASE("the layout is widest-first and leaves no gaps a field could not use", "[schema]") {
	const std::string name = Unique("layout");
	const FieldSpec fields[] = {
		{"Flag", PropertyType::Bool},
		{"Wide", PropertyType::Double},
		{"Count", PropertyType::Int32},
		{"Where", PropertyType::Vector3},
	};

	const Schema *schema = Schemas::Of(Describe(name, fields));
	REQUIRE(schema != nullptr);

	// Every field is aligned for its own type and none overlaps the next.
	uint32_t reached = 0;
	for (const auto &field : schema->Fields()) {
		REQUIRE(field.Offset >= reached);
		REQUIRE(field.Offset + field.Size <= schema->Size());
		reached = field.Offset + field.Size;
	}

	// The double comes first because it is the widest, whatever order the
	// caller named things in.
	REQUIRE(schema->Fields().front().Spelling == "Wide");
	REQUIRE(schema->Find("Wide")->Offset == 0);

	// The blob is a whole number of values wide, so an array of them keeps
	// every field aligned.
	REQUIRE(schema->Size() % schema->Alignment() == 0);
}

TEST_CASE("a described component is stored, read back and queried", "[schema]") {
	const std::string name = Unique("stored");
	const FieldSpec fields[] = {
		{"Current", PropertyType::Double},
		{"Max", PropertyType::Double},
	};

	const ComponentId id = Describe(name, fields);
	const Schema *schema = Schemas::Of(id);

	Store store("schema-store");

	const Entity alive = store.Create();
	const Entity empty = store.Create();

	std::vector<std::byte> value(schema->Size());
	Components::Describe(id).DefaultConstruct(value.data(), 1);
	*reinterpret_cast<double *>(value.data() + schema->Find("Current")->Offset) = 30.0;
	*reinterpret_cast<double *>(value.data() + schema->Find("Max")->Offset) = 100.0;

	store.SetComponent(alive, id, value.data());

	REQUIRE(store.HasComponent(alive, id));
	REQUIRE_FALSE(store.HasComponent(empty, id));

	const void *held = store.GetComponent(alive, id);
	REQUIRE(held != nullptr);
	REQUIRE(
		*reinterpret_cast<const double *>(
			static_cast<const std::byte *>(held) + schema->Find("Current")->Offset
		) == 30.0
	);

	const ComponentId terms[] = {id};
	std::vector<Entity> found;
	store.EachMatching(terms, [&found](Entity entity) { found.push_back(entity); });

	REQUIRE(found.size() == 1);
	REQUIRE(found.front() == alive);
	REQUIRE(store.CountMatching(terms) == 1);

	store.RemoveComponent(alive, id);
	REQUIRE(store.CountMatching(terms) == 0);

	Components::Describe(id).Destruct(value.data(), 1);
}

TEST_CASE("an empty query matches nothing rather than everything", "[schema]") {
	Store store("schema-empty-query");
	store.Create();
	store.Create();

	size_t visited = 0;
	store.EachMatching({}, [&visited](Entity) { visited++; });

	REQUIRE(visited == 0);
	REQUIRE(store.CountMatching({}) == 0);
}

TEST_CASE("a query naming one component twice is the same query", "[schema]") {
	const std::string name = Unique("dedupe");
	const FieldSpec fields[] = {{"A", PropertyType::Float}};
	const ComponentId id = Describe(name, fields);
	const Schema *schema = Schemas::Of(id);

	Store store("schema-dedupe");
	const Entity entity = store.Create();

	std::vector<std::byte> value(schema->Size());
	Components::Describe(id).DefaultConstruct(value.data(), 1);
	store.SetComponent(entity, id, value.data());

	const ComponentId twice[] = {id, id};
	REQUIRE(store.CountMatching(twice) == 1);

	Components::Describe(id).Destruct(value.data(), 1);
}

TEST_CASE("a string field survives a copy, a move and a serialisation", "[schema]") {
	const std::string name = Unique("text");
	const FieldSpec fields[] = {
		{"Label", PropertyType::String},
		{"Kind", PropertyType::Name},
		{"Score", PropertyType::Int32},
	};

	const ComponentId id = Describe(name, fields);
	const Schema *schema = Schemas::Of(id);
	const auto &descriptor = Components::Describe(id);

	// A string field is what makes a described component non-trivial: the
	// storage may not memcpy it, and the four lifetime hooks have to be real.
	REQUIRE_FALSE(descriptor.Trivial);
	REQUIRE(descriptor.Serialisable);

	const uint32_t label = schema->Find("Label")->Offset;
	const uint32_t kind = schema->Find("Kind")->Offset;
	const uint32_t score = schema->Find("Score")->Offset;

	std::vector<std::byte> source(schema->Size());
	descriptor.DefaultConstruct(source.data(), 1);
	*reinterpret_cast<std::string *>(source.data() + label) = "a score that changes";
	*reinterpret_cast<Name *>(source.data() + kind) = Name("Counter");
	*reinterpret_cast<int32_t *>(source.data() + score) = 7;

	std::vector<std::byte> copy(schema->Size());
	descriptor.CopyConstruct(copy.data(), source.data(), 1);
	REQUIRE(*reinterpret_cast<const std::string *>(copy.data() + label) == "a score that changes");
	REQUIRE(*reinterpret_cast<const int32_t *>(copy.data() + score) == 7);

	// **The name crosses as text**, which is the whole reason a described
	// component is serialised field by field rather than as its bytes: a
	// `core::Name`'s id is process-local and must never reach a file.
	engine::core::ByteWriter writer;
	descriptor.Write(writer, source.data(), 1);

	std::vector<std::byte> restored(schema->Size());
	descriptor.DefaultConstruct(restored.data(), 1);
	engine::core::ByteReader reader(writer.Bytes());
	descriptor.Read(reader, restored.data(), 1);

	REQUIRE_FALSE(reader.Failed());
	REQUIRE(*reinterpret_cast<const std::string *>(restored.data() + label) == "a score that changes");
	REQUIRE(*reinterpret_cast<const Name *>(restored.data() + kind) == Name("Counter"));
	REQUIRE(*reinterpret_cast<const int32_t *>(restored.data() + score) == 7);

	descriptor.Destruct(restored.data(), 1);
	descriptor.Destruct(copy.data(), 1);
	descriptor.Destruct(source.data(), 1);
}

TEST_CASE("a described component's padding is zeroed rather than left alone", "[schema]") {
	// The layout below pads: a bool followed by nothing wide enough to fill the
	// tail. `TypeDescriptor` warns that uninitialised padding makes two runs of
	// one scene produce different bytes, and a derived layout cannot promise
	// there is no padding — so it promises the padding is defined.
	const std::string name = Unique("padding");
	const FieldSpec fields[] = {
		{"Wide", PropertyType::Double},
		{"Flag", PropertyType::Bool},
	};

	const ComponentId id = Describe(name, fields);
	const Schema *schema = Schemas::Of(id);
	REQUIRE(schema->Size() > 9);

	std::vector<std::byte> first(schema->Size(), std::byte{0xAB});
	std::vector<std::byte> second(schema->Size(), std::byte{0x5C});

	const auto &descriptor = Components::Describe(id);
	descriptor.DefaultConstruct(first.data(), 1);
	descriptor.DefaultConstruct(second.data(), 1);

	REQUIRE(std::memcmp(first.data(), second.data(), schema->Size()) == 0);

	descriptor.Destruct(second.data(), 1);
	descriptor.Destruct(first.data(), 1);
}

TEST_CASE("many described components each keep their own hooks", "[schema]") {
	// One hook set is generated per slot, so a mistake in the table would show
	// up as two schemas sharing a layout. Ten is enough to catch an off-by-one
	// without registering a quarter of the process's budget.
	// Each schema is one field wider than the last, so a hook that reached the
	// wrong schema reads a stride nobody expects rather than a value that
	// happens to look right.
	std::vector<std::string> names;
	for (int index = 0; index < 10; index++) {
		names.push_back("F" + std::to_string(index));
	}

	std::vector<const Schema *> schemas;
	for (size_t width = 1; width <= names.size(); width++) {
		std::vector<FieldSpec> fields;
		fields.reserve(width);
		for (size_t at = 0; at < width; at++) {
			fields.push_back(FieldSpec{names[at], PropertyType::Double, {}});
		}

		const Schema *schema = Schemas::Of(Describe(Unique("many"), fields));
		REQUIRE(schema != nullptr);
		REQUIRE(schema->Size() == sizeof(double) * width);
		schemas.push_back(schema);
	}

	for (size_t at = 1; at < schemas.size(); at++) {
		REQUIRE(schemas[at]->Size() > schemas[at - 1]->Size());
		REQUIRE(schemas[at]->Name() != schemas[at - 1]->Name());
	}
}
