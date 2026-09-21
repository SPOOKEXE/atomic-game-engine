#include <engine/scene/RenderFeatures.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <type_traits>

TEST_SUITE_ID("engine.scene.renderfeatures")

using namespace engine::scene;

TEST_CASE("render feature policy resolves by scope and device support", "[scene][render]") {
	const uint32_t shadows = FeatureBit(RenderFeature::Shadows);
	const uint32_t ao = FeatureBit(RenderFeature::AmbientOcclusion);
	const uint32_t post = FeatureBit(RenderFeature::PostProcessing);
	const uint32_t tracing = FeatureBit(RenderFeature::RayTracing);

	const ResolvedRenderFeatures resolved = ResolveRenderFeatures(
		shadows | ao,
		{.Enable = post},
		{.Enable = tracing, .Disable = ao},
		{.Enable = ao, .Disable = shadows},
		shadows | ao | post
	);

	CHECK(resolved.Enabled == (ao | post));
	CHECK(resolved.Refused == tracing);
}

TEST_CASE("disable wins over enable in one authored feature layer", "[scene][render]") {
	const uint32_t emission = FeatureBit(RenderFeature::Emission);
	CHECK(ApplyRenderFeaturePolicy(0, {.Enable = emission, .Disable = emission}) == 0);
	CHECK(ApplyRenderFeaturePolicy(UINT32_MAX, {}) == ALL_RENDER_FEATURES);
}

TEST_CASE("world camera and instance precedence matches the GPU material policy", "[scene][render]") {
	const uint32_t emission = FeatureBit(RenderFeature::Emission);
	const uint32_t displacement = FeatureBit(RenderFeature::Displacement);
	const uint32_t shadows = FeatureBit(RenderFeature::Shadows);

	const ResolvedRenderFeatures resolved = ResolveRenderFeatures(
		emission | displacement | shadows,
		{.Disable = emission},
		{.Enable = emission, .Disable = displacement},
		{.Enable = displacement, .Disable = emission},
		emission | displacement | shadows
	);

	// Camera restores emission, then the instance suppresses it. The instance
	// also restores displacement after the camera suppressed it.
	CHECK(resolved.Enabled == (displacement | shadows));
}

TEST_CASE("render effect attachments are flat snapshot data", "[scene][render]") {
	CHECK(std::is_trivially_copyable_v<RenderFeaturePolicy>);
	CHECK(std::is_trivially_copyable_v<RenderEffectAttachment>);
	CHECK(std::is_trivially_copyable_v<RenderEffects>);

	RenderEffects effects;
	effects.Attachments[0].Node = engine::core::Name("outline");
	effects.Attachments[0].Stage = RenderEffectStage::PostProcess;
	effects.Count = 1;

	CHECK(effects.Attachments[0].Node == engine::core::Name("outline"));
	CHECK(effects.Attachments[0].SelectionMask == UINT32_MAX);
}
