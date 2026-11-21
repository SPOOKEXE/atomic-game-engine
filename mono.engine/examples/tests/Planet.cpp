// The procedural planet demo must make a mixed-resolution quadtree surface
// continuous in the same scene data the renderer consumes.

#include <engine/core/Paths.hpp>
#include <engine/ecs/Scheduler.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/examples/Scene.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/EditableMesh.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Services.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

TEST_SUITE_ID("engine.examples.planet")
TEST_DEPENDS("engine.examples.scene")

namespace {
	using engine::core::Vector3;
	using engine::ecs::Entity;
	using engine::ecs::Store;
	using engine::scene::EditableMesh;
	using engine::scene::Transform;

	constexpr size_t PATCH_SIDE = 17;
	constexpr size_t PATCH_VERTICES = PATCH_SIDE * PATCH_SIDE;
	constexpr size_t PATCH_INDICES = (PATCH_SIDE - 1) * (PATCH_SIDE - 1) * 6;
	constexpr std::string_view MESH_PREFIX = "PlanetMesh_Haven_";
	constexpr std::string_view PART_PREFIX = "PlanetChunk_Haven_";

	struct StagedAssets {
		std::filesystem::path Previous = engine::core::Paths::Assets();

		StagedAssets() {
			engine::core::Paths::SetAssetsOverride(engine::core::Paths::Base().parent_path() / "assets");
		}

		~StagedAssets() {
			engine::core::Paths::SetAssetsOverride(Previous);
		}
	};

	struct Edge {
		Vector3 First;
		Vector3 Second;
		size_t Patch = 0;
	};

	float DistanceToSegment(const Vector3 &point, const Edge &edge) {
		const Vector3 span = edge.Second - edge.First;
		const float lengthSquared = span.Dot(span);
		if (lengthSquared <= 1e-8f) return (point - edge.First).Magnitude();
		const float along = std::clamp((point - edge.First).Dot(span) / lengthSquared, 0.0f, 1.0f);
		return (point - (edge.First + span * along)).Magnitude();
	}

	bool NearAnyVertex(const Vector3 &point, const std::vector<Vector3> &vertices) {
		return std::any_of(vertices.begin(), vertices.end(), [&](const Vector3 &other) {
			return (point - other).Magnitude() < 0.001f;
		});
	}

	// Edges bucketed by the grid cells their tolerance-padded bounds touch, so
	// a point only measures the edges near it. Comparing every point against
	// every edge took 21.6 s of this suite on `dev` for 16128 edges.
	class EdgeGrid {
	  public:
		EdgeGrid(const std::vector<Edge> &edges, float tolerance) : Tolerance(tolerance) {
			float longest = tolerance;
			for (const Edge &edge : edges)
				longest = std::max(longest, (edge.Second - edge.First).Magnitude());
			Cell = longest;
			for (uint32_t index = 0; index < edges.size(); index++) {
				const Edge &edge = edges[index];
				const Vector3 low{
					std::min(edge.First.X, edge.Second.X) - tolerance,
					std::min(edge.First.Y, edge.Second.Y) - tolerance,
					std::min(edge.First.Z, edge.Second.Z) - tolerance,
				};
				const Vector3 high{
					std::max(edge.First.X, edge.Second.X) + tolerance,
					std::max(edge.First.Y, edge.Second.Y) + tolerance,
					std::max(edge.First.Z, edge.Second.Z) + tolerance,
				};
				for (int64_t x = Coordinate(low.X); x <= Coordinate(high.X); x++)
					for (int64_t y = Coordinate(low.Y); y <= Coordinate(high.Y); y++)
						for (int64_t z = Coordinate(low.Z); z <= Coordinate(high.Z); z++)
							Cells[Key(x, y, z)].push_back(index);
			}
		}

		// The lowest-index edge of another patch within tolerance of `point`, the
		// same answer a front-to-back scan of `edges` would give.
		const Edge *FirstJoining(const Vector3 &point, size_t patch, const std::vector<Edge> &edges) const {
			const auto found = Cells.find(Key(Coordinate(point.X), Coordinate(point.Y), Coordinate(point.Z)));
			if (found == Cells.end()) return nullptr;
			const Edge *first = nullptr;
			uint32_t firstIndex = UINT32_MAX;
			for (const uint32_t index : found->second) {
				const Edge &other = edges[index];
				if (index >= firstIndex || other.Patch == patch || DistanceToSegment(point, other) > Tolerance)
					continue;
				first = &other;
				firstIndex = index;
			}
			return first;
		}

	  private:
		int64_t Coordinate(float value) const {
			return static_cast<int64_t>(std::floor(value / Cell));
		}
		static uint64_t Key(int64_t x, int64_t y, int64_t z) {
			const auto bits = [](int64_t value) { return static_cast<uint64_t>(value) & 0x1fffffu; };
			return bits(x) | (bits(y) << 21) | (bits(z) << 42);
		}

		float Tolerance;
		float Cell = 1.0f;
		std::unordered_map<uint64_t, std::vector<uint32_t>> Cells;
	};
}

TEST_CASE("the planet quadtree stitches every mixed-resolution edge", "[slow][examples][scene][planet]") {
	const StagedAssets assets;
	Store store("planet.stitch");
	engine::ecs::Scheduler systems;
	std::string error;
	REQUIRE(engine::examples::LoadScene(store, systems, engine::examples::ExamplePath("Planet.luau"), error));
	const Entity camera = store.CreateInstance(engine::scene::CameraClass(), "PlanetStitchCamera");
	REQUIRE(camera != engine::ecs::NULL_ENTITY);
	store.Set<Transform>(camera, Transform{engine::core::CFrame(Vector3{0.0f, 20.0f, -150.0f})});
	REQUIRE(store.SetParent(camera, engine::scene::WorkspaceOf(store)));
	store.SetResource(engine::scene::ActiveCamera{camera});

	// One patch commits per script heartbeat. This reaches a settled mixed-depth
	// leaf set rather than inspecting the initial six roots.
	for (size_t tick = 0; tick < 720; tick++) {
		systems.Tick(store, 1.0f / 60.0f);
	}

	std::vector<Edge> edges;
	std::vector<Vector3> boundaryVertices;
	size_t patches = 0;

	store.Each<const EditableMesh>([&](Entity meshEntity, const EditableMesh &mesh) {
		const std::string_view meshName = store.InstanceNameOf(meshEntity).Text();
		if (!meshName.starts_with(MESH_PREFIX) || mesh.Positions.empty()) return;

		const std::string partName =
			std::string(PART_PREFIX) + std::string(meshName.substr(MESH_PREFIX.size()));
		const Entity part = store.FindFirstChild(engine::scene::WorkspaceOf(store), partName);
		REQUIRE(part != engine::ecs::NULL_ENTITY);
		const Transform *transform = store.Get<Transform>(part);
		REQUIRE(transform != nullptr);
		REQUIRE(mesh.Positions.size() == PATCH_VERTICES);
		REQUIRE(mesh.Normals.size() == PATCH_VERTICES);
		REQUIRE(mesh.UVs.size() == PATCH_VERTICES);
		CHECK(mesh.Indices.size() == PATCH_INDICES);

		std::vector<Vector3> world(PATCH_VERTICES);
		for (size_t index = 0; index < PATCH_VERTICES; index++) {
			world[index] = transform->Frame.PointToWorldSpace(mesh.Positions[index]);
			const Vector3 normal = transform->Frame.VectorToWorldSpace(mesh.Normals[index]).Unit();
			CHECK(normal.Dot(world[index].Unit()) > 0.995f);
		}

		const size_t patch = patches++;
		const auto addEdge = [&](size_t first, size_t second) {
			edges.push_back({world[first], world[second], patch});
		};
		const auto addVertex = [&](size_t index) { boundaryVertices.push_back(world[index]); };
		for (size_t index = 0; index + 1 < PATCH_SIDE; index++) {
			addEdge(index, index + 1);
			addEdge((PATCH_SIDE - 1) * PATCH_SIDE + index, (PATCH_SIDE - 1) * PATCH_SIDE + index + 1);
			addEdge(index * PATCH_SIDE, (index + 1) * PATCH_SIDE);
			addEdge(index * PATCH_SIDE + PATCH_SIDE - 1, (index + 1) * PATCH_SIDE + PATCH_SIDE - 1);
		}
		for (size_t index = 0; index < PATCH_SIDE; index++) {
			addVertex(index);
			addVertex((PATCH_SIDE - 1) * PATCH_SIDE + index);
			addVertex(index * PATCH_SIDE);
			addVertex(index * PATCH_SIDE + PATCH_SIDE - 1);
		}
	});

	REQUIRE(patches > 6);
	REQUIRE(!edges.empty());

	// Every border point must land on another patch's border. An exact vertex
	// match covers equal-depth neighbours. A match in the middle of another
	// edge proves the one-level fine-to-coarse stitch, where the old demo made
	// a gap and covered it with a visible inward skirt.
	const EdgeGrid grid(edges, 0.001f);
	size_t stitched = 0;
	size_t unjoined = 0;
	for (const Edge &edge : edges) {
		for (const Vector3 point : {edge.First, edge.Second}) {
			const Edge *other = grid.FirstJoining(point, edge.Patch, edges);
			if (other == nullptr) {
				unjoined++;
				continue;
			}
			if (!NearAnyVertex(point, {other->First, other->Second})) stitched++;
		}
	}
	CHECK(unjoined == 0);
	CHECK(stitched > 0);
}

TEST_CASE("the rotating gas giant keeps its quadtree chunks joined", "[examples][scene][planet]") {
	const StagedAssets assets;
	Store store("planet.zephyr.rotation");
	engine::ecs::Scheduler systems;
	std::string error;
	REQUIRE(engine::examples::LoadScene(store, systems, engine::examples::ExamplePath("Planet.luau"), error));

	// Zephyr turns every heartbeat. This duration reaches a visibly different
	// orientation and a settled mixed-depth leaf set.
	for (size_t tick = 0; tick < 720; tick++) {
		systems.Tick(store, 1.0f / 60.0f);
	}

	constexpr std::string_view meshPrefix = "PlanetMesh_Zephyr_";
	constexpr std::string_view partPrefix = "PlanetChunk_Zephyr_";
	std::vector<Edge> edges;
	size_t patches = 0;

	store.Each<const EditableMesh>([&](Entity meshEntity, const EditableMesh &mesh) {
		const std::string_view meshName = store.InstanceNameOf(meshEntity).Text();
		if (!meshName.starts_with(meshPrefix) || mesh.Positions.empty()) return;

		const std::string partName =
			std::string(partPrefix) + std::string(meshName.substr(meshPrefix.size()));
		const Entity part = store.FindFirstChild(engine::scene::WorkspaceOf(store), partName);
		REQUIRE(part != engine::ecs::NULL_ENTITY);
		const Transform *transform = store.Get<Transform>(part);
		REQUIRE(transform != nullptr);
		size_t patchSide = 1;
		while (patchSide * patchSide < mesh.Positions.size()) {
			patchSide++;
		}
		REQUIRE(patchSide * patchSide == mesh.Positions.size());

		std::vector<Vector3> world(mesh.Positions.size());
		for (size_t index = 0; index < world.size(); index++) {
			world[index] = transform->Frame.PointToWorldSpace(mesh.Positions[index]);
		}

		const size_t patch = patches++;
		const auto addEdge = [&](size_t first, size_t second) {
			edges.push_back({world[first], world[second], patch});
		};
		for (size_t index = 0; index + 1 < patchSide; index++) {
			addEdge(index, index + 1);
			addEdge((patchSide - 1) * patchSide + index, (patchSide - 1) * patchSide + index + 1);
			addEdge(index * patchSide, (index + 1) * patchSide);
			addEdge(index * patchSide + patchSide - 1, (index + 1) * patchSide + patchSide - 1);
		}
	});

	REQUIRE(patches > 6);
	REQUIRE(!edges.empty());

	for (const Edge &edge : edges) {
		for (const Vector3 point : {edge.First, edge.Second}) {
			const bool joined = std::any_of(edges.begin(), edges.end(), [&](const Edge &other) {
				return other.Patch != edge.Patch && DistanceToSegment(point, other) <= 0.001f;
			});
			CHECK(joined);
		}
	}
}
