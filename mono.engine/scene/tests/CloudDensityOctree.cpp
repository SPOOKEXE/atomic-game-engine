#include <engine/scene/CloudDensity.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <limits>

TEST_SUITE_ID("engine.scene.clouddensityoctree")

using engine::core::Vector3;
using engine::scene::CloudDensityOctree;
using engine::scene::CloudDensityOctreeConfig;

TEST_CASE("cloud density octrees retain only occupied adaptive paths", "[scene][cloud]") {
	CloudDensityOctreeConfig invalid;
	invalid.RootSize.X = 0.0f;
	CHECK_FALSE(CloudDensityOctree::Create(invalid).has_value());
	invalid = {};
	invalid.MaximumDepth = 21;
	CHECK_FALSE(CloudDensityOctree::Create(invalid).has_value());

	const CloudDensityOctreeConfig config{
		.RootMinimum = {-8.0f, 0.0f, -4.0f},
		.RootSize = {16.0f, 8.0f, 8.0f},
		.MaximumDepth = 3,
		.EmptyThreshold = 0.001f,
	};
	auto created = CloudDensityOctree::Create(config);
	REQUIRE(created.has_value());
	CloudDensityOctree &tree = *created;
	CHECK(tree.NodeCount() == 1);
	CHECK(tree.StoredVoxelCount() == 0);
	CHECK(tree.VoxelSize().FuzzyEq({2.0f, 1.0f, 1.0f}));
	CHECK(tree.SampleDensity({-7.5f, 0.5f, -3.5f}) == Catch::Approx(0.0f));
	CHECK_FALSE(tree.SampleDensity({8.0f, 1.0f, 0.0f}).has_value());

	const Vector3 first{-7.5f, 0.5f, -3.5f};
	const Vector3 sameVoxel{-6.1f, 0.9f, -3.1f};
	CHECK(tree.SetDensity(first, 0.8f));
	CHECK(tree.NodeCount() == 4);
	CHECK(tree.StoredVoxelCount() == 1);
	CHECK(tree.SampleDensity(sameVoxel) == Catch::Approx(0.8f));
	CHECK(tree.SetDensity(first, 0.6f));
	CHECK(tree.NodeCount() == 4);
	CHECK_FALSE(tree.SetDensity(first, std::numeric_limits<float>::quiet_NaN()));
	CHECK(tree.SetDensity(first, 0.7f, 1));
	CHECK(tree.SetDensity(first, 0.9f, 3));
	CHECK(tree.SampleDensity(first) == Catch::Approx(0.9f));
	CHECK(tree.SampleDensity({-5.5f, 0.5f, -3.5f}) == Catch::Approx(0.7f));
	CHECK(tree.SetDensity(first, 0.0f, 1));
	CHECK(tree.NodeCount() == 1);
	CHECK(tree.StoredVoxelCount() == 0);
}

TEST_CASE("a cloud density snapshot retains its packed field after a source rebuild", "[scene][cloud]") {
	auto created = CloudDensityOctree::Create({
		.RootMinimum = {-4, -4, -4},
		.RootSize = {8, 8, 8},
		.MaximumDepth = 2,
	});
	REQUIRE(created.has_value());
	REQUIRE(created->SetDensity({-3, -3, -3}, 0.75f));
	engine::scene::CloudDensitySnapshot snapshot{
		.Centre = {10, 20, 30},
		.Config = created->Config(),
		.Nodes = created->GpuNodes(),
	};
	REQUIRE(snapshot.IsValid());
	const size_t packedNodes = snapshot.Nodes.size();
	const float averageDensity = snapshot.Nodes.front().Values[0];
	CHECK(averageDensity > 0);
	created->Clear();
	CHECK(created->StoredVoxelCount() == 0);
	CHECK(snapshot.IsValid());
	CHECK(snapshot.Nodes.size() == packedNodes);
	CHECK(snapshot.Nodes.front().Values[0] == averageDensity);
	CHECK(snapshot.Config.RootMinimum == Vector3{-4, -4, -4});
	CHECK(snapshot.Centre == Vector3{10, 20, 30});
}
