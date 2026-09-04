#include <engine/core/Bytes.hpp>
#include <engine/core/Name.hpp>
#include <engine/core/types/CFrame.hpp>
#include <engine/core/types/Vector2.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/ecs/TypeDescriptor.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/VectorField.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>

TEST_SUITE_ID("engine.scene.vectorfield")

using engine::core::CFrame;
using engine::core::Name;
using engine::core::Vector2;
using engine::core::Vector3;
using engine::ecs::Classes;
using engine::ecs::Entity;
using engine::ecs::Store;
using engine::scene::ResolveVectorField;
using engine::scene::SampleVectorField;
using engine::scene::Transform;
using engine::scene::VectorField2D;
using engine::scene::VectorField3D;

TEST_CASE("a vector field selects its nearest ancestor", "[scene][vectorfield]") {
	engine::scene::RegisterSceneClasses();
	Store store("vector_field.ancestor");

	const Entity field = store.CreateInstance(Classes::Find(Name("VectorField2D")), "Field");
	const Entity child = store.CreateInstance(Classes::Find(Name("PVInstance")), "Child");
	REQUIRE(store.SetParent(child, field));

	VectorField2D *description = store.GetMutable<VectorField2D>(field);
	REQUIRE(description != nullptr);
	description->Vector = Vector2{2.0f, -3.0f};
	description->HalfExtent = Vector2{8.0f, 8.0f};

	const auto sample = ResolveVectorField(store, child);
	CHECK(sample.Source == field);
	CHECK(sample.TwoDimensional);
	CHECK(SampleVectorField(sample, Vector3::Zero) == Vector3{2.0f, 0.0f, -3.0f});
}

TEST_CASE("a bounded two dimensional field clamps and falls off", "[scene][vectorfield]") {
	engine::scene::RegisterSceneClasses();
	Store store("vector_field.bounds");

	const Entity field = store.CreateInstance(Classes::Find(Name("VectorField2D")), "Field");
	VectorField2D *description = store.GetMutable<VectorField2D>(field);
	REQUIRE(description != nullptr);
	description->Vector = Vector2{4.0f, 0.0f};
	description->HalfExtent = Vector2{10.0f, 10.0f};
	description->Falloff = 2.0f;

	const auto sample = ResolveVectorField(store, field);
	CHECK(SampleVectorField(sample, Vector3{8.0f, 0.0f, 0.0f}) == Vector3{4.0f, 0.0f, 0.0f});
	CHECK(SampleVectorField(sample, Vector3{9.0f, 0.0f, 0.0f}) == Vector3{2.0f, 0.0f, 0.0f});
	CHECK(SampleVectorField(sample, Vector3{10.1f, 0.0f, 0.0f}) == Vector3::Zero);
}

TEST_CASE("a three dimensional field has radial and tangential terms", "[scene][vectorfield]") {
	engine::scene::RegisterSceneClasses();
	Store store("vector_field.terms");

	const Entity field = store.CreateInstance(Classes::Find(Name("VectorField3D")), "Field");
	VectorField3D *description = store.GetMutable<VectorField3D>(field);
	REQUIRE(description != nullptr);
	description->Radial = 3.0f;
	description->Tangential = 4.0f;
	description->Axis = Vector3::YAxis;

	const auto sample = ResolveVectorField(store, field);
	const Vector3 force = SampleVectorField(sample, Vector3{1.0f, 0.0f, 0.0f});
	CHECK(force.X == 3.0f);
	CHECK(force.Y == 0.0f);
	CHECK(force.Z == -4.0f);
}

TEST_CASE("a local field rotates its sampled vector with its source", "[scene][vectorfield]") {
	engine::scene::RegisterSceneClasses();
	Store store("vector_field.local");

	const Entity field = store.CreateInstance(Classes::Find(Name("VectorField3D")), "Field");
	VectorField3D *description = store.GetMutable<VectorField3D>(field);
	Transform *transform = store.GetMutable<Transform>(field);
	REQUIRE(description != nullptr);
	REQUIRE(transform != nullptr);
	description->Vector = Vector3{1.0f, 0.0f, 0.0f};
	description->LocalSpace = true;
	transform->Frame = CFrame::Angles(0.0f, 1.57079632679f, 0.0f);

	const Vector3 force = SampleVectorField(ResolveVectorField(store, field), Vector3::Zero);
	CHECK(std::abs(force.X) < 0.00001f);
	CHECK(force.Y == 0.0f);
	CHECK(std::abs(force.Z + 1.0f) < 0.00001f);
}

TEST_CASE("a vector field survives component serialisation", "[scene][vectorfield]") {
	engine::scene::RegisterSceneComponents();

	VectorField3D authored;
	authored.Vector = Vector3{3.0f, -2.0f, 1.0f};
	authored.HalfExtent = Vector3{12.0f, 16.0f, 20.0f};
	authored.Axis = Vector3{0.0f, 0.0f, 1.0f};
	authored.Radial = -4.0f;
	authored.Tangential = 5.0f;
	authored.Falloff = 2.0f;
	authored.LocalSpace = false;

	const auto component = engine::ecs::Components::Find(Name("scene.VectorField3D"));
	const engine::ecs::TypeDescriptor &type = engine::ecs::Components::Describe(component);
	engine::core::ByteWriter writer;
	type.Write(writer, &authored, 1);

	VectorField3D restored;
	engine::core::ByteReader reader(writer.Bytes());
	type.Read(reader, &restored, 1);

	CHECK(restored.Vector == authored.Vector);
	CHECK(restored.HalfExtent == authored.HalfExtent);
	CHECK(restored.Axis == authored.Axis);
	CHECK(restored.Radial == authored.Radial);
	CHECK(restored.Tangential == authored.Tangential);
	CHECK(restored.Falloff == authored.Falloff);
	CHECK(restored.LocalSpace == authored.LocalSpace);
}
