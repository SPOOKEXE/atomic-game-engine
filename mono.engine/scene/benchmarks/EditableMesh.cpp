// The steady collision refresh paid by every presented Studio world.

#include <engine/collision/TriangleMesh.hpp>
#include <engine/core/Name.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/CollisionShapes.hpp>
#include <engine/scene/EditableMesh.hpp>
#include <engine/testing/Bench.hpp>

#include <cstdint>
#include <string>

TEST_SUITE_ID("engine.scene.bench.editablemesh")

namespace {
	engine::ecs::Store &StaticMeshWorld() {
		static engine::ecs::Store store("bench.editablemesh.static");
		static const bool ready = [] {
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
