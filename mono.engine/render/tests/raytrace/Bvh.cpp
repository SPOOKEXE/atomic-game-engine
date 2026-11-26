#include <engine/render/raytrace/Bvh.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>
#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>

#include <random>

TEST_SUITE_ID("engine.render.raytrace.bvh")

using namespace engine;
using namespace engine::render;

namespace {
	struct Soup {
		std::vector<float> Positions;
		std::vector<uint32_t> Indices;
	};

	// Random small triangles in a 20 unit cube.
	Soup Triangles(uint32_t count, uint32_t seed) {
		std::mt19937 random(seed);
		std::uniform_real_distribution<float> where(-10.0f, 10.0f), offset(-1.0f, 1.0f);
		Soup soup;
		for (uint32_t t = 0; t < count; ++t) {
			const glm::vec3 centre(where(random), where(random), where(random));
			for (int k = 0; k < 3; ++k) {
				soup.Positions.insert(soup.Positions.end(), {centre.x + offset(random), centre.y + offset(random), centre.z + offset(random)});
				soup.Indices.push_back(t * 3 + k);
			}
		}
		return soup;
	}

	RayHit Brute(const Soup &soup, const Ray &ray) {
		RayHit best;
		for (uint32_t t = 0; t < soup.Indices.size() / 3; ++t) {
			const auto corner = [&](int k) {
				const uint32_t i = soup.Indices[t * 3 + k];
				return glm::vec3(soup.Positions[i * 3], soup.Positions[i * 3 + 1], soup.Positions[i * 3 + 2]);
			};
			float d = 0, u = 0, v = 0;
			if (RayTriangle(ray, corner(0), corner(1), corner(2), d, u, v) && d < best.Distance) best = {true, d, 0, t, u, v};
		}
		return best;
	}

	Ray RandomRay(std::mt19937 &random) {
		std::uniform_real_distribution<float> where(-12.0f, 12.0f);
		Ray ray;
		ray.Origin = {where(random), where(random), where(random)};
		ray.Direction = glm::normalize(glm::vec3(where(random), where(random), where(random)) - ray.Origin);
		return ray;
	}
}

TEST_CASE("a mesh BVH finds the closest hit brute force finds", "[render][raytrace]") {
	const Soup soup = Triangles(2000, 7);
	MeshBvh bvh;
	bvh.Build(soup.Positions, soup.Indices);
	CHECK(bvh.Nodes().size() > 100);
	std::mt19937 random(11);
	uint32_t hits = 0;
	for (int index = 0; index < 2000; ++index) {
		const Ray ray = RandomRay(random);
		const RayHit expected = Brute(soup, ray);
		const RayHit found = bvh.Closest(ray);
		REQUIRE(found.Found == expected.Found);
		CHECK(bvh.Any(ray) == expected.Found);
		if (!expected.Found) continue;
		++hits;
		CHECK(found.Triangle == expected.Triangle);
		CHECK(std::abs(found.Distance - expected.Distance) < 1e-4f);
	}
	CHECK(hits > 200);
}

TEST_CASE("a scene BVH traces each instance through its frame and scale", "[render][raytrace]") {
	const Soup soup = Triangles(300, 3);
	MeshBvh mesh;
	mesh.Build(soup.Positions, soup.Indices);
	std::vector<BvhInstance> instances;
	Soup world;
	std::mt19937 random(5);
	std::uniform_real_distribution<float> where(-40.0f, 40.0f), angle(0.0f, 6.28f), scale(0.5f, 2.0f);
	for (uint32_t index = 0; index < 12; ++index) {
		BvhInstance instance;
		instance.Mesh = &mesh;
		const glm::quat rotation = glm::angleAxis(angle(random), glm::normalize(glm::vec3(where(random), where(random), where(random))));
		instance.Frame = core::CFrame(core::Vector3{where(random), where(random), where(random)}, rotation);
		instance.Scale = {scale(random), scale(random), scale(random)};
		instances.push_back(instance);
		// The same triangles placed in the world, for brute force.
		const glm::vec3 position(instance.Frame.Position.X, instance.Frame.Position.Y, instance.Frame.Position.Z);
		const uint32_t base = uint32_t(world.Positions.size() / 3);
		for (size_t p = 0; p < soup.Positions.size(); p += 3) {
			const glm::vec3 placed = position + rotation * (glm::vec3(soup.Positions[p], soup.Positions[p + 1], soup.Positions[p + 2]) * instance.Scale);
			world.Positions.insert(world.Positions.end(), {placed.x, placed.y, placed.z});
		}
		for (const uint32_t i : soup.Indices) world.Indices.push_back(base + i);
	}
	SceneBvh scene;
	scene.Build(instances);
	std::uniform_real_distribution<float> far(-50.0f, 50.0f);
	uint32_t hits = 0;
	for (int index = 0; index < 1500; ++index) {
		Ray ray;
		ray.Origin = {far(random), far(random), far(random)};
		ray.Direction = glm::normalize(glm::vec3(far(random), far(random), far(random)) - ray.Origin);
		const RayHit expected = Brute(world, ray);
		const RayHit found = scene.Closest(ray);
		REQUIRE(found.Found == expected.Found);
		CHECK(scene.Any(ray) == expected.Found);
		if (!expected.Found) continue;
		++hits;
		CHECK(found.Instance == expected.Triangle / 300);
		CHECK(found.Triangle == expected.Triangle % 300);
		CHECK(std::abs(found.Distance - expected.Distance) < 1e-3f * std::max(1.0f, expected.Distance));
	}
	CHECK(hits > 50);
}
