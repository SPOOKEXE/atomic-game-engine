#include <engine/scene/CloudDensity.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <limits>

TEST_SUITE_ID("engine.scene.clouddensity")
TEST_DEPENDS("engine.scene.storm")

using engine::core::Vector3;
using engine::scene::BuildCloudDensity;
using engine::scene::CloudDensityBuildConfig;
using engine::scene::CloudDensityOctree;
using engine::scene::CloudDensityOctreeConfig;
using engine::scene::PrepareTornadoField;
using engine::scene::TornadoParameters;
using engine::scene::WindLineCloudDensity;

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

TEST_CASE("cloud density matches TornadoSim fixtures and builds sparse funnel detail", "[scene][cloud]") {
	const auto field = PrepareTornadoField({});
	const Vector3 eyewall{field.Parameters.CoreRadius, field.Parameters.TopHeight * 0.42f, 0.0f};
	const float eyewallDensity = WindLineCloudDensity(field, eyewall, 2.0f);
	const float outerDensity =
		WindLineCloudDensity(field, {field.Parameters.InfluenceRadius * 1.8f, eyewall.Y, 0.0f}, 2.0f);
	TornadoParameters dry = field.Parameters;
	dry.Humidity = 0.12f;
	dry.PressureDrop = 18.0f;
	const float dryDensity = WindLineCloudDensity(PrepareTornadoField(dry), eyewall, 2.0f);
	CHECK(eyewallDensity > outerDensity);
	CHECK(eyewallDensity > dryDensity);
	CHECK(WindLineCloudDensity(field, {0.0f, field.Parameters.TopHeight * 1.3f, 0.0f}, 2.0f) == 0.0f);

	const CloudDensityOctreeConfig config{
		.RootMinimum = {-field.Parameters.InfluenceRadius, 0.0f, -field.Parameters.InfluenceRadius},
		.RootSize =
			{field.Parameters.InfluenceRadius * 2.0f,
			 field.Parameters.TopHeight * 1.16f,
			 field.Parameters.InfluenceRadius * 2.0f},
		.MaximumDepth = 7,
		.EmptyThreshold = 0.001f,
	};
	auto created = CloudDensityOctree::Create(config);
	REQUIRE(created.has_value());
	const auto build = BuildCloudDensity(*created, field, 2.0f);
	CHECK(build.EvaluatedCells > 4096);
	CHECK(build.CoarseCells > 0);
	CHECK(build.FineCells > 0);
	CHECK(build.Nodes == created->NodeCount());
	CHECK(build.StoredLeaves == created->StoredVoxelCount());
	CHECK(build.Nodes < 250000);
	CHECK(created->GpuNodes().size() >= created->NodeCount());

	const auto unchanged = BuildCloudDensity(*created, field, std::numeric_limits<float>::quiet_NaN());
	CHECK(unchanged.EvaluatedCells == 0);
}
