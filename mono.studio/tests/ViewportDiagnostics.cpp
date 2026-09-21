#include <engine/core/types/CFrame.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/testing/Suite.hpp>
#include <studio/ViewportDiagnostics.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("studio.viewport_diagnostics")

TEST_CASE("a frozen viewport frustum stays separate from the inspection pose", "[studio][viewport]") {
	studio::ViewportDiagnostics diagnostics;
	const engine::core::CFrame locked(engine::core::Vector3{2.0f, 3.0f, 4.0f});
	const engine::core::CFrame inspected(engine::core::Vector3{9.0f, 8.0f, 7.0f});

	diagnostics.LockFrustum(locked);
	CHECK(diagnostics.FrustumLocked);
	CHECK(diagnostics.EffectiveFrustum(inspected).Position == locked.Position);
	CHECK(diagnostics.EffectiveFrustum(inspected).LookVector() == locked.LookVector());

	diagnostics.FrustumLocked = false;
	CHECK(diagnostics.EffectiveFrustum(inspected).Position == inspected.Position);
	CHECK(diagnostics.EffectiveFrustum(inspected).LookVector() == inspected.LookVector());
}
