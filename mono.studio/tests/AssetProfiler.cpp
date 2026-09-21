#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <AssetProfiler.hpp>
#include <AssetProfilerSort.hpp>
#include <array>

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

TEST_CASE("asset profiler sorts every measured column", "[studio][assetprofiler]") {
	for (const studio::AssetProfileColumn column : std::array{
			 studio::AssetProfileColumn::Asset,
			 studio::AssetProfileColumn::Kind,
			 studio::AssetProfileColumn::Pulled,
			 studio::AssetProfileColumn::Decoded,
			 studio::AssetProfileColumn::Cpu,
			 studio::AssetProfileColumn::Gpu,
			 studio::AssetProfileColumn::Updates,
			 studio::AssetProfileColumn::Resident,
			 studio::AssetProfileColumn::Delta,
		 }) {
		studio::AssetProfileSortRow first{.Name = "zebra", .Kind = {}};
		studio::AssetProfileSortRow second{.Name = "alpha", .Kind = {}};
		switch (column) {
		case studio::AssetProfileColumn::Asset:
			first.Name = "alpha";
			second.Name = "zebra";
			break;
		case studio::AssetProfileColumn::Kind:
			first.Kind = "mesh";
			second.Kind = "texture";
			break;
		case studio::AssetProfileColumn::Pulled:
			second.PulledBytes = 1;
			break;
		case studio::AssetProfileColumn::Decoded:
			second.DecodedBytes = 1;
			break;
		case studio::AssetProfileColumn::Cpu:
			second.CpuResidentBytes = 1;
			break;
		case studio::AssetProfileColumn::Gpu:
			second.GpuResidentBytes = 1;
			break;
		case studio::AssetProfileColumn::Updates:
			second.Updates = 1;
			break;
		case studio::AssetProfileColumn::Resident:
			second.ResidentInstances = 1;
			break;
		case studio::AssetProfileColumn::Delta:
			second.StagedBytes = 1;
			break;
		case studio::AssetProfileColumn::TotalResident:
			FAIL("the hidden default order is not a clickable column");
		}

		std::vector<studio::AssetProfileSortRow> rows{second, first};
		const std::array ascending{studio::AssetProfileSort{.Column = column, .Ascending = true}};
		studio::SortAssetProfiles(rows, ascending);
		CAPTURE(static_cast<uint8_t>(column));
		CHECK(rows.front().Name == first.Name);

		const std::array descending{studio::AssetProfileSort{.Column = column, .Ascending = false}};
		studio::SortAssetProfiles(rows, descending);
		CHECK(rows.front().Name == second.Name);
	}
}

TEST_CASE("asset profiler uses names to break measured-value ties", "[studio][assetprofiler]") {
	studio::AssetProfileSortRow alpha{.Name = "asset-sort-alpha", .Kind = {}, .GpuResidentBytes = 32};
	studio::AssetProfileSortRow beta{.Name = "asset-sort-beta", .Kind = {}, .GpuResidentBytes = 32};
	std::vector<studio::AssetProfileSortRow> rows{beta, alpha};
	const std::array sort{
		studio::AssetProfileSort{.Column = studio::AssetProfileColumn::Gpu, .Ascending = false}
	};

	studio::SortAssetProfiles(rows, sort);

	CHECK(rows.front().Name == "asset-sort-alpha");
}

TEST_CASE("asset profiler breaks delta-byte ties by staged instances", "[studio][assetprofiler]") {
	std::vector<studio::AssetProfileSortRow> rows{
		{.Name = "alpha", .Kind = {}, .StagedInstances = 2, .StagedBytes = 32},
		{.Name = "zebra", .Kind = {}, .StagedInstances = 1, .StagedBytes = 32},
	};
	const std::array sort{
		studio::AssetProfileSort{.Column = studio::AssetProfileColumn::Delta, .Ascending = true}
	};

	studio::SortAssetProfiles(rows, sort);

	CHECK(rows.front().Name == "zebra");
}

TEST_CASE("asset profiler applies multi-column rows in header order", "[studio][assetprofiler]") {
	std::vector<studio::AssetProfileSortRow> rows{
		{.Name = "alpha", .Kind = "mesh", .PulledBytes = 1},
		{.Name = "beta", .Kind = "mesh", .PulledBytes = 2},
		{.Name = "gamma", .Kind = "texture", .PulledBytes = 3},
	};
	const std::array sorts{
		studio::AssetProfileSort{.Column = studio::AssetProfileColumn::Kind, .Ascending = true},
		studio::AssetProfileSort{.Column = studio::AssetProfileColumn::Pulled, .Ascending = false},
	};

	studio::SortAssetProfiles(rows, sorts);

	CHECK(rows[0].Name == "beta");
	CHECK(rows[1].Name == "alpha");
	CHECK(rows[2].Name == "gamma");
}

TEST_CASE("asset profiler retains total resident bytes as its unsorted order", "[studio][assetprofiler]") {
	std::vector<studio::AssetProfileSortRow> rows{
		{.Name = "cpu", .Kind = {}, .CpuResidentBytes = 32, .GpuResidentBytes = 16},
		{.Name = "gpu", .Kind = {}, .CpuResidentBytes = 8, .GpuResidentBytes = 64},
	};
	const std::array sort{
		studio::AssetProfileSort{.Column = studio::AssetProfileColumn::TotalResident, .Ascending = false}
	};

	studio::SortAssetProfiles(rows, sort);

	CHECK(rows.front().Name == "gpu");
}
