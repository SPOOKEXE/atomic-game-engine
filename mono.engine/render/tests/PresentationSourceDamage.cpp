#include "PresentationSourceDamageTable.hpp"

#include <engine/ecs/Store.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scene/Visibility.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <tuple>
#include <type_traits>

TEST_SUITE_ID("engine.render.presentationsourcedamage")
TEST_DEPENDS("engine.scene.services")

namespace {
	using namespace engine;
	using namespace render::presentation_source_damage_detail;

	template <class Descriptor> void CheckSource(Descriptor source, size_t index) {
		using Component = typename Descriptor::ComponentType;
		DYNAMIC_SECTION("source descriptor " << index) {
			ecs::Store store("presentation-source-damage");
			const ecs::Entity workspace = scene::InstallServices(store);
			const ecs::Entity part = scene::MakePart(store, scene::PartDesc{});
			REQUIRE(part != ecs::NULL_ENTITY);
			if constexpr (std::is_same_v<Component, scene::Rendered>) {
				REQUIRE_FALSE(store.Has<Component>(part));
			} else {
				store.Remove<Component>(part);
				REQUIRE_FALSE(store.Has<Component>(part));
			}

			render::DrawList draw;
			const auto collect = [&] {
				render::PresentationSourceDamage damage;
				source.Collect(store, draw, [](ecs::Entity) { return true; }, damage);
				return damage;
			};
			const auto checkEpoch = [&] {
				const auto &seen = draw.Revisions.*Descriptor::RevisionMember;
				CHECK(seen.Writes == store.ComponentChangeVersion<Component>());
				CHECK(seen.Membership == store.ComponentMembershipVersion<Component>());
			};
			const auto first = collect();
			CHECK_FALSE(first.Full);
			CHECK_FALSE(first.Pose);
			checkEpoch();
			draw.SourcesReady = true;

			if constexpr (std::is_same_v<Component, scene::Rendered>) {
				REQUIRE(store.SetParent(part, workspace));
				REQUIRE(scene::SyncRendered(store) == 1);
			} else {
				store.Set(part, Component{});
			}
			REQUIRE(store.Has<Component>(part));
			const auto added = collect();
			CHECK(added.Full);
			CHECK_FALSE(added.Pose);
			checkEpoch();
			store.ClearChanges();
			const auto steadyAfterAdd = collect();
			CHECK_FALSE(steadyAfterAdd.Full);
			CHECK_FALSE(steadyAfterAdd.Pose);

			if constexpr (std::is_same_v<Component, scene::Rendered>) {
				// Rendered is derived by SyncRendered. Its mark is never authored or
				// written as a reported value change between presentation passes.
				REQUIRE(scene::SyncRendered(store) == 1);
				const auto steadyDerived = collect();
				CHECK_FALSE(steadyDerived.Full);
				CHECK_FALSE(steadyDerived.Pose);
				store.DestroyInstance(part);
			} else {
				store.Set(part, Component{});
				const auto changed = collect();
				CHECK(changed.Full == (Descriptor::Damage == DamageClass::Full));
				CHECK(changed.Pose == (Descriptor::Damage == DamageClass::Pose));
				checkEpoch();
				store.ClearChanges();
				const auto steadyAfterChange = collect();
				CHECK_FALSE(steadyAfterChange.Full);
				CHECK_FALSE(steadyAfterChange.Pose);
				store.Remove<Component>(part);
			}
			CHECK_FALSE(store.Has<Component>(part));
			const auto removed = collect();
			CHECK(removed.Full);
			CHECK_FALSE(removed.Pose);
			checkEpoch();
			store.ClearChanges();
			const auto steadyAfterRemove = collect();
			CHECK_FALSE(steadyAfterRemove.Full);
			CHECK_FALSE(steadyAfterRemove.Pose);
		}
	}
}

TEST_CASE(
	"every presentation source descriptor tracks membership writes and a steady second frame",
	"[render][presentation][damage]"
) {
	engine::scene::RegisterSceneClasses();
	size_t index = 0;
	std::apply(
		[&](const auto &...source) { (CheckSource(source, index++), ...); },
		engine::render::presentation_source_damage_detail::SOURCE_TABLE
	);
	CHECK(index == 15);
	constexpr auto revisions = std::apply(
		[](const auto &...source) {
			return std::array{std::remove_cvref_t<decltype(source)>::RevisionMember...};
		},
		engine::render::presentation_source_damage_detail::SOURCE_TABLE
	);
	for (size_t left = 0; left < revisions.size(); ++left)
		for (size_t right = left + 1; right < revisions.size(); ++right)
			CHECK(revisions[left] != revisions[right]);
}
