#include <engine/render/PipelineAdmission.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.render.pipelineadmission")

TEST_CASE("pipeline admission result distinguishes acceptance and refusal", "[render][graph]") {
	using engine::core::Name;
	using engine::render::PipelineAdmissionResult;
	using engine::render::PipelineAdmissionStage;
	using engine::render::PipelineFailure;

	CHECK(PipelineAdmissionResult{});
	const PipelineAdmissionResult refused{
		.Failure = PipelineFailure{
			.Stage = PipelineAdmissionStage::Capability,
			.Offender = Name("rgba16f-source"),
			.Reason = "the device does not support the required format",
		},
	};
	CHECK_FALSE(refused);
	CHECK(
		engine::render::FormatPipelineFailure(*refused.Failure) ==
		"refused during device capability: the device does not support the required format at "
		"'rgba16f-source'"
	);
}
