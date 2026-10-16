#include <engine/core/Paths.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/EditableMesh.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/script/BakedProceduralReference.hpp>
#include <engine/script/ProceduralService.hpp>
#include <engine/scripthost/Runtime.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

TEST_SUITE_ID("engine.scripthost.proceduralservice")
TEST_DEPENDS("engine.script.proceduralservice")
TEST_DEPENDS("engine.script.bakedproceduralreference")
using namespace engine;
namespace {
	std::string ForestScript(script::Language language) {
		const auto filename = language == script::Language::Luau ? "ForestVillage.luau" : "ForestVillage.js";
		auto path = core::Paths::Assets() / "examples/scripts" / filename;
		if (!std::filesystem::is_regular_file(path))
			path = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() /
				   "examples/assets/scripts" / filename;
		std::ifstream file(path, std::ios::binary);
		REQUIRE(file.good());
		return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
	}
	struct ForestResult {
		std::vector<uint8_t> Authority{};
		std::vector<std::vector<uint8_t>> Artifacts{};
		std::vector<std::vector<core::Vector3>> Positions{};
		std::vector<std::vector<core::Color3>> Colours{};
		std::vector<core::Vector3> Centres{};
		std::vector<core::Vector3> HalfExtents{};
	};
	ForestResult RunForest(script::Language language) {
		scene::RegisterSceneClasses();
		ecs::Store store("forest-village-parity");
		const auto runtime = script::MakeRuntime(store, language);
		REQUIRE(runtime);
		REQUIRE(runtime->ProceduralEndpointForWorld());
		const bool ran = runtime->Run(ForestScript(language), "ForestVillage");
		INFO(runtime->LastError());
		REQUIRE(ran);
		const auto *published = store.Resource<script::ProceduralPublished>();
		REQUIRE(published);
		REQUIRE(published->Regions.size() == 2);
		ForestResult out;
		procedural::Diagnostic diagnostic;
		REQUIRE(script::EncodeProceduralAuthority(store, out.Authority, diagnostic));
		for (size_t index = 0; index < 2; ++index) {
			CHECK(published->Regions[index].ActivationTick == store.Time().Tick);
			out.Artifacts.push_back(published->Regions[index].ArtifactBytes);
			const auto name = "GeneratedRegion" + std::to_string(index + 1);
			const auto meshEntity = store.FindFirstChild(scene::WorkspaceOf(store), name);
			REQUIRE(meshEntity != ecs::NULL_ENTITY);
			const auto *mesh = store.Get<scene::EditableMesh>(meshEntity);
			REQUIRE(mesh);
			REQUIRE(!mesh->Positions.empty());
			out.Positions.push_back(mesh->Positions);
			out.Colours.push_back(mesh->Colours);
			const auto *region = store.Get<script::ProceduralSceneRegion>(meshEntity);
			REQUIRE(region);
			CHECK(region->ArtifactDigest == published->Regions[index].ArtifactDigest);
			ecs::Entity partEntity = ecs::NULL_ENTITY;
			store.Each<scene::Visual>([&](ecs::Entity entity, const scene::Visual &visual) {
				if (visual.Mesh == scene::EditableMeshContentName(store, meshEntity)) partEntity = entity;
			});
			REQUIRE(partEntity != ecs::NULL_ENTITY);
			out.Centres.push_back(store.Get<scene::Transform>(partEntity)->Frame.Position);
			out.HalfExtents.push_back(store.Get<scene::Bounds>(partEntity)->HalfExtent);
			CHECK(store.Get<scene::Collider>(partEntity)->Mask == spatial::LayerMask::None());
			CHECK_FALSE(store.Get<scene::Collider>(partEntity)->CanQuery);
		}
		const char *useBaked = language == script::Language::Luau
								   ? "local p=game:GetService('ProceduralService');"
									 "assert(p:UseBaked({X=0,Z=0,Cells=16},"
									 "workspace:FindFirstChild('GeneratedRegion1'),"
									 "workspace:FindFirstChild('GeneratedRegionView1'),'town/region.aproc'));"
									 "assert(p:UseBaked({X=0,Z=0,Cells=16},"
									 "workspace:FindFirstChild('GeneratedRegion1'),"
									 "workspace:FindFirstChild('GeneratedRegionView1'),'town/region.aproc'))"
								   : "const p=game.GetService('ProceduralService');"
									 "if(!p.UseBaked({X:0,Z:0,Cells:16},"
									 "workspace.FindFirstChild('GeneratedRegion1'),"
									 "workspace.FindFirstChild('GeneratedRegionView1'),'town/region.aproc'))"
									 "throw new Error('UseBaked refused');"
									 "if(!p.UseBaked({X:0,Z:0,Cells:16},"
									 "workspace.FindFirstChild('GeneratedRegion1'),"
									 "workspace.FindFirstChild('GeneratedRegionView1'),'town/region.aproc'))"
									 "throw new Error('UseBaked retry refused');";
		const bool referenced = runtime->Run(useBaked, "ProceduralUseBaked");
		INFO(runtime->LastError());
		REQUIRE(referenced);
		const auto mesh = store.FindFirstChild(scene::WorkspaceOf(store), "GeneratedRegion1");
		const auto *reference = store.Get<script::BakedProceduralReference>(mesh);
		REQUIRE(reference);
		CHECK(reference->Description.Text() == "town/region.aproc");
		CHECK(reference->Scope.CellsPerSide == 16);
		CHECK(reference->Revision == 2);
		// Malformed authoring must leave the last accepted facts and geometry intact.
		const char *invalid =
			language == script::Language::Luau
				? "local p=game:GetService('ProceduralService');local ok=pcall(function() "
				  "p:ConfigureForestVillage({Name=string.rep('x',70000)}) end);assert(not ok)"
				: "const boundsService=game.GetService('ProceduralService');let "
				  "refused=false;try{boundsService.ConfigureForestVillage({Name:'x'.repeat(70000)});}catch(e)"
				  "{refused="
				  "true;}if(!refused)throw new Error('oversized record accepted');";
		const bool checked = runtime->Run(invalid, "ProceduralBounds");
		INFO(runtime->LastError());
		REQUIRE(checked);
		std::vector<uint8_t> after;
		REQUIRE(script::EncodeProceduralAuthority(store, after, diagnostic));
		CHECK(after == out.Authority);
		CHECK(
			store.Resource<script::ProceduralPublished>()->Regions.front().ArtifactBytes ==
			out.Artifacts.front()
		);
		return out;
	}
}
TEST_CASE(
	"paired forest village scripts publish identical live geometry through both real VMs",
	"[procedural][service]"
) {
	const auto luau = RunForest(script::Language::Luau);
	const auto javascript = RunForest(script::Language::JavaScript);
	CHECK(luau.Authority == javascript.Authority);
	CHECK(luau.Artifacts == javascript.Artifacts);
	CHECK(luau.Positions == javascript.Positions);
	CHECK(luau.Colours == javascript.Colours);
	CHECK(luau.Centres == javascript.Centres);
	CHECK(luau.HalfExtents == javascript.HalfExtents);
}
TEST_CASE("real runtime teardown and store clear revoke procedural endpoints", "[procedural][service]") {
	for (const auto language : {script::Language::Luau, script::Language::JavaScript}) {
		scene::RegisterSceneComponents();
		ecs::Store store("procedural-runtime-lifetime");
		auto runtime = script::MakeRuntime(store, language);
		REQUIRE(runtime);
		const auto endpoint = runtime->ProceduralEndpointForWorld();
		REQUIRE(endpoint);
		store.Clear();
		procedural::Diagnostic diagnostic;
		const std::array<procedural::RegionScope, 1> scopes{{{0, 0, 16}}};
		CHECK_FALSE(endpoint->Configure(
			store, procedural::ForestVillageDefaults("expired", 1), scopes, {}, diagnostic
		));
		runtime.reset();
		auto replacement = script::MakeRuntime(store, language);
		REQUIRE(replacement);
		REQUIRE(replacement->ProceduralEndpointForWorld());
		const auto detached = replacement->ProceduralEndpointForWorld();
		replacement.reset();
		CHECK_FALSE(detached->Configure(
			store, procedural::ForestVillageDefaults("expired", 1), scopes, {}, diagnostic
		));
	}
}
TEST_CASE(
	"replicas package-only and world-free runtimes do not create procedural endpoints",
	"[procedural][service]"
) {
	for (const auto language : {script::Language::Luau, script::Language::JavaScript}) {
		scene::RegisterSceneComponents();
		ecs::Store store("procedural-unavailable");
		script::RuntimeLimits package;
		package.PackageOnly = true;
		auto packaged = script::MakeRuntime(store, language, package);
		CHECK_FALSE(packaged->ProceduralEndpointForWorld());
		script::RuntimeLimits restricted;
		restricted.Capabilities = script::ScriptCapabilities::Input;
		auto limited = script::MakeRuntime(store, language, restricted);
		CHECK_FALSE(limited->ProceduralEndpointForWorld());
		store.SetAdoptOnly(true);
		auto replica = script::MakeRuntime(store, language);
		CHECK_FALSE(replica->ProceduralEndpointForWorld());
		CHECK_FALSE(store.Resource<script::ProceduralAuthority>());
	}
}

TEST_CASE(
	"Studio command and plugin runtimes coexist with Run on the same serial world", "[procedural][service]"
) {
	for (const auto language : {script::Language::Luau, script::Language::JavaScript}) {
		scene::RegisterSceneClasses();
		ecs::Store store("procedural-studio-roles");
		script::RuntimeLimits editing;
		editing.Role = {false, false, true};
		editing.Origin = script::ScriptOrigin::Plugin;
		auto command = script::MakeRuntime(store, language, editing);
		auto plugin = script::MakeRuntime(store, language, editing);
		REQUIRE(command->ProceduralEndpointForWorld());
		REQUIRE(plugin->ProceduralEndpointForWorld());
		script::RuntimeLimits running;
		running.Role = {true, false, true};
		auto game = script::MakeRuntime(store, language, running);
		REQUIRE(game->ProceduralEndpointForWorld());
		CHECK(command->ProceduralEndpointForWorld()->BelongsTo(store, command->Identity()));
		CHECK(plugin->ProceduralEndpointForWorld()->BelongsTo(store, plugin->Identity()));
		CHECK(game->ProceduralEndpointForWorld()->BelongsTo(store, game->Identity()));
	}
}
