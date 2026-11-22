#include <engine/ecs/Attributes.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scene/environment/PhysicalParticleField.hpp>
#include <engine/scripthost/Runtime.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.scripthost.particlecollision")

using namespace engine;

namespace {
	scene::ParticleCollision Hit(ecs::Entity field, ecs::Entity part) {
		scene::ParticleCollision hit;
		hit.Field = field;
		hit.Hit = part;
		hit.Position = {1, 2, 3};
		hit.Velocity = {0, -10, 0};
		hit.Normal = {0, 1, 0};
		hit.Radius = 0.25f;
		hit.ParticleId = 42;
		hit.Response = scene::ParticleResponse::Stick;
		return hit;
	}
}

TEST_CASE("OnParticleCollision hands the hit to handlers in both VMs", "[script][particles]") {
	scene::RegisterSceneClasses();
	for (const auto language : {script::Language::Luau, script::Language::JavaScript}) {
		ecs::Store store("script.particles");
		scene::InstallServices(store);
		const auto workspace = scene::ServiceOf(store, ecs::Classes::Find(core::Name("Workspace")));
		const auto field =
			store.CreateInstance(ecs::Classes::Find(core::Name("PhysicalParticleField")), "Hail");
		const auto roof = store.CreateInstance(ecs::Classes::Find(core::Name("Part")), "Roof");
		REQUIRE(store.SetParent(field, workspace));
		REQUIRE(store.SetParent(roof, workspace));

		// Logged before the runtime exists, so never delivered to it.
		scene::LogParticleCollision(store, Hit(field, roof));

		const auto runtime = script::MakeRuntime(store, language);
		const bool ran = runtime->Run(
			language == script::Language::Luau ? R"(
local field = workspace:FindFirstChild('Hail')
local hits = 0
field.OnParticleCollision:Connect(function(hit)
	assert(hit.Instance.Name == 'Roof')
	assert(hit.Field == field)
	assert(hit.Position == Vector3.new(1, 2, 3))
	assert(hit.Direction == Vector3.new(0, -1, 0))
	assert(hit.Normal == Vector3.new(0, 1, 0))
	assert(hit.Speed == 10 and hit.ParticleId == 42)
	assert(hit.ParticleComponents.Response == 'Stick')
	assert(hit.ParticleComponents.Radius == 0.25)
	hits += 1
	workspace:SetAttribute('Hits', hits)
end)
)"
											   : R"(
const field = workspace.FindFirstChild('Hail');
let hits = 0;
field.OnParticleCollision.Connect((hit) => {
	if (hit.Instance.Name !== 'Roof') throw new Error('instance');
	if (hit.Field.Name !== 'Hail') throw new Error('field');
	if (!hit.Position.Equals(Vector3.new(1, 2, 3))) throw new Error('position');
	if (!hit.Direction.Equals(Vector3.new(0, -1, 0))) throw new Error('direction');
	if (hit.Speed !== 10 || hit.ParticleId !== 42) throw new Error('numbers');
	if (hit.ParticleComponents.Response !== 'Stick') throw new Error('response');
	hits += 1;
	workspace.SetAttribute('Hits', hits);
});
)",
			"particles"
		);
		INFO(runtime->LastError());
		REQUIRE(ran);
		REQUIRE(runtime->Heartbeat(1.0f / 60.0f));
		ecs::AttributeValue hits;
		CHECK_FALSE(ecs::GetAttribute(store, workspace, core::Name("Hits"), hits));

		scene::LogParticleCollision(store, Hit(field, roof));
		scene::LogParticleCollision(store, Hit(field, roof));
		REQUIRE(runtime->Heartbeat(1.0f / 60.0f));
		INFO(runtime->LastError());
		REQUIRE(ecs::GetAttribute(store, workspace, core::Name("Hits"), hits));
		CHECK(hits.Double == 2);
	}
}
