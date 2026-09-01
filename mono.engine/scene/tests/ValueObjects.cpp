// The concrete `ValueBase` family and its typed `Value` properties.
//
// Each leaf owns one component matching the type scripts see. These cases keep
// the class tree, property descriptors, defaults and snapshot form in step.

#include <engine/core/Bytes.hpp>
#include <engine/core/Name.hpp>
#include <engine/core/types/CFrame.hpp>
#include <engine/core/types/Color3.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>

TEST_SUITE_ID("engine.scene.valueobjects")

using engine::core::CFrame;
using engine::core::Color3;
using engine::core::Name;
using engine::core::Vector3;
using engine::ecs::Classes;
using engine::ecs::Entity;
using engine::ecs::NULL_ENTITY;
using engine::ecs::PropertyType;
using engine::ecs::Store;
using engine::scene::BoolValue;
using engine::scene::CFrameValue;
using engine::scene::Color3Value;
using engine::scene::IntValue;
using engine::scene::NumberValue;
using engine::scene::ObjectValue;
using engine::scene::RegisterSceneClasses;
using engine::scene::TextContent;
using engine::scene::Vector3Value;

namespace {
	Entity Make(Store &store, const char *klass) {
		const Entity instance = store.CreateInstance(Classes::Find(Name(klass)), klass);
		REQUIRE(instance != NULL_ENTITY);
		return instance;
	}

	template <class T> void Write(Store &store, Entity instance, const T &value) {
		REQUIRE(store.SetProperty(instance, Name("Value"), &value, sizeof(value)));
	}

	template <class T> T Read(const Store &store, Entity instance) {
		T value{};
		REQUIRE(store.GetProperty(instance, Name("Value"), &value, sizeof(value)));
		return value;
	}

	PropertyType ValueType(const char *klass) {
		const auto &info = Classes::Describe(Classes::Find(Name(klass)));
		for (const auto &property : info.Properties) {
			if (property.Name == Name("Value")) {
				return property.Type;
			}
		}
		FAIL("class has no Value property");
		return PropertyType::Opaque;
	}
}

TEST_CASE("value classes descend from ValueBase and expose exact property types", "[scene][valueobjects]") {
	RegisterSceneClasses();
	const auto valueBase = Classes::Find(Name("ValueBase"));
	REQUIRE(valueBase.IsValid());

	struct Expected {
		const char *Class;
		PropertyType Type;
	};
	const Expected expected[] = {
		{"BoolValue", PropertyType::Bool},
		{"CFrameValue", PropertyType::CFrame},
		{"Color3Value", PropertyType::Color3},
		{"IntValue", PropertyType::Int64},
		{"NumberValue", PropertyType::Double},
		{"ObjectValue", PropertyType::Reference},
		{"StringValue", PropertyType::String},
		{"Vector3Value", PropertyType::Vector3},
	};

	for (const Expected &entry : expected) {
		INFO(entry.Class);
		const auto klass = Classes::Find(Name(entry.Class));
		REQUIRE(klass.IsValid());
		CHECK(Classes::IsA(klass, valueBase));
		CHECK(ValueType(entry.Class) == entry.Type);
	}
}

TEST_CASE("fresh value objects carry only their matching value component", "[scene][valueobjects]") {
	RegisterSceneClasses();
	Store store("value_objects.defaults");

	const Entity boolean = Make(store, "BoolValue");
	CHECK(store.Has<BoolValue>(boolean));
	CHECK_FALSE(store.Has<TextContent>(boolean));
	CHECK_FALSE(Read<bool>(store, boolean));

	const Entity integer = Make(store, "IntValue");
	CHECK(store.Has<IntValue>(integer));
	CHECK_FALSE(store.Has<NumberValue>(integer));
	CHECK(Read<int64_t>(store, integer) == 0);

	const Entity text = Make(store, "StringValue");
	CHECK(store.Has<TextContent>(text));
	CHECK(Read<std::string>(store, text).empty());

	const Entity object = Make(store, "ObjectValue");
	CHECK(store.Has<ObjectValue>(object));
	CHECK(Read<Entity>(store, object) == NULL_ENTITY);
}

TEST_CASE("typed value properties round trip through a snapshot", "[scene][valueobjects]") {
	RegisterSceneClasses();
	Store source("value_objects.snapshot.source");

	const Entity target = Make(source, "Part");
	const Entity boolean = Make(source, "BoolValue");
	const Entity frame = Make(source, "CFrameValue");
	const Entity colour = Make(source, "Color3Value");
	const Entity integer = Make(source, "IntValue");
	const Entity number = Make(source, "NumberValue");
	const Entity object = Make(source, "ObjectValue");
	const Entity text = Make(source, "StringValue");
	const Entity vector = Make(source, "Vector3Value");

	Write(source, boolean, true);
	Write(source, frame, CFrame(Vector3(1.0f, 2.0f, 3.0f)));
	Write(source, colour, Color3{0.25f, 0.5f, 0.75f});
	Write(source, integer, int64_t{-9000000000LL});
	Write(source, number, 3.141592653589793);
	Write(source, object, target);
	Write(source, text, std::string("hello value"));
	Write(source, vector, Vector3{-4.0f, 5.0f, 6.0f});

	engine::core::ByteWriter writer;
	REQUIRE(source.Save(writer));

	Store restored("value_objects.snapshot.restored");
	engine::core::ByteReader reader(writer.Bytes());
	REQUIRE(restored.Load(reader));

	CHECK(Read<bool>(restored, boolean));
	CHECK(Read<CFrame>(restored, frame).Position == Vector3{1.0f, 2.0f, 3.0f});
	const Color3 restoredColour = Read<Color3>(restored, colour);
	CHECK(restoredColour.R == 0.25f);
	CHECK(restoredColour.G == 0.5f);
	CHECK(restoredColour.B == 0.75f);
	CHECK(Read<int64_t>(restored, integer) == -9000000000LL);
	CHECK(Read<double>(restored, number) == 3.141592653589793);
	CHECK(Read<Entity>(restored, object) == target);
	CHECK(Read<std::string>(restored, text) == "hello value");
	CHECK(Read<Vector3>(restored, vector) == Vector3{-4.0f, 5.0f, 6.0f});
}
