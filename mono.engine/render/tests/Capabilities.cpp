#include <engine/render/Capabilities.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.render.capabilities")
TEST_DEPENDS("engine.graph.pipelinecatalogue")

namespace engine::render::tests {

	TEST_CASE("capability checks name each mandatory feature", "[render][capabilities]") {
		DeviceCaps caps;
		graph::NodeRequirements needs;

		needs.Compute = true;
		CHECK(CheckCapabilities(caps, needs).Status == CapabilityStatus::MissingCompute);
		caps.HasCompute = true;

		needs.StorageTextures = true;
		CHECK(CheckCapabilities(caps, needs).Status == CapabilityStatus::MissingStorageTextures);
		caps.HasStorageTextures = true;

		needs.IndirectDraws = true;
		CHECK(CheckCapabilities(caps, needs).Status == CapabilityStatus::MissingIndirectDraws);
		caps.HasIndirectDraws = true;

		needs.Formats = {graph::ResourceFormat::RGBA16F};
		const CapabilityCheck missingFormat = CheckCapabilities(caps, needs);
		CHECK(missingFormat.Status == CapabilityStatus::MissingFormat);
		CHECK(missingFormat.Format == graph::ResourceFormat::RGBA16F);

		caps.Formats.push_back(graph::ResourceFormat::RGBA16F);
		CHECK(CheckCapabilities(caps, needs).Accepted());
	}

	TEST_CASE("timestamps are useful rather than mandatory", "[render][capabilities]") {
		DeviceCaps caps;
		graph::NodeRequirements needs;
		needs.TimestampsUseful = true;

		CHECK_FALSE(caps.HasTimestamps);
		CHECK(CheckCapabilities(caps, needs).Accepted());
	}

	TEST_CASE("default pipeline tiers retain exact fallthrough causes", "[render][capabilities]") {
		DeviceCaps caps;
		caps.HasIndirectDraws = true;
		caps.Formats = {
			graph::ResourceFormat::RGBA8,
			graph::ResourceFormat::RGBA8_SRGB,
			graph::ResourceFormat::RGB10A2,
			graph::ResourceFormat::RGBA16F,
			graph::ResourceFormat::R32F,
			graph::ResourceFormat::D24S8,
			graph::ResourceFormat::D32F,
		};

		const PipelineTierDecision tierB = ChooseDefaultPipeline(caps);
		CHECK(tierB.Tier == DefaultPipelineTier::B);
		REQUIRE(tierB.Fallthrough.size() == 1);
		CHECK(tierB.Fallthrough[0].Tier == DefaultPipelineTier::A);
		CHECK(tierB.Fallthrough[0].Cause.Status == CapabilityStatus::MissingCompute);

		caps.HasIndirectDraws = false;
		const PipelineTierDecision tierC = ChooseDefaultPipeline(caps);
		CHECK(tierC.Tier == DefaultPipelineTier::C);
		REQUIRE(tierC.Fallthrough.size() == 2);
		CHECK(tierC.Fallthrough[1].Tier == DefaultPipelineTier::B);
		CHECK(tierC.Fallthrough[1].Cause.Status == CapabilityStatus::MissingIndirectDraws);
	}
}
