#include "SurfaceCapturePlan.hpp"

#include "SurfaceCaptureCache.hpp"

#include <engine/scene/SurfaceCameras.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
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

	std::vector<SurfaceView> MirrorBallViews() {
		const float phi = (1.0f + std::sqrt(5.0f)) * .5f;
		const std::array<core::Vector3, 12> vertices{
			core::Vector3{-1, phi, 0},
			{1, phi, 0},
			{-1, -phi, 0},
			{1, -phi, 0},
			{0, -1, phi},
			{0, 1, phi},
			{0, -1, -phi},
			{0, 1, -phi},
			{phi, 0, -1},
			{phi, 0, 1},
			{-phi, 0, -1},
			{-phi, 0, 1}
		};
		const std::array<std::array<size_t, 3>, 20> indices{
			{{0, 11, 5},  {0, 5, 1},  {0, 1, 7},  {0, 7, 10}, {0, 10, 11}, {1, 5, 9}, {5, 11, 4},
			 {11, 10, 2}, {10, 7, 6}, {7, 1, 8},  {3, 9, 4},  {3, 4, 2},   {3, 2, 6}, {3, 6, 8},
			 {3, 8, 9},	  {4, 9, 5},  {2, 4, 11}, {6, 2, 10}, {8, 6, 7},   {9, 8, 1}}
		};
		std::vector<std::array<core::Vector3, 3>> faces;
		for (const auto &face : indices) {
			faces.push_back({vertices[face[0]], vertices[face[1]], vertices[face[2]]});
		}
		for (size_t subdivision = 0; subdivision < 2; ++subdivision) {
			std::vector<std::array<core::Vector3, 3>> split;
			for (const auto &face : faces) {
				const auto ab = (face[0] + face[1]) * .5f;
				const auto bc = (face[1] + face[2]) * .5f;
				const auto ca = (face[2] + face[0]) * .5f;
				split.push_back({face[0], ab, ca});
				split.push_back({face[1], bc, ab});
				split.push_back({face[2], ca, bc});
				split.push_back({ab, bc, ca});
			}
			faces = std::move(split);
		}
		std::vector<SurfaceView> mirrors;
		for (auto face : faces) {
			for (auto &vertex : face)
				vertex = vertex.Unit() * 12.0f;
			const auto centre = (face[0] + face[1] + face[2]) / 3.0f;
			auto normal = (face[1] - face[0]).Cross(face[2] - face[0]).Unit();
			if (normal.Dot(centre) < 0) normal = -normal;
			const auto first = (face[1] - face[0]).Unit();
			const auto second = normal.Cross(first);
			float leastX = std::numeric_limits<float>::max(), mostX = -leastX;
			float leastY = leastX, mostY = -leastY;
			for (const auto &vertex : face) {
				const auto offset = vertex - centre;
				leastX = std::min(leastX, offset.Dot(first));
				mostX = std::max(mostX, offset.Dot(first));
				leastY = std::min(leastY, offset.Dot(second));
				mostY = std::max(mostY, offset.Dot(second));
			}
			SurfaceView mirror;
			mirror.Index = static_cast<int16_t>(mirrors.size());
			mirror.PaneCentre = centre + first * ((leastX + mostX) * .5f) +
								second * ((leastY + mostY) * .5f) + core::Vector3{0, 22, 0};
			mirror.PaneNormal = normal;
			mirror.PaneFirst = first * ((mostX - leastX) * .5f);
			mirror.PaneSecond = second * ((mostY - leastY) * .5f);
			mirror.Width = 4;
			mirror.Height = 2;
			mirrors.push_back(mirror);
		}
		return mirrors;
	}
}

TEST_CASE("surface capture cache reuses only an unchanged retained scene", "[render][surface-capture-plan]") {
	using namespace engine::render;
	MixedCaptures scene;
	SurfaceCaptureCache cache;
	const uint64_t signature = SurfaceCaptureSignature(scene.Request);
	CHECK(cache.NeedsRefresh(signature));

	cache.Commit(signature, 512, false);
	CHECK_FALSE(cache.NeedsRefresh(signature));
	CHECK(cache.Pixels == 512);
	CHECK_FALSE(cache.BudgetExceeded);

	scene.Request.Frame.Position.X = 1.0f;
	CHECK(cache.NeedsRefresh(SurfaceCaptureSignature(scene.Request)));
	scene.Request.Frame.Position.X = 0.0f;
	scene.Request.Width++;
	CHECK(cache.NeedsRefresh(SurfaceCaptureSignature(scene.Request)));
	scene.Request.Width--;
	scene.Portals[0].Centre.Z += 1.0f;
	CHECK(cache.NeedsRefresh(SurfaceCaptureSignature(scene.Request)));
	const uint64_t portalSignature = SurfaceCaptureSignature(scene.Request);
	scene.Request.MirrorDepth = 1;
	CHECK(SurfaceCaptureSignature(scene.Request) != portalSignature);

	const uint64_t changedSignature = SurfaceCaptureSignature(scene.Request);
	cache.Commit(changedSignature, 0, true);
	CHECK_FALSE(cache.NeedsRefresh(changedSignature));
	CHECK(cache.BudgetExceeded);
}

TEST_CASE(
	"surface capture signatures retain wide portal partners", "[render][surface-capture-plan][surface-wide]"
) {
	MixedCaptures scene;
	REQUIRE(sizeof(scene.Portals[0].Partner) >= sizeof(int16_t));
	scene.Portals[0].Partner = 0;
	const auto zero = SurfaceCaptureSignature(scene.Request);
	scene.Portals[0].Partner = static_cast<decltype(scene.Portals[0].Partner)>(256);
	CHECK(SurfaceCaptureSignature(scene.Request) != zero);
	scene.Portals[0].Partner = static_cast<decltype(scene.Portals[0].Partner)>(255);
	const auto positive = SurfaceCaptureSignature(scene.Request);
	scene.Portals[0].Partner = -1;
	CHECK(SurfaceCaptureSignature(scene.Request) != positive);
}

TEST_CASE("surface captures preserve all 320 root slots", "[render][surface-capture-plan][surface-wide]") {
	constexpr size_t FACETS = 320;
	REQUIRE(engine::scene::MAX_SURFACES >= FACETS);
	MixedCaptures scene;
	std::vector<SurfaceView> mirrors(FACETS, scene.Mirrors[0]);
	for (size_t index = 0; index < mirrors.size(); ++index) {
		mirrors[index].Index = static_cast<int16_t>(index);
		mirrors[index].Width = 4;
		mirrors[index].Height = 2;
	}
	scene.Request.Mirrors = mirrors;
	scene.Request.Portals = {};
	scene.Request.Depth = 1;
	scene.Request.PixelBudget = FACETS * 8;
	SurfaceCapturePlan plan;
	REQUIRE(PlanSurfaceCaptures(scene.Request, plan) == SurfaceCaptureStatus::Ok);
	CHECK(plan.Entries.size() == FACETS);
	CHECK(plan.Pixels == scene.Request.PixelBudget);
	for (size_t index = 0; index < FACETS; ++index) {
		INFO("surface " << index);
		REQUIRE(plan.Roots[index] != NO_SURFACE_CAPTURE);
		const auto &entry = plan.Entries[plan.Roots[index]];
		CHECK(entry.Slot == static_cast<int16_t>(index));
		CHECK(entry.Arrival == static_cast<int16_t>(index));
		CHECK(entry.RootSlot == static_cast<int16_t>(index));
	}
}

TEST_CASE(
	"a 320-facet mirror ball honors one mirror bounce", "[render][surface-capture-plan][surface-wide]"
) {
	const auto mirrors = MirrorBallViews();
	REQUIRE(mirrors.size() == 320);
	MixedCaptures scene;
	scene.Request.Mirrors = mirrors;
	scene.Request.Portals = {};
	scene.Request.Frame = core::CFrame::LookAt({0, 26, 46}, {0, 22, 0});
	scene.Request.PixelBudget = UINT64_MAX;
	SurfaceCapturePlan plan;
	REQUIRE(PlanSurfaceCaptures(scene.Request, plan) == SurfaceCaptureStatus::BudgetExceeded);
	CHECK(plan.Entries.empty());
	scene.Request.MirrorDepth = 1;
	REQUIRE(PlanSurfaceCaptures(scene.Request, plan) == SurfaceCaptureStatus::Ok);
	REQUIRE(plan.Entries.size() > 100);
	for (const auto &entry : plan.Entries) {
		CHECK(entry.Depth == 1);
		CHECK(plan.Roots[entry.Slot] != NO_SURFACE_CAPTURE);
	}
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

TEST_CASE("mirror depth counts mirrors along mixed capture paths", "[render][surface-capture-plan]") {
	MixedCaptures scene;
	scene.Request.MirrorDepth = 1;
	SurfaceCapturePlan plan;
	SECTION("a terminal mirror can still show a portal") {
		REQUIRE(PlanSurfaceCaptures(scene.Request, plan) == SurfaceCaptureStatus::Ok);
		REQUIRE(plan.Entries.size() == 2);
		CHECK(plan.Entries[0].Kind == SurfaceCaptureKind::Mirror);
		CHECK(plan.Entries[1].Kind == SurfaceCaptureKind::Portal);
		CHECK(plan.Entries[0].Children[1] == 1);
	}
	SECTION("a terminal portal can still show one mirror") {
		scene.Mirrors[0].PaneCentre = {20, 0, -3};
		scene.Portals[0].Centre = {0, 0, -1};
		scene.Portals[0].Normal = {0, 0, 1};
		scene.Portals[0].Warp.Frame = core::CFrame(core::Vector3(20, 0, 0));
		scene.Request.Portals = std::span(scene.Portals).first(1);
		scene.Request.PixelBudget = 32 * 16 + 16 * 8;
		REQUIRE(PlanSurfaceCaptures(scene.Request, plan) == SurfaceCaptureStatus::Ok);
		REQUIRE(plan.Entries.size() == 2);
		CHECK(plan.Pixels == scene.Request.PixelBudget);
		CHECK(plan.Entries[0].Kind == SurfaceCaptureKind::Portal);
		CHECK(plan.Entries[1].Kind == SurfaceCaptureKind::Mirror);
		CHECK(plan.Entries[0].Children[0] == 1);
	}
	SECTION("one mirror cannot recurse into another mirror") {
		std::array mirrors{scene.Mirrors[0], scene.Mirrors[0]};
		mirrors[1].Index = 1;
		mirrors[1].PaneCentre = {0, 0, 1};
		mirrors[1].PaneNormal = {0, 0, -1};
		scene.Request.Mirrors = mirrors;
		scene.Request.Portals = {};
		REQUIRE(PlanSurfaceCaptures(scene.Request, plan) == SurfaceCaptureStatus::Ok);
		REQUIRE(plan.Entries.size() == 1);
		CHECK(plan.Entries[0].Children[1] == NO_SURFACE_CAPTURE);
		scene.Request.MirrorDepth = 2;
		REQUIRE(PlanSurfaceCaptures(scene.Request, plan) == SurfaceCaptureStatus::Ok);
		REQUIRE(plan.Entries.size() == 2);
		CHECK(plan.Entries[0].Children[1] == 1);
	}
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
