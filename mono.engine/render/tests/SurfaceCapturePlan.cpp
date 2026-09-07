#include "SurfaceCapturePlan.hpp"

#include <engine/scene/SurfaceCameras.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <limits>

TEST_SUITE_ID("engine.render.surfacecaptureplan")

namespace {
	using namespace engine;
	using namespace engine::render;
	struct MixedCaptures {
		std::array<SurfaceView, 1> Mirrors;
		std::array<PortalView, 2> Portals;
		SurfaceCaptureRequest Request;
		MixedCaptures() {
			auto &mirror = Mirrors[0];
			mirror.Index = 0;
			mirror.PaneCentre = {0, 0, -3};
			mirror.PaneNormal = {0, 0, 1};
			mirror.PaneFirst = {2, 0, 0};
			mirror.PaneSecond = {0, 2, 0};
			mirror.Width = 16;
			mirror.Height = 8;
			auto &portal = Portals[0];
			portal.Index = 1;
			portal.Partner = 2;
			portal.Centre = {0, 0, 1};
			portal.Normal = {0, 0, -1};
			portal.First = {2, 0, 0};
			portal.Second = {0, 2, 0};
			portal.Warp.Frame =
				core::CFrame(core::Vector3(20, 0, -2)) * core::CFrame::Angles(0, 3.14159265358979323846f, 0);
			Portals[1] = portal;
			Portals[1].Index = 2;
			Portals[1].Partner = 1;
			Portals[1].Centre = {20, 0, -3};
			Portals[1].Warp.Frame = portal.Warp.Frame.Inverse();
			Request.Mirrors = Mirrors;
			Request.Portals = Portals;
			scene::Camera camera;
			camera.FieldOfViewRadians = 1.57079632679489661923f;
			Request.Projection = scene::ResolveCamera({}, camera, 2).Projection;
			Request.Width = 32;
			Request.Height = 16;
			Request.Depth = 2;
			Request.PixelBudget = 256;
		}
	};
}

TEST_CASE(
	"mixed surface cameras execute in postorder with each parent's fitted lens",
	"[render][surface-capture-plan]"
) {
	MixedCaptures scene;
	SurfaceCapturePlan plan;
	REQUIRE(PlanSurfaceCaptures(scene.Request, plan) == SurfaceCaptureStatus::Ok);
	REQUIRE(plan.Entries.size() == 2);
	REQUIRE(plan.Postorder.size() == 2);
	CHECK(plan.Postorder[0] == 1);
	CHECK(plan.Postorder[1] == 0);
	CHECK(plan.Pixels == 256);
	CHECK(plan.Roots[0] == 0);
	CHECK(plan.Roots[1] == NO_SURFACE_CAPTURE);
	CHECK(plan.Roots[2] == NO_SURFACE_CAPTURE);
	const auto &mirror = plan.Entries[0];
	const auto &portal = plan.Entries[1];
	CHECK(mirror.Children[1] == 1);
	CHECK(mirror.Arrival == 0);
	CHECK(portal.Arrival == 2);
	CHECK(portal.Children[2] == NO_SURFACE_CAPTURE);
	CHECK(mirror.Frame.Position.Z == Catch::Approx(-6));
	CHECK(portal.Frame.Position.X == Catch::Approx(20));
	CHECK(portal.Frame.Position.Z == Catch::Approx(4));
	CHECK(portal.Width == 16);
	CHECK(portal.Height == 8);
	CHECK(portal.Matrices.Projection[0][0] == Catch::Approx(mirror.Matrices.Projection[0][0]));
	CHECK(portal.Matrices.Projection[1][1] == Catch::Approx(mirror.Matrices.Projection[1][1]));
	const auto destinationPoint = portal.Matrices.ViewProjection * glm::vec4(20, 0, -6, 1);
	CHECK(destinationPoint.w > 0);
	CHECK(destinationPoint.z >= 0);
	CHECK(destinationPoint.z <= destinationPoint.w);
	const auto *allocation = plan.Entries.data();
	REQUIRE(PlanSurfaceCaptures(scene.Request, plan) == SurfaceCaptureStatus::Ok);
	CHECK(plan.Entries.data() == allocation);
}

TEST_CASE(
	"surface planning refuses exhausted budgets before leaving partial captures",
	"[render][surface-capture-plan]"
) {
	MixedCaptures scene;
	SurfaceCapturePlan plan;
	SECTION("one pixel short") {
		scene.Request.PixelBudget--;
	}
	SECTION("zero recursion with a visible mirror") {
		scene.Request.Depth = 0;
	}
	REQUIRE(PlanSurfaceCaptures(scene.Request, plan) == SurfaceCaptureStatus::BudgetExceeded);
	CHECK(plan.Entries.empty());
	CHECK(plan.Postorder.empty());
	CHECK(plan.Pixels == 0);
	CHECK(plan.Roots[0] == NO_SURFACE_CAPTURE);
}

TEST_CASE(
	"a requested recursion bound permits an explicit terminal surface", "[render][surface-capture-plan]"
) {
	MixedCaptures scene;
	scene.Request.Depth = 1;
	scene.Request.PixelBudget = 128;
	SurfaceCapturePlan plan;
	REQUIRE(PlanSurfaceCaptures(scene.Request, plan) == SurfaceCaptureStatus::Ok);
	REQUIRE(plan.Entries.size() == 1);
	CHECK(plan.Entries[0].Children[1] == NO_SURFACE_CAPTURE);
	CHECK(plan.Pixels == 128);
}

TEST_CASE(
	"surface plans reject ambiguous slots and nonfinite projection inputs", "[render][surface-capture-plan]"
) {
	MixedCaptures scene;
	SECTION("a mirror and portal cannot share an input slot") {
		scene.Portals[0].Index = 0;
	}
	SECTION("invalid projection") {
		scene.Request.Projection[0][0] = std::numeric_limits<float>::infinity();
	}
	SurfaceCapturePlan plan;
	CHECK(PlanSurfaceCaptures(scene.Request, plan) == SurfaceCaptureStatus::Invalid);
	CHECK(plan.Entries.empty());
}

TEST_CASE(
	"child mirror resolution fits the parent capture without changing its authored aspect",
	"[render][surface-capture-plan]"
) {
	MixedCaptures scene;
	scene.Mirrors[0].Width = 512;
	scene.Mirrors[0].Height = 512;
	scene.Request.PixelBudget = 512;
	SurfaceCapturePlan plan;
	REQUIRE(PlanSurfaceCaptures(scene.Request, plan) == SurfaceCaptureStatus::Ok);
	REQUIRE(plan.Entries.size() == 2);
	CHECK(plan.Entries[0].Width == 16);
	CHECK(plan.Entries[0].Height == 16);
	CHECK(plan.Entries[1].Width == 16);
	CHECK(plan.Entries[1].Height == 16);
	CHECK(plan.Pixels == 512);
	CHECK(scene.Mirrors[0].Width == 512);
	CHECK(scene.Mirrors[0].Height == 512);
}

TEST_CASE(
	"child transparency orders existing packed slots from its own eye", "[render][surface-capture-plan]"
) {
	std::array<scene::DrawInstance, 4> instances;
	instances[1].Frame.Position = {20, 0, -4};
	instances[2].Frame.Position = {20, 0, -6};
	instances[3].Frame.Position = instances[1].Frame.Position;
	const std::array<uint32_t, 4> packed{0, 3, 1, 2};
	std::vector<CaptureBlendSlot> order;
	REQUIRE(OrderCaptureTransparency(instances, packed, 1, 3, {0, 0, -6}, order));
	REQUIRE(order.size() == 3);
	CHECK(order[0].Slot == 2);
	CHECK(order[1].Slot == 1);
	CHECK(order[2].Slot == 3);
	const auto *storage = order.data();
	REQUIRE(OrderCaptureTransparency(instances, packed, 1, 3, {20, 0, 4}, order));
	CHECK(order[0].Slot == 3);
	CHECK(order[1].Slot == 2);
	CHECK(order[2].Slot == 1);
	CHECK(order.data() == storage);
	CHECK_FALSE(OrderCaptureTransparency(instances, packed, UINT32_MAX, 3, {}, order));
	CHECK(order.empty());
	const std::array<uint32_t, 2> invalid{1, 99};
	CHECK_FALSE(OrderCaptureTransparency(instances, invalid, 0, 2, {}, order));
	CHECK(order.empty());
}

TEST_CASE(
	"small portal scales keep the destination clip in front of a near-plane eye",
	"[render][surface-capture-plan]"
) {
	PortalView portal;
	portal.Index = 0;
	portal.Partner = 1;
	portal.Centre = {};
	portal.Normal = {0, 0, 1};
	portal.First = {1, 0, 0};
	portal.Second = {0, 1, 0};
	portal.Warp.Frame = core::CFrame({20, 0, 0}) * core::CFrame::Angles(0, 3.14159265358979323846f, 0);
	portal.Warp.Scale = .5f;
	SurfaceCaptureRequest request;
	request.Portals = std::span(&portal, 1);
	request.Frame.Position = {0, 0, .002f};
	SECTION("scaled destination bias") {}
	SECTION("submillimetre destination separation") {
		request.Frame.Position.Z = .0002f;
	}
	scene::Camera camera;
	camera.NearPlane = .00001f;
	request.Projection = scene::ResolveCamera(request.Frame, camera, 1).Projection;
	request.Width = request.Height = 32;
	request.Depth = 1;
	request.PixelBudget = 1024;
	SurfaceCapturePlan plan;
	REQUIRE(PlanSurfaceCaptures(request, plan) == SurfaceCaptureStatus::Ok);
	REQUIRE(plan.Entries.size() == 1);
	const auto &child = plan.Entries.front();
	const auto destination = portal.Warp.Point(portal.Centre);
	const auto outward = portal.Warp.Rotate(portal.Normal) * -1;
	const auto justBeyond = destination + outward * .00001f;
	const auto clip = child.Matrices.ViewProjection * glm::vec4(justBeyond.X, justBeyond.Y, justBeyond.Z, 1);
	CHECK(clip.w > 0);
	CHECK(clip.z >= 0);
	CHECK(clip.z <= clip.w);
	const float mappedDistance = portal.Warp.Length(request.Frame.Position.Z);
	const auto beforeClip = destination - outward * (mappedDistance * .75f);
	const auto excluded =
		child.Matrices.ViewProjection * glm::vec4(beforeClip.X, beforeClip.Y, beforeClip.Z, 1);
	CHECK(excluded.w > 0);
	CHECK(excluded.z < 0);
}
