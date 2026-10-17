#include <engine/core/Bytes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/AuthoredAffordance.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.scene.authoredaffordance")

TEST_CASE(
	"part affordances are explicitly authored and survive cloning and snapshots",
	"[scene][affordance][optional]"
) {
	using namespace engine;
	scene::RegisterSceneComponents();
	ecs::Store store("authored-affordance");
	const auto plain = store.CreateInstance(scene::PartClass(), "Plain");
	REQUIRE(plain != ecs::NULL_ENTITY);
	CHECK_FALSE(store.Get<scene::AuthoredAffordance>(plain));
	core::Name id, kind;
	bool enabled = true;
	REQUIRE(store.GetProperty(plain, core::Name("AffordanceId"), &id, sizeof(id)));
	REQUIRE(store.GetProperty(plain, core::Name("AffordanceKind"), &kind, sizeof(kind)));
	REQUIRE(store.GetProperty(plain, core::Name("AffordanceEnabled"), &enabled, sizeof(enabled)));
	CHECK_FALSE(id.IsValid());
	CHECK(kind == core::Name("None"));
	CHECK_FALSE(enabled);
	REQUIRE(store.SetProperty(plain, core::Name("AffordanceId"), &id, sizeof(id)));
	REQUIRE(store.SetProperty(plain, core::Name("AffordanceKind"), &kind, sizeof(kind)));
	REQUIRE(store.SetProperty(plain, core::Name("AffordanceEnabled"), &enabled, sizeof(enabled)));
	CHECK_FALSE(store.Get<scene::AuthoredAffordance>(plain));

	const auto authored = store.CreateInstance(scene::PartClass(), "Authored");
	id = core::Name("door/interact");
	kind = core::Name("Interactable");
	enabled = true;
	REQUIRE(store.SetProperty(authored, core::Name("AffordanceKind"), &kind, sizeof(kind)));
	REQUIRE(store.SetProperty(authored, core::Name("AffordanceId"), &id, sizeof(id)));
	REQUIRE(store.SetProperty(authored, core::Name("AffordanceEnabled"), &enabled, sizeof(enabled)));
	const auto clone = store.CloneInstance(authored);
	REQUIRE(clone != ecs::NULL_ENTITY);
	const auto plainClone = store.CloneInstance(plain);
	REQUIRE(plainClone != ecs::NULL_ENTITY);
	CHECK_FALSE(store.Get<scene::AuthoredAffordance>(plainClone));
	core::ByteWriter writer;
	REQUIRE(store.Save(writer));
	ecs::Store restored("affordance-restored");
	core::ByteReader reader(writer.Bytes());
	REQUIRE(restored.Load(reader));
	for (const auto entity : {authored, clone}) {
		const auto *value = restored.Get<scene::AuthoredAffordance>(entity);
		REQUIRE(value);
		CHECK(value->Id == id);
		CHECK(value->Kind == scene::AuthoredAffordanceKind::Interactable);
		CHECK(value->Enabled);
	}
	CHECK_FALSE(restored.Get<scene::AuthoredAffordance>(plain));
	CHECK_FALSE(restored.Get<scene::AuthoredAffordance>(plainClone));

	// Disabling a tag keeps its authored identity. Clearing every field removes it.
	enabled = false;
	REQUIRE(restored.SetProperty(clone, core::Name("AffordanceEnabled"), &enabled, sizeof(enabled)));
	REQUIRE(restored.Get<scene::AuthoredAffordance>(clone));
	CHECK(restored.Get<scene::AuthoredAffordance>(clone)->Id == id);
	id = {};
	kind = core::Name("None");
	REQUIRE(restored.SetProperty(clone, core::Name("AffordanceId"), &id, sizeof(id)));
	REQUIRE(restored.SetProperty(clone, core::Name("AffordanceKind"), &kind, sizeof(kind)));
	CHECK_FALSE(restored.Get<scene::AuthoredAffordance>(clone));
}
