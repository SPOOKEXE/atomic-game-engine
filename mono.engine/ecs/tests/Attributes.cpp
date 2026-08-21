// Attributes, and the eighteen-case conversion that puts one in a save file.
//
// **The module had no suite of its own.** Attributes were covered from the
// scripting side, through both VMs, which exercises the two or three types a
// script test happens to use and none of the rest - and the half that has to be
// right for all eighteen is the serialisation, because an attribute survives a
// save and a wrong case there is a value that changes when a world is reloaded.
// That is the failure this file is aimed at, and it is the same one the
// component sweep in `engine.ecs.invariants` is aimed at one level down.
//
// Every allowed `PropertyType` is written, saved, loaded into a fresh world and
// compared, in one table rather than eighteen cases - a list nobody has to
// remember to extend is the only kind that stays complete.

#include <engine/core/Bytes.hpp>
#include <engine/ecs/Attributes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

TEST_SUITE_ID("engine.ecs.attributes")

using engine::core::ByteReader;
using engine::core::ByteWriter;
using engine::core::Name;
using engine::ecs::AttributeNames;
using engine::ecs::AttributeTypeAllowed;
using engine::ecs::AttributeValue;
using engine::ecs::ClearAttributes;
using engine::ecs::Entity;
using engine::ecs::GetAttribute;
using engine::ecs::PropertyType;
using engine::ecs::RegisterAttributeComponents;
using engine::ecs::SetAttribute;
using engine::ecs::Store;

namespace attributes_test {
	namespace core = engine::core;

	// One distinctive value per allowed type. Distinctive matters: a writer that
	// put a `Vector2` where a `UDim` belongs would still round-trip if both were
	// left at their defaults.
	std::vector<AttributeValue> EveryType() {
		std::vector<AttributeValue> values;

		const auto add = [&values](PropertyType type, auto fill) {
			AttributeValue value;
			value.Type = type;
			fill(value);
			values.push_back(std::move(value));
		};

		add(PropertyType::Bool, [](AttributeValue &v) { v.Bool = true; });
		add(PropertyType::Int32, [](AttributeValue &v) { v.Int32 = -1'234'567; });
		add(PropertyType::Int64, [](AttributeValue &v) { v.Int64 = -9'000'000'000LL; });
		add(PropertyType::Float, [](AttributeValue &v) { v.Float = 1.5f; });
		add(PropertyType::Double, [](AttributeValue &v) { v.Double = -2.25; });
		add(PropertyType::Name, [](AttributeValue &v) { v.Name = Name("attributes_test.interned"); });
		add(PropertyType::Enum, [](AttributeValue &v) { v.Name = Name("attributes_test.Member"); });
		add(PropertyType::String,
			[](AttributeValue &v) { v.String = std::string("a\0computed\0value", 16); });
		add(PropertyType::Vector3, [](AttributeValue &v) { v.Vector3 = core::Vector3{1.0f, 2.0f, 3.0f}; });
		add(PropertyType::Color3, [](AttributeValue &v) { v.Color3 = core::Color3{0.25f, 0.5f, 0.75f}; });
		add(PropertyType::CFrame, [](AttributeValue &v) {
			v.CFrame.Position = core::Vector3{4.0f, 5.0f, 6.0f};
			v.CFrame.QuaternionX = 0.1f;
			v.CFrame.QuaternionY = 0.2f;
			v.CFrame.QuaternionZ = 0.3f;
			v.CFrame.QuaternionW = 0.4f;
		});
		add(PropertyType::Vector2, [](AttributeValue &v) { v.Vector2 = core::Vector2{7.0f, 8.0f}; });
		add(PropertyType::UDim, [](AttributeValue &v) { v.UDim = core::UDim{0.5f, -9.0f}; });
		add(PropertyType::UDim2, [](AttributeValue &v) { v.UDim2 = core::UDim2{0.1f, 2.0f, 0.3f, 4.0f}; });
		add(PropertyType::Rect, [](AttributeValue &v) { v.Rect = core::Rect{1.0f, 2.0f, 3.0f, 4.0f}; });
		add(PropertyType::NumberRange,
			[](AttributeValue &v) { v.NumberRange = core::NumberRange{-5.0f, 5.0f}; });
		add(PropertyType::NumberSequence, [](AttributeValue &v) {
			v.NumberSequence.Add(core::NumberKeypoint{0.0f, 1.0f, 0.0f});
			v.NumberSequence.Add(core::NumberKeypoint{1.0f, 0.0f, 0.25f});
		});
		add(PropertyType::ColorSequence, [](AttributeValue &v) {
			v.ColorSequence.Add(core::ColorKeypoint{0.0f, core::Color3{1.0f, 0.0f, 0.0f}});
			v.ColorSequence.Add(core::ColorKeypoint{1.0f, core::Color3{0.0f, 0.0f, 1.0f}});
		});

		return values;
	}

	// Compares the field the type selects, and only that one. Every other field
	// is unspecified by `AttributeValue`'s own contract, so comparing the whole
	// struct would be asserting on bytes nobody promised.
	bool Same(const AttributeValue &left, const AttributeValue &right) {
		if (left.Type != right.Type) {
			return false;
		}

		switch (left.Type) {
		case PropertyType::Bool:
			return left.Bool == right.Bool;
		case PropertyType::Int32:
			return left.Int32 == right.Int32;
		case PropertyType::Int64:
			return left.Int64 == right.Int64;
		case PropertyType::Float:
			return left.Float == right.Float;
		case PropertyType::Double:
			return left.Double == right.Double;
		case PropertyType::Name:
		case PropertyType::Enum:
			return left.Name == right.Name;
		case PropertyType::String:
			return left.String == right.String;
		case PropertyType::Vector3:
			return left.Vector3.X == right.Vector3.X && left.Vector3.Y == right.Vector3.Y &&
				   left.Vector3.Z == right.Vector3.Z;
		case PropertyType::Color3:
			return left.Color3.R == right.Color3.R && left.Color3.G == right.Color3.G &&
				   left.Color3.B == right.Color3.B;
		case PropertyType::CFrame:
			return left.CFrame.Position.X == right.CFrame.Position.X &&
				   left.CFrame.Position.Y == right.CFrame.Position.Y &&
				   left.CFrame.Position.Z == right.CFrame.Position.Z &&
				   left.CFrame.QuaternionX == right.CFrame.QuaternionX &&
				   left.CFrame.QuaternionY == right.CFrame.QuaternionY &&
				   left.CFrame.QuaternionZ == right.CFrame.QuaternionZ &&
				   left.CFrame.QuaternionW == right.CFrame.QuaternionW;
		case PropertyType::Vector2:
			return left.Vector2.X == right.Vector2.X && left.Vector2.Y == right.Vector2.Y;
		case PropertyType::UDim:
			return left.UDim.Scale == right.UDim.Scale && left.UDim.Offset == right.UDim.Offset;
		case PropertyType::UDim2:
			return left.UDim2.X.Scale == right.UDim2.X.Scale && left.UDim2.X.Offset == right.UDim2.X.Offset &&
				   left.UDim2.Y.Scale == right.UDim2.Y.Scale && left.UDim2.Y.Offset == right.UDim2.Y.Offset;
		case PropertyType::Rect:
			return left.Rect.Min.X == right.Rect.Min.X && left.Rect.Min.Y == right.Rect.Min.Y &&
				   left.Rect.Max.X == right.Rect.Max.X && left.Rect.Max.Y == right.Rect.Max.Y;
		case PropertyType::NumberRange:
			return left.NumberRange.Minimum == right.NumberRange.Minimum &&
				   left.NumberRange.Maximum == right.NumberRange.Maximum;
		case PropertyType::NumberSequence: {
			if (left.NumberSequence.Count != right.NumberSequence.Count) {
				return false;
			}
			for (uint32_t index = 0; index < left.NumberSequence.Count; index++) {
				const auto &a = left.NumberSequence.Keypoints[index];
				const auto &b = right.NumberSequence.Keypoints[index];
				if (a.Time != b.Time || a.Value != b.Value || a.Envelope != b.Envelope) {
					return false;
				}
			}
			return true;
		}
		case PropertyType::ColorSequence: {
			if (left.ColorSequence.Count != right.ColorSequence.Count) {
				return false;
			}
			for (uint32_t index = 0; index < left.ColorSequence.Count; index++) {
				const auto &a = left.ColorSequence.Keypoints[index];
				const auto &b = right.ColorSequence.Keypoints[index];
				if (a.Time != b.Time || a.Value.R != b.Value.R || a.Value.G != b.Value.G ||
					a.Value.B != b.Value.B) {
					return false;
				}
			}
			return true;
		}
		case PropertyType::Reference:
		case PropertyType::Opaque:
			return true;
		}
		return false;
	}

	// A name per type, so one entity can carry the whole table at once.
	Name KeyFor(PropertyType type) {
		return Name(std::string("attribute.") + std::string(engine::ecs::Describe(type)));
	}
}

using namespace attributes_test;

TEST_CASE("every attribute type a world may hold survives a save", "[ecs][attributes]") {
	// **The case this file exists for.** An attribute reaches a save file
	// through an eighteen-case writer and an eighteen-case reader, and a pair
	// that disagrees about one of them is a value that changes when a world is
	// reloaded - which presents as a game whose settings drift and not as
	// anything a serialisation test would have been asked for.
	RegisterAttributeComponents();

	Store source("source");
	const Entity instance = source.Create();

	const std::vector<AttributeValue> values = EveryType();
	for (const AttributeValue &value : values) {
		INFO("setting " << engine::ecs::Describe(value.Type));
		REQUIRE(SetAttribute(source, instance, KeyFor(value.Type), value));
	}

	ByteWriter writer;
	REQUIRE(source.Save(writer));

	Store restored("restored");
	ByteReader reader(writer.Bytes());
	REQUIRE(restored.Load(reader));

	for (const AttributeValue &value : values) {
		INFO("reading back " << engine::ecs::Describe(value.Type));

		AttributeValue back;
		REQUIRE(GetAttribute(restored, instance, KeyFor(value.Type), back));
		CHECK(Same(value, back));
	}
}

TEST_CASE("every attribute type survives being read straight back", "[ecs][attributes]") {
	// The half above without the file in the way, so that a failure says which
	// of the two the fault is in.
	RegisterAttributeComponents();

	Store store("store");
	const Entity instance = store.Create();

	for (const AttributeValue &value : EveryType()) {
		INFO(engine::ecs::Describe(value.Type));
		REQUIRE(SetAttribute(store, instance, KeyFor(value.Type), value));

		AttributeValue back;
		REQUIRE(GetAttribute(store, instance, KeyFor(value.Type), back));
		CHECK(Same(value, back));
	}
}

TEST_CASE("a reference is refused because it means nothing in another world", "[ecs][attributes]") {
	// The one refusal `AttributeTypeAllowed` makes, and its reason: an
	// `ecs::Entity` is a handle within one world and an attribute survives a
	// save file, so storing one would write a number that means a different row
	// when it is read back.
	RegisterAttributeComponents();

	CHECK_FALSE(AttributeTypeAllowed(PropertyType::Reference));
	CHECK_FALSE(AttributeTypeAllowed(PropertyType::Opaque));
	CHECK(AttributeTypeAllowed(PropertyType::String));

	Store store("store");
	const Entity instance = store.Create();

	AttributeValue reference;
	reference.Type = PropertyType::Reference;
	CHECK_FALSE(SetAttribute(store, instance, Name("Target"), reference));
}

TEST_CASE("setting nothing is how an attribute is taken back", "[ecs][attributes]") {
	// Roblox's `SetAttribute(name, nil)`, and the only spelling there is - a
	// separate remove would be a second way to say it, and the two would drift.
	RegisterAttributeComponents();

	Store store("store");
	const Entity instance = store.Create();

	AttributeValue value;
	value.Type = PropertyType::Int32;
	value.Int32 = 5;
	REQUIRE(SetAttribute(store, instance, Name("Score"), value));

	AttributeValue nothing;
	nothing.Type = PropertyType::Opaque;
	REQUIRE(SetAttribute(store, instance, Name("Score"), nothing));

	AttributeValue back;
	CHECK_FALSE(GetAttribute(store, instance, Name("Score"), back));
	CHECK(AttributeNames(store, instance).empty());
}

TEST_CASE("writing a second type over the first replaces it rather than merging", "[ecs][attributes]") {
	// `AttributeValue` keeps every field and clears none of them, which is the
	// discipline its own header states. What must not survive is the *type*: a
	// reader trusting `Type` has to see the last thing written.
	RegisterAttributeComponents();

	Store store("store");
	const Entity instance = store.Create();

	AttributeValue number;
	number.Type = PropertyType::Float;
	number.Float = 3.5f;
	REQUIRE(SetAttribute(store, instance, Name("Setting"), number));

	AttributeValue text;
	text.Type = PropertyType::String;
	text.String = "now a string";
	REQUIRE(SetAttribute(store, instance, Name("Setting"), text));

	AttributeValue back;
	REQUIRE(GetAttribute(store, instance, Name("Setting"), back));
	CHECK(back.Type == PropertyType::String);
	CHECK(back.String == "now a string");
	CHECK(AttributeNames(store, instance).size() == 1);
}

TEST_CASE("names come back sorted rather than in hash order", "[ecs][attributes]") {
	// Both callers - a script iterating and a document being written - want the
	// same answer twice, which hash order does not promise between two runs.
	RegisterAttributeComponents();

	Store store("store");
	const Entity instance = store.Create();

	AttributeValue value;
	value.Type = PropertyType::Bool;
	value.Bool = true;

	for (const char *name : {"zebra", "apple", "mango", "banana"}) {
		REQUIRE(SetAttribute(store, instance, Name(name), value));
	}

	const std::vector<Name> names = AttributeNames(store, instance);
	REQUIRE(names.size() == 4);
	CHECK(names[0].Text() == "apple");
	CHECK(names[1].Text() == "banana");
	CHECK(names[2].Text() == "mango");
	CHECK(names[3].Text() == "zebra");
}

TEST_CASE("clearing drops every attribute and says how many", "[ecs][attributes]") {
	RegisterAttributeComponents();

	Store store("store");
	const Entity instance = store.Create();
	const Entity other = store.Create();

	AttributeValue value;
	value.Type = PropertyType::Int32;
	value.Int32 = 1;

	REQUIRE(SetAttribute(store, instance, Name("A"), value));
	REQUIRE(SetAttribute(store, instance, Name("B"), value));
	REQUIRE(SetAttribute(store, other, Name("A"), value));

	CHECK(ClearAttributes(store, instance) == 2);
	CHECK(AttributeNames(store, instance).empty());

	// The other instance keeps its own, which is what a map per entity buys and
	// what a single map keyed by a pair would have made easy to get wrong.
	CHECK(AttributeNames(store, other).size() == 1);

	// And clearing again is nothing rather than a second count.
	CHECK(ClearAttributes(store, instance) == 0);
}

TEST_CASE("a dead instance takes neither a read nor a write", "[ecs][attributes]") {
	RegisterAttributeComponents();

	Store store("store");
	const Entity instance = store.Create();

	AttributeValue value;
	value.Type = PropertyType::Bool;
	value.Bool = true;
	REQUIRE(SetAttribute(store, instance, Name("Live"), value));

	store.Destroy(instance);

	AttributeValue back;
	CHECK_FALSE(GetAttribute(store, instance, Name("Live"), back));
	CHECK_FALSE(SetAttribute(store, instance, Name("Live"), value));

	// And a fresh entity reusing the index does not inherit them, which is what
	// `StoreState::DropAttributes` is for.
	const Entity reused = store.Create();
	CHECK(AttributeNames(store, reused).empty());
}

TEST_CASE("a truncated attribute table is refused rather than half-read", "[ecs][attributes]") {
	// The hostile-input half. An attribute blob carries a length-prefixed string
	// and a keypoint count, both of which a corrupt file can lie about.
	RegisterAttributeComponents();

	Store source("source");
	const Entity instance = source.Create();

	AttributeValue value;
	value.Type = PropertyType::String;
	value.String = "long enough that cutting the file lands inside it";
	REQUIRE(SetAttribute(source, instance, Name("Text"), value));

	ByteWriter writer;
	REQUIRE(source.Save(writer));

	const std::span<const std::byte> saved = writer.Bytes();
	for (const size_t cut : {saved.size() / 4, saved.size() / 2, saved.size() - 1}) {
		INFO("cut at " << cut);

		Store target("target");
		ByteReader reader(saved.subspan(0, cut));
		CHECK_FALSE(target.Load(reader));

		// Empty rather than an entity with half a table on it.
		CHECK(target.TableCount() == 0);
	}
}

TEST_CASE("a recycled entity does not inherit the dead one's attributes", "[ecs][attributes]") {
	// **`AttributeTable::Entities` is keyed by 32 bits of a 64-bit handle**, and
	// this is the case that says why that is safe rather than lucky.
	//
	// `Entity::Id` carries an index in its low half and a generation in its
	// high half; the map key is `instance.Id` narrowed to `uint32_t`, so it is
	// the index alone and the generation is gone. Two *live* entities can never
	// collide - an index is only handed out once at a time - but a destroyed
	// entity's index is handed out again, and its row would be read by whoever
	// gets it next.
	//
	// What makes that not happen is `StoreState.cpp`'s `DropAttributes`, which
	// erases the row when the entity is destroyed. That is the invariant this
	// case pins: delete the hook and the narrowing becomes a real leak, and
	// nothing else in the suite would notice.
	RegisterAttributeComponents();

	Store store("store");

	const Entity first = store.Create();
	AttributeValue carried;
	carried.Type = PropertyType::Int32;
	carried.Int32 = 7;
	REQUIRE(SetAttribute(store, first, Name("Carried"), carried));

	store.Destroy(first);

	// Recreate until the directory hands the index back. It does so immediately
	// in practice; the loop is here so the case cannot hang on an allocator that
	// defers reuse.
	Entity second = store.Create();
	for (int attempt = 0; attempt < 64 && (second.Id & 0xFFFF'FFFFull) != (first.Id & 0xFFFF'FFFFull);
		 attempt++) {
		second = store.Create();
	}
	REQUIRE((second.Id & 0xFFFF'FFFFull) == (first.Id & 0xFFFF'FFFFull));
	REQUIRE(second != first);

	// The index matches and the generation does not, which is exactly the pair a
	// 32-bit key cannot tell apart.
	AttributeValue read;
	CHECK_FALSE(GetAttribute(store, second, Name("Carried"), read));
}
