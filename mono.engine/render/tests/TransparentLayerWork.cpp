#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <TransparentLayerWork.hpp>

TEST_SUITE_ID("engine.render.transparentlayerwork")

TEST_CASE("effect-only transparent layers retain nearest and colour phases", "[render][transparent-layer]") {
	using engine::render::TransparentLayerWork;

	CHECK(TransparentLayerWork{}.PhaseCount() == 1);
	CHECK(TransparentLayerWork{.Particles = 1}.HasSceneVisuals());
	CHECK(TransparentLayerWork{.Particles = 1}.PhaseCount() == 2);
	CHECK(TransparentLayerWork{.Ribbons = 1}.HasSceneVisuals());
	CHECK(TransparentLayerWork{.Ribbons = 1}.PhaseCount() == 2);
	CHECK(TransparentLayerWork{.InterfaceBatches = 1}.PhaseCount() == 2);
	CHECK(
		TransparentLayerWork{.Meshes = 3, .Particles = 8, .Ribbons = 4, .InterfaceBatches = 2}.PhaseCount() ==
		2
	);
}

TEST_CASE("ordered opaque captures include complete world effects", "[render][transparent-layer]") {
	using engine::render::CaptureIncludesWorldEffects;
	using engine::render::PortalImageScope;

	CHECK(CaptureIncludesWorldEffects(PortalImageScope::CompleteWorld, false));
	CHECK(CaptureIncludesWorldEffects(PortalImageScope::CompleteWorld, true));
	CHECK_FALSE(CaptureIncludesWorldEffects(PortalImageScope::OpaqueLighting, false));
	CHECK(CaptureIncludesWorldEffects(PortalImageScope::OpaqueLighting, true));
}
