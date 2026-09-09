#include "RenderFixture.hpp"

#include <engine/assets/Mesh.hpp>
#include <engine/render/PortalCaptureTreeImport.hpp>
#include <engine/render/PortalGeometry.hpp>
#include <engine/scene/SurfaceCameras.hpp>
#include <engine/testing/Suite.hpp>

TEST_SUITE_ID("engine.render.portalcapturetreevalidation")
TEST_DEPENDS("engine.render.portalcapturetreeimport")

namespace {
	using namespace engine;
	using namespace engine::render;
	const core::Name APERTURE("entrance-validation-plane");

	PortalCaptureTree EntranceTree(float extent, bool wrongClip) {
		PortalCaptureTree tree;
		for (size_t index = 0; index < 2; ++index) {
			PortalCaptureTreeNode node;
			node.Producer = {
				"validation-room-" + std::to_string(index), "portal-image-requests", 1, index + 1
			};
			node.Camera.Frustum = {-.1f, .1f, -.1f, .1f, .1f, 100};
			if (index != 0) {
				node.Camera.Projection = PortalImageProjection::Seam;
				node.Camera.ClipPlane = {0, 0, -1, wrongClip ? -3.f : -2.f + scene::PortalClipBias(2.f)};
			}
			auto &opaque = node.Layers.Opaque;
			opaque.Key = {index + 1, "door", 2, 3};
			opaque.Status = PortalImageStatus::Ok;
			opaque.Scope = PortalImageScope::OpaqueLighting;
			opaque.CaptureTick = 1;
			opaque.CaptureLighting.emplace();
			opaque.Width = opaque.Height = 8;
			opaque.RowStride = 64;
			opaque.Pixels.resize(8 * 8 * 8);
			opaque.Depth.resize(8 * 8 * 4);
			opaque.PixelHash = assets::Hasher::Of(opaque.Pixels);
			opaque.DepthHash = assets::Hasher::Of(opaque.Depth);
			tree.Nodes.push_back(std::move(node));
		}
		PortalCaptureTreeEdge edge;
		edge.PortalKey = "door";
		edge.Centre = {0, 0, -2};
		edge.First = {extent, 0, 0};
		edge.Second = {0, extent, 0};
		PortalGeometry geometry;
		auto &row = geometry.Rows.emplace_back();
		row.Assets[0] = APERTURE.Text();
		row.Pose[2] = -2;
		row.HalfExtent = {1, 1, .5f};
		std::string error;
		REQUIRE(EncodePortalGeometry(geometry, edge.Geometry, error));
		tree.Edges.push_back(std::move(edge));
		REQUIRE(ValidPortalCaptureTree(tree));
		return tree;
	}
}

TEST_CASE(
	"capture tree entrance validation rejects wrong clips before composition work",
	"[render][gpu][portal-tree-validation][.]"
) {
	test::FixtureDevice fixture;
	fixture.Initialise();
	auto &renderer = fixture.Render;
	assets::MeshData plane;
	plane.Vertices = {
		{{-1, -1, 0}, {0, 0, 1}, {0, 1}},
		{{1, -1, 0}, {0, 0, 1}, {1, 1}},
		{{1, 1, 0}, {0, 0, 1}, {1, 0}},
		{{-1, 1, 0}, {0, 0, 1}, {0, 0}}
	};
	plane.Indices = {0, 1, 2, 0, 2, 3};
	plane.ComputeBounds();
	REQUIRE(renderer.AddMesh(APERTURE, plane));
	PortalImageBinding binding;
	binding.World = 9;
	binding.WorldName = core::Name("validation-consumer");
	binding.Portal = core::Name("door");
	binding.Expected = {1, "door", 2, 3};
	binding.ExpectedScope = PortalImageScope::OpaqueLighting;
	binding.ExpectedProjection = PortalImageProjection::Eye;
	SceneTarget target{8, 8};
	View view;
	view.World = binding.World;
	view.WorldName = binding.WorldName;
	view.Target = &target;
	const auto queue = [&](PortalCaptureTree tree) {
		const auto token = renderer.QueuePortalCaptureTree(binding, std::move(tree));
		REQUIRE(token != 0);
		OverlayImage overlay;
		renderer.Render(std::span(&view, 1), overlay, nullptr, false);
		REQUIRE(renderer.PortalCaptureTreeReady(token));
		return token;
	};
	for (const float extent : {1.f, 1e30f}) {
		CAPTURE(extent);
		const auto valid = queue(EntranceTree(extent, false));
		const auto control = renderer.ComposePortalCaptureTree(valid, view);
		REQUIRE(control != 0);
		REQUIRE(renderer.DropPortalImage(control));
		renderer.DropPortalCaptureTree(valid);
	}

	for (const float extent : {1.f, 1e30f}) {
		CAPTURE(extent);
		// The wire validates this finite cross product in double. Composition
		// must not let float overflow turn a mismatched clip comparison into NaN.
		const auto token = queue(EntranceTree(extent, true));
		const auto before = renderer.PortalImageUsage();
		auto *device = static_cast<SDL_GPUDevice *>(renderer.Backend().Device);
		const auto memory = gpu::MemoryStatistics(device);
		const auto composed = renderer.ComposePortalCaptureTree(token, view);
		CHECK(composed == 0);
		CHECK(renderer.PortalImageUsage().Images == before.Images);
		CHECK(gpu::MemoryStatistics(device).TextureAllocations == memory.TextureAllocations);
		if (composed != 0) renderer.DropPortalImage(composed);
		renderer.DropPortalCaptureTree(token);
	}

	// The accepted child camera is finite, but its focal scale multiplied by
	// the seam scale exceeds float range. Refuse before rendering the child.
	auto overflow = EntranceTree(1, false);
	overflow.Edges[0].Scale = 1e38f;
	overflow.Edges[0].Centre = {0, 0, -1e-38f};
	const float entranceDistance = -overflow.Edges[0].Centre[2] * overflow.Edges[0].Scale;
	auto &child = overflow.Nodes[1];
	child.Camera.Frustum = {-.01f, .01f, -.01f, .01f, .1f, 100};
	child.Camera.ClipPlane = {0, 0, -1, -entranceDistance + scene::PortalClipBias(entranceDistance)};
	// A different extent makes rendering before validation observable even
	// when the earlier controls left reusable composition targets behind.
	auto &opaque = child.Layers.Opaque;
	opaque.Width = opaque.Height = 16;
	opaque.RowStride = opaque.Width * 8;
	opaque.Pixels.resize(size_t(opaque.RowStride) * opaque.Height);
	opaque.Depth.resize(size_t(opaque.Width) * opaque.Height * 4);
	opaque.PixelHash = assets::Hasher::Of(opaque.Pixels);
	opaque.DepthHash = assets::Hasher::Of(opaque.Depth);
	REQUIRE(ValidPortalCaptureTree(overflow));
	const auto token = queue(std::move(overflow));
	const auto before = renderer.PortalImageUsage();
	auto *device = static_cast<SDL_GPUDevice *>(renderer.Backend().Device);
	const auto memory = gpu::MemoryStatistics(device);
	const auto composed = renderer.ComposePortalCaptureTree(token, view);
	CHECK(composed == 0);
	CHECK(renderer.PortalImageUsage().Images == before.Images);
	CHECK(gpu::MemoryStatistics(device).TextureAllocations == memory.TextureAllocations);
	if (composed != 0) renderer.DropPortalImage(composed);
	renderer.DropPortalCaptureTree(token);
}
