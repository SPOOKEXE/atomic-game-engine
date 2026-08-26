// Bulk mesh work stays pure off-thread and publishes in owner order.

#include <engine/ecs/Store.hpp>
#include <engine/scene/EditableMesh.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/script/EditableMeshJobs.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <vector>

TEST_SUITE_ID("engine.script.editablemeshjobs")

namespace {
	engine::scene::EditableMeshGeometry Triangle(float offset) {
		engine::scene::EditableMeshGeometry geometry;
		geometry.Positions = {
			engine::core::Vector3{offset, 0.0f, 0.0f},
			engine::core::Vector3{offset + 1.0f, 0.0f, 0.0f},
			engine::core::Vector3{offset, 0.0f, 1.0f},
		};
		geometry.Normals.assign(3, engine::core::Vector3{0.0f, 1.0f, 0.0f});
		geometry.UVs.assign(3, engine::core::Vector2{});
		geometry.Colours.assign(3, engine::core::Color3{1.0f, 1.0f, 1.0f});
		geometry.Alphas.assign(3, 0.0f);
		geometry.Indices = {0, 2, 1};
		return geometry;
	}
}

TEST_CASE("editable mesh requests prepare together and commit in ticket order", "[script][editablemesh]") {
	engine::scene::RegisterSceneComponents();
	engine::ecs::Store store("script.editablemeshjobs.batch");
	engine::script::EditableMeshJobs jobs;
	const std::array meshes{
		store.CreateInstance(engine::scene::EditableMeshClass(), "First"),
		store.CreateInstance(engine::scene::EditableMeshClass(), "Second"),
		store.CreateInstance(engine::scene::EditableMeshClass(), "Third"),
	};

	std::array<uint64_t, 3> tickets{};
	for (size_t index = 0; index < meshes.size(); index++) {
		tickets[index] = jobs.Submit(store, meshes[index], Triangle(static_cast<float>(index)));
	}
	REQUIRE(jobs.PendingCount() == meshes.size());

	jobs.Run(store);
	REQUIRE(jobs.PendingCount() == 0);
	REQUIRE(jobs.Completions().size() == meshes.size());
	for (size_t index = 0; index < meshes.size(); index++) {
		CHECK(jobs.Completions()[index].Ticket == tickets[index]);
		CHECK(jobs.Completions()[index].Result == engine::scene::EditableMeshCommit::Applied);
		const auto *mesh = store.Get<engine::scene::EditableMesh>(meshes[index]);
		REQUIRE(mesh != nullptr);
		CHECK(mesh->Revision == 1);
		CHECK(mesh->Positions.front().X == static_cast<float>(index));
	}
}

TEST_CASE("editable mesh jobs reject stale and destroyed targets", "[script][editablemesh]") {
	engine::scene::RegisterSceneComponents();
	engine::ecs::Store store("script.editablemeshjobs.cancel");
	engine::script::EditableMeshJobs jobs;
	const engine::ecs::Entity stale = store.CreateInstance(engine::scene::EditableMeshClass(), "Stale");
	const engine::ecs::Entity destroyed =
		store.CreateInstance(engine::scene::EditableMeshClass(), "Destroyed");

	const uint64_t staleTicket = jobs.Submit(store, stale, Triangle(0.0f));
	const uint64_t destroyedTicket = jobs.Submit(store, destroyed, Triangle(2.0f));
	REQUIRE(engine::scene::AddVertex(store, stale, engine::core::Vector3{}).has_value());
	store.Destroy(destroyed);
	REQUIRE_FALSE(store.Alive(destroyed));

	jobs.Run(store);
	REQUIRE(jobs.Completions().size() == 2);
	CHECK(jobs.Completions()[0].Ticket == staleTicket);
	CHECK(jobs.Completions()[0].Result == engine::scene::EditableMeshCommit::Stale);
	CHECK(jobs.Completions()[1].Ticket == destroyedTicket);
	CHECK(jobs.Completions()[1].Result == engine::scene::EditableMeshCommit::Missing);
	CHECK(store.Get<engine::scene::EditableMesh>(stale)->Positions.size() == 1);

	jobs.ClearCompletions();
	CHECK(jobs.Completions().empty());
}
