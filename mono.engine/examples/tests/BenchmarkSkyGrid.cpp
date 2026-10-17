// The SkyGrid PBR benchmark scene, loaded through the public script path.
//
// This checks the bounded inputs a headless suite can see: eight editable
// terrain meshes, their six shared PBR maps, eight selected shader sources and
// the deterministic camera flight. GPU compilation and frame timing belong to
// the client benchmark run, not to a mock renderer here.

#include <engine/core/Name.hpp>
#include <engine/core/Paths.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/ecs/Scheduler.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/examples/Scene.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/EditableImage.hpp>
#include <engine/scene/EditableMesh.hpp>
#include <engine/scene/Materials.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scene/Shaders.hpp>
#include <engine/script/Runtime.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <string_view>

TEST_SUITE_ID("engine.examples.benchmark-skygrid")
TEST_DEPENDS("engine.scripthost.scripting")

using Catch::Approx;

namespace {
	using engine::ecs::Entity;
	using engine::ecs::Scheduler;
	using engine::ecs::Store;

	struct StagedAssets {
		std::filesystem::path Previous = engine::core::Paths::Assets();

		StagedAssets() {
			engine::core::Paths::SetAssetsOverride(engine::core::Paths::Base().parent_path() / "assets");
		}

		~StagedAssets() {
			engine::core::Paths::SetAssetsOverride(Previous);
		}
	};

	Entity InScene(Store &store, std::string_view name) {
		const Entity workspace = engine::scene::WorkspaceOf(store);
		if (workspace != engine::ecs::NULL_ENTITY) {
			const Entity child = store.FindFirstChild(workspace, name);
			if (child != engine::ecs::NULL_ENTITY) {
				return child;
			}
		}
		return store.FindFirstRoot(name);
	}
}

TEST_CASE("SkyGrid PBR is a deterministic eight-variant terrain course", "[examples][scene][benchmark]") {
	const StagedAssets assets;
	Store store("benchmark.skygrid");
	Scheduler systems;
	std::shared_ptr<engine::script::Runtime> runtime;
	std::string error;
	const bool loaded = engine::examples::LoadScene(
		store, systems, engine::examples::ExamplePath("BenchmarkSkyGrid.luau"), error, &runtime
	);
	INFO(error);
	REQUIRE(loaded);
	REQUIRE(runtime != nullptr);

	// Each `SetGeometry` resumes at the next script barrier. Eight meshes and a
	// few camera frames need more than one tick, but remain a tight test budget.
	for (size_t tick = 0; tick < 16; tick++) {
		systems.Tick(store, 1.0f / 60.0f);
	}
	CHECK(runtime->LastError().empty());

	const std::array mapNames{
		"SkyGridPbr_Colour",
		"SkyGridPbr_Normal",
		"SkyGridPbr_Roughness",
		"SkyGridPbr_Occlusion",
		"SkyGridPbr_Metalness",
		"SkyGridPbr_Emissive",
	};
	for (const char *mapName : mapNames) {
		const Entity image = InScene(store, mapName);
		REQUIRE(image != engine::ecs::NULL_ENTITY);
		const auto *editable = store.Get<engine::scene::EditableImage>(image);
		REQUIRE(editable != nullptr);
		CHECK(editable->Width == 256);
		CHECK(editable->Height == 256);
		CHECK(editable->Revision > 0);
	}

	// Shader-only materials intentionally retain the direct live PBR maps. The
	// resolve pass both verifies that rule and makes the selected shader visible
	// on the same draw input the renderer consumes.
	CHECK(engine::scene::ResolveMaterials(store) == 8);

	const std::array shaderNames{
		"SkyGridBasalt",
		"SkyGridAmber",
		"SkyGridVerdant",
		"SkyGridCobalt",
		"SkyGridViolet",
		"SkyGridCopper",
		"SkyGridGlacier",
		"SkyGridEmber",
	};
	std::set<std::string> shaderSources;
	for (size_t index = 0; index < shaderNames.size(); index++) {
		const std::string suffix = std::to_string(index + 1);
		const Entity shader = InScene(store, shaderNames[index]);
		REQUIRE(shader != engine::ecs::NULL_ENTITY);
		const engine::scene::ShaderText source =
			engine::scene::ShaderTextOf(store, engine::core::Name(shaderNames[index]));
		CHECK(source.Found);
		CHECK(source.Revision > 0);
		CHECK(source.Code.find("metalnessMap") != std::string::npos);
		CHECK(source.Code.find("inWorldPosition") != std::string::npos);
		CHECK(source.Code.find("lights.Position") != std::string::npos);
		CHECK(source.Code.find("CotangentFrame") != std::string::npos);
		CHECK(source.Code.find("lighting.Eye") != std::string::npos);
		CHECK(source.Code.find("DistributionGGX") != std::string::npos);
		CHECK(shaderSources.insert(source.Code).second);

		const Entity meshEntity = InScene(store, "SkyGridTerrainMesh_" + suffix);
		REQUIRE(meshEntity != engine::ecs::NULL_ENTITY);
		const auto *mesh = store.Get<engine::scene::EditableMesh>(meshEntity);
		REQUIRE(mesh != nullptr);
		CHECK(mesh->Positions.size() == 45 * 65);
		CHECK(mesh->Indices.size() == 44 * 64 * 6);

		const Entity part = InScene(store, "SkyGridBall_" + suffix);
		REQUIRE(part != engine::ecs::NULL_ENTITY);
		const auto *appearance = store.Get<engine::scene::SurfaceAppearance>(part);
		REQUIRE(appearance != nullptr);
		CHECK(appearance->ColourMap.IsValid());
		CHECK(appearance->NormalMap.IsValid());
		CHECK(appearance->RoughnessMap.IsValid());
		CHECK(appearance->OcclusionMap.IsValid());
		CHECK(appearance->MetalnessMap.IsValid());
		CHECK(appearance->EmissiveMap.IsValid());
		CHECK(appearance->Shader == engine::core::Name(shaderNames[index]));

		const Entity material = store.FindFirstChild(part, "SkyGridMaterial_" + suffix);
		REQUIRE(material != engine::ecs::NULL_ENTITY);
		const auto *selection = store.Get<engine::scene::MaterialRef>(material);
		REQUIRE(selection != nullptr);
		CHECK(selection->Shader == engine::core::Name(shaderNames[index]));
	}

	const Entity camera = InScene(store, "SkyGridBenchmarkCamera");
	REQUIRE(camera != engine::ecs::NULL_ENTITY);
	REQUIRE(store.Resource<engine::scene::ActiveCamera>() != nullptr);
	CHECK(store.Resource<engine::scene::ActiveCamera>()->Entity == camera);
	const auto *transform = store.Get<engine::scene::Transform>(camera);
	REQUIRE(transform != nullptr);
	CHECK(transform->Frame.Position.Z != 0.0f);
	const engine::core::Vector3 firstFlightPosition = transform->Frame.Position;

	// The authored ten-second period must return to exactly the same seam point,
	// otherwise captures at a nominally identical time exercise different culling.
	for (size_t tick = 0; tick < 600; tick++) {
		systems.Tick(store, 1.0f / 60.0f);
	}
	const auto *repeated = store.Get<engine::scene::Transform>(camera);
	REQUIRE(repeated != nullptr);
	CHECK(repeated->Frame.Position.X == Approx(firstFlightPosition.X));
	CHECK(repeated->Frame.Position.Y == Approx(firstFlightPosition.Y));
	CHECK(repeated->Frame.Position.Z == Approx(firstFlightPosition.Z));
}
