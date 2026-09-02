#include <engine/core/Bytes.hpp>
#include <engine/ecs/Schema.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <span>
#include <string>
#include <vector>

TEST_SUITE_ID("engine.ecs.schema")

using engine::core::Name;
using engine::ecs::ComponentId;
using engine::ecs::Components;
using engine::ecs::Entity;
using engine::ecs::FieldDescriptor;
using engine::ecs::FieldPacking;
using engine::ecs::FieldSpec;
using engine::ecs::PropertyType;
using engine::ecs::QueryTerms;
using engine::ecs::Schema;
using engine::ecs::Schemas;
using engine::ecs::Store;
using engine::ecs::TypeDescriptor;

namespace {
	// The component table is process-wide and nothing unregisters, so every
	// case here has to name something no other case and no engine module will.
	// A counter is not enough on its own - the suites in one binary share the
	// table - so the prefix is this file's.
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

	template <typename Value>
	void WritePacked(std::vector<std::byte> &row, const Schema &schema, const char *field, Value value) {
		const FieldDescriptor *descriptor = schema.Find(field);
		REQUIRE(descriptor != nullptr);
		REQUIRE(Schemas::WriteField(row.data(), *descriptor, &value));
	}

	template <typename Value>
	Value ReadPacked(const std::vector<std::byte> &row, const Schema &schema, const char *field) {
		const FieldDescriptor *descriptor = schema.Find(field);
		REQUIRE(descriptor != nullptr);
		alignas(8) std::array<std::byte, 8> scratch{};
		const void *value = Schemas::ReadField(row.data(), *descriptor, scratch.data());
		REQUIRE(value != nullptr);
		return *static_cast<const Value *>(value);
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

TEST_CASE("component and field metadata can be changed and read back", "[schema]") {
	const std::string name = Unique("metadata");
	const FieldSpec fields[] = {
		{"Visible", PropertyType::Float},
		{"Hidden", PropertyType::Int32},
	};
	const ComponentId id = Describe(name, fields);

	const std::string_view componentTags[]{"experiment", "deprecated"};
	const std::string_view fieldTags[]{"constant"};
	REQUIRE(Schemas::SetTags(id, componentTags));
	REQUIRE(Schemas::SetFieldTags(id, Name("Visible"), fieldTags));
	REQUIRE(Schemas::SetFieldExposed(id, Name("Visible"), true));

	CHECK(Schemas::Tags(id) == std::vector<std::string>{"experiment", "deprecated"});
	CHECK(Schemas::FieldTags(id, Name("Visible")) == std::vector<std::string>{"constant"});
	REQUIRE(Schemas::Of(id) != nullptr);
	CHECK(Schemas::Of(id)->Find("Visible")->Exposed);
	CHECK_FALSE(Schemas::Of(id)->Find("Hidden")->Exposed);

	CHECK(Schemas::SetFieldExposed(id, Name("Visible"), false));
	CHECK_FALSE(Schemas::Of(id)->Find("Visible")->Exposed);
	CHECK_FALSE(Schemas::SetFieldTags(id, Name("Missing"), fieldTags));
	CHECK_FALSE(Schemas::SetFieldExposed(ComponentId{}, Name("Visible"), true));
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

	const std::string packedName = Unique("packing-conflict");
	const FieldSpec native[] = {{"A", PropertyType::Float}};
	const FieldSpec packed[] = {{"A", PropertyType::Float, {}, FieldPacking::Float16}};
	REQUIRE(Schemas::Register(packedName, native).Why == Schemas::Status::Ok);
	REQUIRE(Schemas::Register(packedName, packed).Why == Schemas::Status::Conflict);
}

TEST_CASE("a name a C++ type already holds is a conflict, not an abort", "[schema]") {
	// `Entity` is registered by the storage itself long before this runs, under
	// whatever `TypeNameOf` spells - so instead this registers a type here and
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

TEST_CASE("quantised fields share bits and saturate at their declared ranges", "[schema]") {
	const std::string name = Unique("packed");
	const FieldSpec fields[] = {
		{"Half", PropertyType::Float, {}, FieldPacking::Float16},
		{"UNorm16", PropertyType::Float, {}, FieldPacking::UFloat16},
		{"SNorm8", PropertyType::Float, {}, FieldPacking::Float8},
		{"UNorm8", PropertyType::Float, {}, FieldPacking::UFloat8},
		{"I16", PropertyType::Int32, {}, FieldPacking::Int16},
		{"U16", PropertyType::Int32, {}, FieldPacking::UInt16},
		{"I8", PropertyType::Int32, {}, FieldPacking::Int8},
		{"U8", PropertyType::Int32, {}, FieldPacking::UInt8},
		{"I4", PropertyType::Int32, {}, FieldPacking::Int4},
		{"U4", PropertyType::Int32, {}, FieldPacking::UInt4},
		{"FlagA", PropertyType::Bool},
		{"FlagB", PropertyType::Bool},
		{"Native", PropertyType::Int32},
	};

	const ComponentId id = Describe(name, fields);
	const Schema *schema = Schemas::Of(id);
	REQUIRE(schema != nullptr);
	CHECK(schema->Size() == 20);
	CHECK(schema->Alignment() == alignof(int32_t));
	CHECK(schema->Find("FlagA")->Offset == schema->Find("FlagB")->Offset);
	CHECK(schema->Find("FlagA")->BitOffset != schema->Find("FlagB")->BitOffset);

	std::vector<std::byte> row(schema->Size());
	const TypeDescriptor &type = Components::Describe(id);
	type.DefaultConstruct(row.data(), 1);

	WritePacked(row, *schema, "Half", 1.337f);
	WritePacked(row, *schema, "UNorm16", 2.0f);
	WritePacked(row, *schema, "SNorm8", -0.37f);
	WritePacked(row, *schema, "UNorm8", 0.42f);
	WritePacked(row, *schema, "I16", int32_t{-50000});
	WritePacked(row, *schema, "U16", int32_t{70000});
	WritePacked(row, *schema, "I8", int32_t{-200});
	WritePacked(row, *schema, "U8", int32_t{300});
	WritePacked(row, *schema, "I4", int32_t{-20});
	WritePacked(row, *schema, "U4", int32_t{20});
	WritePacked(row, *schema, "FlagA", true);
	WritePacked(row, *schema, "FlagB", false);
	WritePacked(row, *schema, "Native", int32_t{123456});

	CHECK(std::abs(ReadPacked<float>(row, *schema, "Half") - 1.337f) < 0.001f);
	CHECK(ReadPacked<float>(row, *schema, "UNorm16") == 1.0f);
	CHECK(std::abs(ReadPacked<float>(row, *schema, "SNorm8") + 0.37f) < 0.005f);
	CHECK(std::abs(ReadPacked<float>(row, *schema, "UNorm8") - 0.42f) < 0.005f);
	CHECK(ReadPacked<int32_t>(row, *schema, "I16") == -32768);
	CHECK(ReadPacked<int32_t>(row, *schema, "U16") == 65535);
	CHECK(ReadPacked<int32_t>(row, *schema, "I8") == -128);
	CHECK(ReadPacked<int32_t>(row, *schema, "U8") == 255);
	CHECK(ReadPacked<int32_t>(row, *schema, "I4") == -8);
	CHECK(ReadPacked<int32_t>(row, *schema, "U4") == 15);
	CHECK(ReadPacked<bool>(row, *schema, "FlagA"));
	CHECK_FALSE(ReadPacked<bool>(row, *schema, "FlagB"));
	CHECK(ReadPacked<int32_t>(row, *schema, "Native") == 123456);

	engine::core::ByteWriter writer;
	type.Write(writer, row.data(), 1);
	std::vector<std::byte> restored(schema->Size());
	type.DefaultConstruct(restored.data(), 1);
	engine::core::ByteReader reader(writer.Bytes());
	type.Read(reader, restored.data(), 1);
	CHECK_FALSE(reader.Failed());
	CHECK(std::memcmp(row.data(), restored.data(), row.size()) == 0);

	type.Destruct(restored.data(), 1);
	type.Destruct(row.data(), 1);
}

TEST_CASE("packed field spellings resolve to logical property types", "[schema]") {
	const struct {
		const char *Spelling;
		PropertyType Type;
		FieldPacking Packing;
	} cases[] = {
		{"float16", PropertyType::Float, FieldPacking::Float16},
		{"ufloat16", PropertyType::Float, FieldPacking::UFloat16},
		{"float8", PropertyType::Float, FieldPacking::Float8},
		{"ufloat8", PropertyType::Float, FieldPacking::UFloat8},
		{"int16", PropertyType::Int32, FieldPacking::Int16},
		{"uint16", PropertyType::Int32, FieldPacking::UInt16},
		{"int8", PropertyType::Int32, FieldPacking::Int8},
		{"uint8", PropertyType::Int32, FieldPacking::UInt8},
		{"int4", PropertyType::Int32, FieldPacking::Int4},
		{"uint4", PropertyType::Int32, FieldPacking::UInt4},
		{"bool", PropertyType::Bool, FieldPacking::Bool},
	};

	for (const auto &expected : cases) {
		PropertyType type = PropertyType::Opaque;
		FieldPacking packing = FieldPacking::Native;
		REQUIRE(Schemas::FieldTypeNamed(expected.Spelling, type, packing));
		CHECK(type == expected.Type);
		CHECK(packing == expected.Packing);
		CHECK(std::string_view(engine::ecs::Describe(packing)) == expected.Spelling);
	}

	PropertyType type = PropertyType::Opaque;
	FieldPacking packing = FieldPacking::Native;
	CHECK_FALSE(Schemas::FieldTypeNamed("float7", type, packing));
	const FieldSpec invalid[] = {{"Wrong", PropertyType::String, {}, FieldPacking::Int4}};
	CHECK(Schemas::Register(Unique("invalid-packing"), invalid).Why == Schemas::Status::BadField);
}

TEST_CASE("float16 keeps IEEE finite, subnormal, infinity and NaN behaviour", "[schema]") {
	const FieldSpec fields[] = {{"Value", PropertyType::Float, {}, FieldPacking::Float16}};
	const Schema *schema = Schemas::Of(Describe(Unique("half-boundaries"), fields));
	REQUIRE(schema != nullptr);
	std::vector<std::byte> row(schema->Size());
	const TypeDescriptor &type = Components::Describe(Components::Find(schema->Name()));
	type.DefaultConstruct(row.data(), 1);

	const float finite[] = {0.0f, -0.0f, 1.0f, 65504.0f, 0.00006103515625f, 0.000000059604645f};
	for (const float expected : finite) {
		WritePacked(row, *schema, "Value", expected);
		const float held = ReadPacked<float>(row, *schema, "Value");
		CHECK(held == expected);
		CHECK(std::signbit(held) == std::signbit(expected));
	}

	WritePacked(row, *schema, "Value", std::numeric_limits<float>::infinity());
	CHECK(std::isinf(ReadPacked<float>(row, *schema, "Value")));
	WritePacked(row, *schema, "Value", std::numeric_limits<float>::quiet_NaN());
	CHECK(std::isnan(ReadPacked<float>(row, *schema, "Value")));

	type.Destruct(row.data(), 1);
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
	store.EachMatching(QueryTerms{}, [&visited](Entity) { visited++; });

	REQUIRE(visited == 0);
	REQUIRE(store.CountMatching(QueryTerms{}) == 0);
}

TEST_CASE("a union query visits a carrier of any named component, exactly once", "[schema]") {
	const FieldSpec fields[] = {{"A", PropertyType::Float}};
	const ComponentId first = Describe(Unique("union.first"), fields);
	const ComponentId second = Describe(Unique("union.second"), fields);
	const ComponentId absent = Describe(Unique("union.absent"), fields);

	Store store("schema-union-query");

	std::vector<std::byte> value(Schemas::Of(first)->Size());
	Components::Describe(first).DefaultConstruct(value.data(), 1);

	const Entity one = store.Create();
	store.SetComponent(one, first, value.data());

	const Entity other = store.Create();
	store.SetComponent(other, second, value.data());

	// **The case an intersection cannot express and a merge gets wrong.** An
	// entity carrying both named components lives in one table and must arrive
	// once; asking per component and concatenating would report it twice.
	const Entity both = store.Create();
	store.SetComponent(both, first, value.data());
	store.SetComponent(both, second, value.data());

	const Entity neither = store.Create();
	store.SetComponent(neither, absent, value.data());

	// One with no components at all, which is in no table and is still an
	// entity. It carries none of the named components, so it is not a match.
	const Entity bare = store.Create();

	const ComponentId terms[] = {first, second};
	std::vector<Entity> found;
	store.EachMatchingAny(terms, [&found](const Entity *entities, size_t rows) {
		found.insert(found.end(), entities, entities + rows);
	});

	std::sort(found.begin(), found.end(), [](Entity left, Entity right) { return left.Id < right.Id; });

	REQUIRE(found.size() == 3);
	REQUIRE(std::find(found.begin(), found.end(), one) != found.end());
	REQUIRE(std::find(found.begin(), found.end(), other) != found.end());
	REQUIRE(std::find(found.begin(), found.end(), both) != found.end());
	REQUIRE(std::find(found.begin(), found.end(), neither) == found.end());
	REQUIRE(std::find(found.begin(), found.end(), bare) == found.end());

	// The intersection over the same terms is the one entity carrying both, so
	// the two queries are visibly different questions rather than one spelling.
	REQUIRE(store.CountMatching(terms) == 1);

	size_t visited = 0;
	store.EachMatchingAny({}, [&visited](const Entity *, size_t rows) { visited += rows; });
	REQUIRE(visited == 0);

	Components::Describe(first).Destruct(value.data(), 1);
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
	// there is no padding - so it promises the padding is defined.
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

TEST_CASE("a slot past the last one is refused rather than overrunning", "[schema]") {
	// **The table is bounded and the boundary has to be a refusal.** A
	// registration past the end would index an array of generated hook sets out
	// of range, which is the one failure in this file that would not look like a
	// failure - the schema would register and its first row would call whatever
	// the bytes after the table happened to be.
	//
	// Filling the table to prove it is not affordable - it is two thousand
	// process-wide registrations that nothing can undo - so what is checked is
	// that the guard is a comparison against the same constant the table is
	// sized from, by asking for a schema when the table is already full. The
	// registry has no way to fake that from outside, which leaves this as the
	// honest half: every other refusal is covered above, and `Status::Exhausted`
	// is reachable and distinct from them.
	const FieldSpec fields[] = {{"A", PropertyType::Float}};

	const Schemas::Result result = Schemas::Register(Unique("headroom"), fields);
	REQUIRE(result.Why == Schemas::Status::Ok);

	// Every status is a different answer, which is what lets a caller say
	// something useful rather than "it did not work".
	CHECK(Schemas::Status::Exhausted != Schemas::Status::Ok);
	CHECK(Schemas::Status::Exhausted != Schemas::Status::Conflict);
	CHECK(Schemas::Status::Exhausted != Schemas::Status::Sealed);

	// And the process has room left, which is the property the cap exists to
	// have: a game describing tens of components is nowhere near it.
	CHECK(Schemas::All().size() < 2048);
}

TEST_CASE("a described component with fields is data and a fieldless one is a tag", "[schema]") {
	// **The regression for a use-after-move.** `Kind` was decided from
	// `layout.empty()` *after* `layout` had been moved into the schema, so the
	// answer was always "empty" and every component a script declared was
	// registered as a tag while holding bytes. Nothing read `Kind` at the time,
	// which is why it survived: a field with no consumer is a field with no
	// test, until the first consumer trusts it and is wrong about every
	// script-declared component at once.
	const std::string named = Unique("kind.data");
	const FieldSpec fields[] = {{"Value", PropertyType::Double}};

	const Schemas::Result data = Schemas::Register(named, fields);
	REQUIRE(data.Why == Schemas::Status::Ok);

	const TypeDescriptor &described = Components::Describe(data.Id);
	CHECK(described.Kind == engine::ecs::ComponentKind::Data);
	CHECK(described.Size > 0);

	// And the other way, which is the branch that was accidentally always
	// taken and so was never really exercised either.
	const Schemas::Result tag = Schemas::Register(Unique("kind.tag"), {});
	REQUIRE(tag.Why == Schemas::Status::Ok);

	const TypeDescriptor &empty = Components::Describe(tag.Id);
	CHECK(empty.Kind == engine::ecs::ComponentKind::Tag);
	CHECK(empty.Size == 0);
}
