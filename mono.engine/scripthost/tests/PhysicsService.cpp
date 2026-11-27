// `PhysicsService` hands collision groups to scripts as records, read only.
//
// The same script runs in both VMs, because the service is one surface both
// adapters install and a difference between them is the bug worth catching.

#include <engine/ecs/Store.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/script/Runtime.hpp>
#include <engine/scripthost/Runtime.hpp>
#include <engine/spatial/CollisionGroups.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.scripthost.physicsservice")

using engine::ecs::Store;
using engine::script::Language;
using engine::script::MakeRuntime;
using engine::spatial::CollisionGroups;

namespace {
	// Three groups where Players and Projectiles do not meet.
	void RegisterGroups() {
		CollisionGroups::Reset();
		CollisionGroups::Register("Players");
		CollisionGroups::Register("Projectiles");
		CollisionGroups::SetCollidable(engine::core::Name("Players"), engine::core::Name("Projectiles"), false);
	}
}

TEST_CASE("collision groups read back as records in both VMs", "[scripting][physics]") {
	for (const auto language : {Language::Luau, Language::JavaScript}) {
		RegisterGroups();
		engine::scene::RegisterSceneClasses();
		Store store("physicsservice.records");
		const auto runtime = MakeRuntime(store, language);
		REQUIRE(runtime != nullptr);

		const char *source = language == Language::Luau ? R"(
local PhysicsService = game:GetService('PhysicsService')
assert(PhysicsService.CollisionGroupCount == 3, 'wrong count')

local players = PhysicsService:GetCollisionGroup('Players')
assert(players.Name == 'Players', 'wrong name')
assert(#players.CollidesWith == 2, 'Players should meet Default and itself')
for _, other in players.CollidesWith do
	assert(other ~= 'Projectiles', 'Players should not meet Projectiles')
end

assert(PhysicsService:GetCollisionGroup('Missing') == nil, 'an unknown group was found')

local all = PhysicsService:GetCollisionGroups()
assert(#all == 3 and all[1].Name == 'Default', 'Default is not first')
assert(#all[1].CollidesWith == 3, 'Default should meet every group')

assert(not pcall(function() PhysicsService.CollisionGroupCount = 9 end), 'the count was writable')
)"
														: R"(
const PhysicsService = game.GetService('PhysicsService');
if (PhysicsService.CollisionGroupCount !== 3) throw new Error('wrong count');

const players = PhysicsService.GetCollisionGroup('Players');
if (players.Name !== 'Players') throw new Error('wrong name');
if (players.CollidesWith.length !== 2) throw new Error('Players should meet Default and itself');
if (players.CollidesWith.includes('Projectiles')) throw new Error('Players should not meet Projectiles');

if (PhysicsService.GetCollisionGroup('Missing') != null) throw new Error('an unknown group was found');

const all = PhysicsService.GetCollisionGroups();
if (all.length !== 3 || all[0].Name !== 'Default') throw new Error('Default is not first');
if (all[0].CollidesWith.length !== 3) throw new Error('Default should meet every group');

let refused = false;
try { PhysicsService.CollisionGroupCount = 9; } catch (e) { refused = true; }
if (!refused) throw new Error('the count was writable');
)";
		INFO(source);
		const bool ok = runtime->Run(source);
		INFO(runtime->LastError());
		CHECK(ok);
	}
	CollisionGroups::Reset();
}
