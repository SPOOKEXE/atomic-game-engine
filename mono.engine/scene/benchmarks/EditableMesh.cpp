// The steady collision refresh paid by every presented Studio world.

#include <engine/collision/TriangleMesh.hpp>
#include <engine/core/Name.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/CollisionShapes.hpp>
#include <engine/scene/EditableMesh.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/testing/Bench.hpp>

#include <array>
#include <cstdint>
#include <string>

TEST_SUITE_ID("engine.scene.bench.editablemesh")

namespace {
	engine::ecs::Store &StaticMeshWorld() {
		static engine::ecs::Store store("bench.editablemesh.static");
		static const bool ready = [] {
			engine::scene::RegisterSceneComponents();
			engine::scene::CollisionShapes shapes;
			for (uint32_t index = 0; index < 2'000; index++) {
				engine::collision::TriangleMesh mesh;
				mesh.Vertices.resize(128);
				mesh.Indices.resize(384);
				shapes.SetMesh(engine::core::Name("static-shape-" + std::to_string(index)), std::move(mesh));
			}
			store.SetResource(std::move(shapes));
			return true;
		}();
		(void)ready;
		return store;
	}

	engine::ecs::Store &UnfinishedEditableWorld() {
		static engine::ecs::Store store("bench.editablemesh.unfinished");
		static const bool ready = [] {
			engine::scene::RegisterSceneComponents();
			const engine::ecs::Entity entity = store.Create();
			engine::scene::EditableMesh mesh;
			mesh.Positions.push_back(engine::core::Vector3{});
			store.Set(entity, std::move(mesh));

			engine::scene::CollisionShapes shapes;
			for (uint32_t index = 0; index < 2'000; index++) {
				engine::collision::TriangleMesh collision;
				collision.Vertices.resize(128);
				collision.Indices.resize(384);
				shapes.SetMesh(
					engine::core::Name("unfinished-static-shape-" + std::to_string(index)),
					std::move(collision)
				);
			}
			store.SetResource(std::move(shapes));
			return true;
		}();
		(void)ready;
		return store;
	}

	const engine::scene::EditableMeshGeometry &TerrainChunkGeometry() {
		static const engine::scene::EditableMeshGeometry geometry = [] {
			constexpr uint32_t side = 65;
			engine::scene::EditableMeshGeometry result;
			result.Positions.reserve(side * side);
			result.Normals.reserve(side * side);
			result.UVs.reserve(side * side);
			result.Colours.reserve(side * side);
			result.Alphas.reserve(side * side);
			result.Indices.reserve((side - 1) * (side - 1) * 6);

			for (uint32_t z = 0; z < side; z++) {
				for (uint32_t x = 0; x < side; x++) {
					result.Positions.push_back(
						engine::core::Vector3{static_cast<float>(x), 0.0f, static_cast<float>(z)}
					);
					result.Normals.push_back(engine::core::Vector3{0.0f, 1.0f, 0.0f});
					result.UVs.push_back(
						engine::core::Vector2{
							static_cast<float>(x) / static_cast<float>(side - 1),
							static_cast<float>(z) / static_cast<float>(side - 1),
						}
					);
					result.Colours.push_back(engine::core::Color3{0.4f, 0.6f, 0.3f});
					result.Alphas.push_back(0.0f);
				}
			}
			for (uint32_t z = 0; z + 1 < side; z++) {
				for (uint32_t x = 0; x + 1 < side; x++) {
					const uint32_t at = z * side + x;
					result.Indices.insert(
						result.Indices.end(), {at, at + side, at + 1, at + 1, at + side, at + side + 1}
					);
				}
			}
			return result;
		}();
		return geometry;
	}

	struct DirtyTerrainWorld {
		engine::ecs::Store Scene{"bench.editablemesh.dirty-terrain"};
		std::array<engine::ecs::Entity, 8> TerrainMeshes;
	};

	struct RetainedTerrainWorld {
		engine::ecs::Store Scene{"bench.editablemesh.retained-terrain"};
		std::array<engine::ecs::Entity, 64> TerrainMeshes;
	};

	DirtyTerrainWorld &TerrainWorld() {
		static DirtyTerrainWorld world;
		static const bool ready = [&] {
			engine::scene::RegisterSceneComponents();
			const engine::scene::EditableMeshGeometry &geometry = TerrainChunkGeometry();
			for (engine::ecs::Entity &entity : world.TerrainMeshes) {
				entity = world.Scene.Create();
				engine::scene::EditableMesh mesh;
				mesh.Positions = geometry.Positions;
				mesh.Normals = geometry.Normals;
				mesh.UVs = geometry.UVs;
				mesh.Colours = geometry.Colours;
				mesh.Alphas = geometry.Alphas;
				mesh.Indices = geometry.Indices;
				world.Scene.Set(entity, std::move(mesh));
			}

			engine::scene::CollisionShapes shapes;
			for (uint32_t index = 0; index < 2'000; index++) {
				engine::collision::TriangleMesh collision;
				collision.Vertices.resize(128);
				collision.Indices.resize(384);
				shapes.SetMesh(
					engine::core::Name("dirty-terrain-static-shape-" + std::to_string(index)),
					std::move(collision)
				);
			}
			world.Scene.SetResource(std::move(shapes));
			engine::testing::Consume(engine::scene::RefreshEditableMeshCollision(world.Scene));
			return true;
		}();
		(void)ready;
		return world;
	}

	RetainedTerrainWorld &RetainedTerrainWorldOf() {
		static RetainedTerrainWorld world;
		static const bool ready = [] {
			engine::scene::RegisterSceneComponents();
			const engine::scene::EditableMeshGeometry &geometry = TerrainChunkGeometry();
			for (engine::ecs::Entity &entity : world.TerrainMeshes) {
				entity = world.Scene.Create();
				engine::scene::EditableMesh mesh;
				mesh.Positions = geometry.Positions;
				mesh.Normals = geometry.Normals;
				mesh.UVs = geometry.UVs;
				mesh.Colours = geometry.Colours;
				mesh.Alphas = geometry.Alphas;
				mesh.Indices = geometry.Indices;
				world.Scene.Set(entity, std::move(mesh));
			}

			engine::scene::CollisionShapes shapes;
			for (uint32_t index = 0; index < 2'000; index++) {
				engine::collision::TriangleMesh collision;
				collision.Vertices.resize(128);
				collision.Indices.resize(384);
				shapes.SetMesh(
					engine::core::Name("retained-terrain-static-shape-" + std::to_string(index)),
					std::move(collision)
				);
			}
			world.Scene.SetResource(std::move(shapes));
			engine::testing::Consume(engine::scene::RefreshEditableMeshCollision(world.Scene));
			return true;
		}();
		(void)ready;
		return world;
	}
}

BENCH_PER_ITEM("editable collision steady world without editable meshes", 100'000) {
	engine::ecs::Store &store = StaticMeshWorld();
	for (uint32_t frame = 0; frame < 100'000; frame++) {
		engine::testing::Consume(engine::scene::RefreshEditableMeshCollision(store));
	}
}

BENCH_PER_ITEM("editable collision unfinished mesh without a resident shape", 100'000) {
	engine::ecs::Store &store = UnfinishedEditableWorld();
	for (uint32_t frame = 0; frame < 100'000; frame++) {
		engine::testing::Consume(engine::scene::RefreshEditableMeshCollision(store));
	}
}

BENCH_PER_ITEM("editable mesh terrain-sized signature", 1'000) {
	const engine::scene::EditableMeshGeometry &geometry = TerrainChunkGeometry();
	for (uint32_t iteration = 0; iteration < 1'000; iteration++) {
		engine::testing::Consume(engine::scene::EditableMeshSignature(geometry));
	}
}

BENCH_PER_ITEM("editable mesh terrain-sized prepare", 250) {
	const engine::scene::EditableMeshGeometry &geometry = TerrainChunkGeometry();
	for (uint32_t iteration = 0; iteration < 250; iteration++) {
		engine::testing::Consume(engine::scene::PrepareEditableMesh(geometry).Signature);
	}
}

BENCH_PER_ITEM("editable collision terrain refresh beside resident shapes", 250) {
	DirtyTerrainWorld &world = TerrainWorld();
	for (uint32_t iteration = 0; iteration < 250; iteration++) {
		// Eight chunks clear RefreshEditableMeshCollision's two-item dispatch floor,
		// so the measured path includes its Jobs::For worker split.
		for (const engine::ecs::Entity entity : world.TerrainMeshes) {
			world.Scene.GetMutable<engine::scene::EditableMesh>(entity)->Revision++;
		}
		engine::testing::Consume(engine::scene::RefreshEditableMeshCollision(world.Scene));
	}
}

BENCH_PER_ITEM("editable collision retained terrain ledger", 1'000) {
	RetainedTerrainWorld &world = RetainedTerrainWorldOf();
	for (uint32_t iteration = 0; iteration < 1'000; iteration++) {
		engine::testing::Consume(engine::scene::RefreshEditableMeshCollision(world.Scene));
	}
}
