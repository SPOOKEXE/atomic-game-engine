// The steady collision refresh paid by every presented Studio world.

#include <engine/collision/TriangleMesh.hpp>
#include <engine/core/Name.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/parallel/Jobs.hpp>
#include <engine/scene/CollisionShapes.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/editable/EditableMesh.hpp>
#include <engine/testing/Bench.hpp>

#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <vector>

TEST_SUITE_ID("engine.scene.bench.editablemesh")

namespace {
	void DemandMeshCollision(engine::ecs::Store &store, engine::ecs::Entity mesh) {
		engine::scene::Collider collider;
		collider.Shape = engine::scene::ShapeKind::Mesh;
		collider.Geometry = engine::scene::EditableMeshContentName(store, mesh);
		store.Set(store.Create(), collider);
	}

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
			DemandMeshCollision(store, entity);

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
			// Studio and the client run this refresh with the process pool
			// started, so the eight rebakes spread over workers as they do there.
			// Started in the fixture so the measured body never pays for it.
			engine::parallel::Jobs::Start(0);
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
				DemandMeshCollision(world.Scene, entity);
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
				DemandMeshCollision(world.Scene, entity);
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

namespace {
	// A 25,000-vertex skinned body as one interleaved buffer: position,
	// normal, UV, four u16 joints and four f32 weights, 56 bytes a vertex.
	struct PackedBody {
		std::vector<std::byte> Vertices;
		std::vector<std::byte> Indices;
		engine::scene::EditableBufferLayout Layout;
	};

	const PackedBody &BodyBuffers() {
		static const PackedBody body = [] {
			constexpr uint32_t side = 158;
			constexpr uint32_t count = 25'000;
			PackedBody packed;
			packed.Vertices.resize(size_t(count) * 56);
			for (uint32_t vertex = 0; vertex < count; vertex++) {
				std::byte *at = packed.Vertices.data() + size_t(vertex) * 56;
				const float fields[8] = {float(vertex % side), float(vertex / side), 0, 0, 0, 1, 0, 0};
				std::memcpy(at, fields, sizeof(fields));
				const uint16_t joints[4] = {uint16_t(vertex % 60), uint16_t((vertex + 1) % 60), 0, 0};
				std::memcpy(at + 32, joints, sizeof(joints));
				const float weights[4] = {0.7f, 0.3f, 0, 0};
				std::memcpy(at + 40, weights, sizeof(weights));
			}
			std::vector<uint32_t> indices;
			for (uint32_t row = 0; row + 1 < count / side; row++)
				for (uint32_t column = 0; column + 1 < side; column++) {
					const uint32_t at = row * side + column;
					indices.insert(indices.end(), {at, at + side, at + 1, at + 1, at + side, at + side + 1});
				}
			packed.Indices.resize(indices.size() * 4);
			std::memcpy(packed.Indices.data(), indices.data(), packed.Indices.size());
			auto &layout = packed.Layout;
			layout.VertexCount = count;
			layout.Position = {true, 0, 56};
			layout.Normal = {true, 12, 56};
			layout.UV = {true, 24, 56};
			layout.Joints = {true, 32, 56};
			layout.Weights = {true, 40, 56};
			return packed;
		}();
		return body;
	}
}

// The slider-change path for a script-built body: decode the buffers, then
// validate, sign and commit them. The acceptance budget is 10 ms per item.
BENCH_PER_ITEM("editable mesh 25k skinned vertices from buffers", 20) {
	static engine::ecs::Store store("bench.editablemesh.body");
	static const engine::ecs::Entity mesh = [] {
		engine::scene::RegisterSceneComponents();
		return store.CreateInstance(engine::scene::EditableMeshClass(), "Body");
	}();
	const PackedBody &body = BodyBuffers();
	for (uint32_t iteration = 0; iteration < 20; iteration++) {
		engine::scene::EditableMeshGeometry geometry;
		const std::string refused =
			engine::scene::DecodeEditableMeshBuffers(body.Layout, body.Vertices, body.Indices, geometry);
		// Moving one vertex keeps every commit a real change.
		geometry.Positions[0].Z = float(iteration);
		const uint32_t revision = store.Get<engine::scene::EditableMesh>(mesh)->Revision;
		engine::testing::Consume(refused.size());
		engine::testing::Consume(
			static_cast<uint32_t>(engine::scene::CommitEditableMesh(
				store, mesh, engine::scene::PrepareEditableMesh(std::move(geometry)), revision
			))
		);
	}
}

// The same slider change as a stream edit: positions and normals of the
// committed body, rewritten from one packed buffer each.
BENCH_PER_ITEM("editable mesh 25k skinned vertex streams from buffers", 20) {
	static engine::ecs::Store store("bench.editablemesh.streams");
	static const engine::ecs::Entity mesh = [] {
		engine::scene::RegisterSceneComponents();
		const engine::ecs::Entity body = store.CreateInstance(engine::scene::EditableMeshClass(), "Body");
		const PackedBody &packed = BodyBuffers();
		engine::scene::EditableMeshGeometry geometry;
		(void)engine::scene::DecodeEditableMeshBuffers(
			packed.Layout, packed.Vertices, packed.Indices, geometry
		);
		(void)engine::scene::ReplaceEditableMesh(store, body, std::move(geometry));
		return body;
	}();
	static std::vector<float> positions = [] {
		std::vector<float> values;
		for (const auto &position : store.Get<engine::scene::EditableMesh>(mesh)->Positions)
			values.insert(values.end(), {position.X, position.Y, position.Z});
		return values;
	}();
	static const std::vector<float> normals(positions.size(), 0.5f);
	for (uint32_t iteration = 0; iteration < 20; iteration++) {
		positions[2] += 1.0f;
		engine::testing::Consume(
			engine::scene::SetEditableVertexStream(
				store,
				mesh,
				engine::scene::EditableVertexStream::Position,
				std::as_bytes(std::span(positions))
			)
		);
		engine::testing::Consume(
			engine::scene::SetEditableVertexStream(
				store, mesh, engine::scene::EditableVertexStream::Normal, std::as_bytes(std::span(normals))
			)
		);
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
