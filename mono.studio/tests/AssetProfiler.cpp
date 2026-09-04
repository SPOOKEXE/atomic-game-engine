#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <AssetProfiler.hpp>

TEST_SUITE_ID("studio.assetprofiler")

TEST_CASE("asset profiler separates mesh host payload from its gpu payload", "[studio][assetprofiler]") {
	engine::assets::MeshData mesh;
	mesh.Vertices.resize(3);
	mesh.Indices = {0, 1, 2};

	const studio::AssetFootprint footprint = studio::MeshFootprint(mesh);
	const uint64_t expected = 3 * sizeof(engine::assets::MeshVertex) + 3 * sizeof(uint32_t);
	CHECK(footprint.DecodedBytes == expected);
	CHECK(footprint.CpuResidentBytes == expected);
	CHECK(footprint.GpuResidentBytes == expected);
}

TEST_CASE("asset profiler counts every expanded texture mip on the gpu", "[studio][assetprofiler]") {
	engine::assets::TextureData texture;
	texture.Width = 4;
	texture.Height = 2;
	texture.Format = engine::assets::TextureFormat::R8;
	texture.Pixels.resize(8);
	texture.Mips = {std::vector<std::byte>(2), std::vector<std::byte>(1)};

	const studio::AssetFootprint footprint = studio::TextureFootprint(texture);
	CHECK(footprint.DecodedBytes == 11);
	CHECK(footprint.CpuResidentBytes == 0);
	CHECK(footprint.GpuResidentBytes == (4 * 2 + 2 * 1 + 1 * 1) * 4);
}
