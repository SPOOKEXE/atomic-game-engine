#include <engine/core/types/CFrame.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <studio/ViewportDiagnostics.hpp>

TEST_SUITE_ID("studio.viewport_diagnostics")

TEST_CASE("a locked virtual camera holds its whole pose and lens", "[studio][viewport]") {
	studio::ViewportDiagnostics diagnostics;
	const engine::core::CFrame locked(
		engine::core::Vector3{2.0f, 3.0f, 4.0f}, engine::core::CFrame::Angles(0.2f, 0.4f, 0.1f).Rotation()
	);
	const engine::core::CFrame inspected(
		engine::core::Vector3{9.0f, 8.0f, 7.0f}, engine::core::CFrame::Angles(-0.3f, 1.1f, 0.0f).Rotation()
	);

	diagnostics.LockFrustum(locked, 1.2f, 1.5f);
	CHECK(diagnostics.FrustumLocked);
	// The inspection camera moves; the virtual camera and its marker do not.
	CHECK(diagnostics.EffectiveFrustum(inspected).Position == locked.Position);
	CHECK(diagnostics.EffectiveFrustum(inspected).LookVector() == locked.LookVector());
	CHECK(diagnostics.EffectiveFrustum(inspected).UpVector() == locked.UpVector());
	CHECK(diagnostics.FrozenFieldOfView == 1.2f);
	CHECK(diagnostics.FrozenAspectRatio == 1.5f);

	diagnostics.FrustumLocked = false;
	CHECK(diagnostics.EffectiveFrustum(inspected).Position == inspected.Position);
	CHECK(diagnostics.EffectiveFrustum(inspected).LookVector() == inspected.LookVector());
}
