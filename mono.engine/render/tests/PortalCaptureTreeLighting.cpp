#include "RenderFixture.hpp"

#include <engine/assets/Mesh.hpp>
#include <engine/assets/Texture.hpp>
#include <engine/core/Bytes.hpp>
#include <engine/graph/PipelineDocument.hpp>
#include <engine/graph/Shadow.hpp>
#include <engine/render/PortalCaptureTreeCompose.hpp>
#include <engine/render/PortalCaptureTreeImport.hpp>
#include <engine/render/PortalGeometry.hpp>
#include <engine/render/PortalImageRuntime.hpp>
#include <engine/render/PortalShadowImageImport.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/generators/catch_generators.hpp>
#include <glm/packing.hpp>

TEST_SUITE_ID("engine.render.portalcapturetreelighting")
TEST_DEPENDS("engine.render.portalcapturetreeimport")

namespace {
	using namespace engine;
	using namespace engine::render;
	constexpr uint32_t EXTENT = 65;
	const core::Name MESH("lighting-oracle-plane");
	const core::Name GLASS_ROOM_PIPELINE("lighting-glass-room-capture");
	const core::Name NATIVE_PIPELINE("lighting-native-reference");
	const core::Name MATERIAL_AO("lighting-material-ao");
	const core::Name EMISSION("lighting-emission");
	enum class LightingScenario {
		Original,
		Contact,
		FogMaterial,
		BrightAmbient,
		GlassBeforeBody,
		GlassBehindBody,
		Shadows,
		FirstPersonRig,
		FirstPersonRows
	};
	const core::Name NO_AO_PIPELINE("lighting-native-no-ao-control");

	scene::DrawInstance Panel(
		uint64_t source, float x, float depth, float width, float height, core::Color3 colour, float y = 0
	) {
		scene::DrawInstance row;
		row.Source = source;
		row.Frame.Position = {x, y, -depth};
		row.HalfExtent = {width, height, .001f};
		row.Mesh = MESH;
		row.CastShadow = false;
		row.Tint = colour;
		row.EmissiveStrength = 0;
		return row;
	}
	std::vector<scene::DrawInstance> Frame(uint64_t source, float depth, float half, core::Color3 colour) {
		return {
			Panel(source, -50 - half, depth, 50, 100, colour),
			Panel(source + 1, 50 + half, depth, 50, 100, colour),
			Panel(source + 2, 0, depth, half, 50, colour, -50 - half),
			Panel(source + 3, 0, depth, half, 50, colour, 50 + half)
		};
	}
	scene::DrawInstance Scaled(scene::DrawInstance row, float scale) {
		row.Frame.Position = row.Frame.Position * scale;
		row.HalfExtent = row.HalfExtent * scale;
		return row;
	}

	void InstallNative(Renderer &renderer, bool glass) {
		using namespace graph;
		PipelineDocument document;
		for (const char *name : {"lighting-baseline", "directional-response"})
			document.Record(
				{.Kind = EditKind::AddResource,
				 .Name = core::Name(name),
				 .Resource = ResourceKind::Colour,
				 .Format = ResourceFormat::RGBA32F}
			);
		const auto native = DefaultPbrDocument();
		for (const auto &edit : native.Edits()) {
			if (edit.Kind == EditKind::AddNode && edit.Name == core::Name("present")) {
				document.Record(
					{.Kind = EditKind::AddNode,
					 .Name = core::Name("source-shadow-export"),
					 .NodeKind = core::Name("shadow-capture"),
					 .Scope = NodeScope::View}
				);
				document.Record(
					{.Kind = EditKind::Reads, .Target = core::Name("shadow"), .Key = core::Name("shadow")}
				);
				document.Record(
					{.Kind = EditKind::AddResource,
					 .Name = core::Name("oracle-depth"),
					 .Resource = ResourceKind::Colour,
					 .Format = ResourceFormat::R32F}
				);
				document.Record(
					{.Kind = EditKind::AddNode,
					 .Name = core::Name("oracle-depth-linearise"),
					 .NodeKind = core::Name("depth-linearise"),
					 .Scope = NodeScope::View}
				);
				document.Record(
					{.Kind = EditKind::Reads, .Target = core::Name("depth"), .Key = core::Name("depth")}
				);
				document.Record(
					{.Kind = EditKind::Writes,
					 .Target = core::Name("oracle-depth"),
					 .Key = core::Name("linear")}
				);
				document.Record({.Kind = EditKind::Set, .Key = core::Name("background"), .Value = "zero"});
				document.Record(
					{.Kind = EditKind::AddResource,
					 .Name = core::Name("ambient-response"),
					 .Resource = ResourceKind::Colour,
					 .Format = ResourceFormat::RGBA32F}
				);
				document.Record(
					{.Kind = EditKind::AddNode,
					 .Name = core::Name("ambient-response"),
					 .NodeKind = core::Name("ambient-response"),
					 .Scope = NodeScope::View}
				);
				for (const auto &[resource, port] : std::array{
						 std::pair{"albedo", "albedo"},
						 std::pair{"normal", "normal"},
						 std::pair{"material", "material"},
						 std::pair{"linear-depth", "depth"},
						 std::pair{"occlusion", "occlusion"}
					 })
					document.Record(
						{.Kind = EditKind::Reads, .Target = core::Name(resource), .Key = core::Name(port)}
					);
				document.Record(
					{.Kind = EditKind::Writes,
					 .Target = core::Name("ambient-response"),
					 .Key = core::Name("response")}
				);
			}
			if (glass && edit.Kind == EditKind::AddResource && edit.Name == core::Name("depth")) {
				auto depth = edit;
				depth.Format = ResourceFormat::D32F;
				document.Record(depth);
			} else
				document.Record(edit);
			if (edit.Kind == EditKind::Writes && edit.Target == core::Name("lit"))
				for (const char *name : {"lighting-baseline", "directional-response"})
					document.Record(
						{.Kind = EditKind::Writes, .Target = core::Name(name), .Key = core::Name(name)}
					);
		}
		for (const bool room : {false, true}) {
			document.Record(
				{.Kind = EditKind::AddNode,
				 .Name = core::Name(room ? "room-export" : "native-export"),
				 .NodeKind = core::Name("capture"),
				 .Scope = NodeScope::Frame}
			);
			document.Record(
				{.Kind = EditKind::Reads,
				 .Target = core::Name(room ? "lit" : "lens-b"),
				 .Key = core::Name("source")}
			);
			document.Record(
				{.Kind = EditKind::Reads, .Target = core::Name("oracle-depth"), .Key = core::Name("depth")}
			);
			if (room)
				for (const char *name :
					 {"normal", "ambient-response", "lighting-baseline", "directional-response"})
					document.Record(
						{.Kind = EditKind::Reads, .Target = core::Name(name), .Key = core::Name(name)}
					);
		}
		RenderGraph pipeline;
		core::Name offender;
		REQUIRE(Build(document, pipeline, offender) == PipelineDocumentStatus::Ok);
		REQUIRE(renderer.SetPipeline(NATIVE_PIPELINE, pipeline));
		document.Record({.Kind = EditKind::Enable, .Name = core::Name("ssao"), .Enabled = false});
		RenderGraph control;
		REQUIRE(Build(document, control, offender) == PipelineDocumentStatus::Ok);
		REQUIRE(renderer.SetPipeline(NO_AO_PIPELINE, control));
		if (glass) {
			PipelineDocument glassView;
			for (const auto &[name, format] : std::array{
					 std::pair{"glass-colour", ResourceFormat::RGBA16F},
					 std::pair{"glass-depth", ResourceFormat::R32F},
					 std::pair{"glass-z", ResourceFormat::D32F}
				 })
				glassView.Record(
					{.Kind = EditKind::AddResource,
					 .Name = core::Name(name),
					 .Resource = format == ResourceFormat::D32F ? ResourceKind::Depth : ResourceKind::Colour,
					 .Format = format}
				);
			glassView.Record(
				{.Kind = EditKind::AddNode,
				 .Name = core::Name("glass-layer"),
				 .NodeKind = core::Name("transparent-layer"),
				 .Scope = NodeScope::View}
			);
			for (const auto &[resource, port] : std::array{
					 std::pair{"depth", "opaque-z"},
					 std::pair{"ordered-entities", "entities"},
					 std::pair{"view-instances", "instances"},
					 std::pair{"shadow", "shadow"}
				 })
				glassView.Record(
					{.Kind = EditKind::Reads, .Target = core::Name(resource), .Key = core::Name(port)}
				);
			for (const auto &[resource, port] : std::array{
					 std::pair{"glass-colour", "colour"},
					 std::pair{"glass-depth", "depth"},
					 std::pair{"glass-z", "z"}
				 })
				glassView.Record(
					{.Kind = EditKind::Writes, .Target = core::Name(resource), .Key = core::Name(port)}
				);
			PipelineDocument roomDocument;
			bool inserted = false;
			for (const auto &edit : document.Edits()) {
				if (!inserted && edit.Kind == EditKind::AddNode && edit.Scope == NodeScope::Frame) {
					for (const auto &viewEdit : glassView.Edits())
						roomDocument.Record(viewEdit);
					inserted = true;
				}
				roomDocument.Record(edit);
			}
			REQUIRE(inserted);
			roomDocument.Record(
				{.Kind = EditKind::AddNode,
				 .Name = core::Name("glass-export"),
				 .NodeKind = core::Name("capture"),
				 .Scope = NodeScope::Frame}
			);
			roomDocument.Record(
				{.Kind = EditKind::Reads, .Target = core::Name("glass-colour"), .Key = core::Name("source")}
			);
			roomDocument.Record(
				{.Kind = EditKind::Reads, .Target = core::Name("glass-depth"), .Key = core::Name("depth")}
			);
			roomDocument.Record({.Kind = EditKind::Enable, .Name = core::Name("ssao"), .Enabled = true});
			RenderGraph room;
			REQUIRE(Build(roomDocument, room, offender) == PipelineDocumentStatus::Ok);
			REQUIRE(renderer.SetPipeline(GLASS_ROOM_PIPELINE, room));
		}
		assets::MeshData plane;
		plane.Vertices = {
			{{-1, -1, 0}, {0, 0, 1}, {0, 1}},
			{{1, -1, 0}, {0, 0, 1}, {1, 1}},
			{{1, 1, 0}, {0, 0, 1}, {1, 0}},
			{{-1, 1, 0}, {0, 0, 1}, {0, 0}}
		};
		plane.Indices = {0, 1, 2, 0, 2, 3};
		plane.ComputeBounds();
		REQUIRE(renderer.AddMesh(MESH, plane));
		assets::TextureData texture;
		texture.Width = texture.Height = 1;
		texture.Format = assets::TextureFormat::RGBA8;
		texture.Pixels.assign(4, std::byte{255});
		REQUIRE(renderer.AddTexture(EMISSION, texture));
		texture.Pixels[0] = std::byte{128};
		REQUIRE(renderer.AddTexture(MATERIAL_AO, texture));
	}
	std::vector<std::byte> ReadComposed(Renderer &renderer, const char *resource, uint32_t pixelBytes);
	struct NativeImage {
		ResourceImage Export;
		std::optional<ResourceImage> Glass;
	};
	NativeImage NativeCapture(
		Renderer &renderer,
		View view,
		std::span<const scene::DrawInstance> rows,
		bool room = false,
		bool noAo = false,
		bool glass = false
	) {
		view.Pipeline = glass ? GLASS_ROOM_PIPELINE : noAo ? NO_AO_PIPELINE : NATIVE_PIPELINE;
		view.Instances = rows;
		const core::Name node(room ? "room-export" : "native-export");
		const std::array nodes{node, core::Name("glass-export")};
		std::array<uint64_t, 2> tokens{};
		const size_t count = glass ? 2 : 1;
		REQUIRE(renderer.QueueResourceImages(
			view.Pipeline,
			std::span(nodes).first(count),
			view.Slot,
			ResourceImageDelivery::CopiedPixels,
			std::span(tokens).first(count)
		));
		OverlayImage overlay;
		REQUIRE(renderer.Render(std::span(&view, 1), overlay, nullptr, false).Ran(node));
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while (std::chrono::steady_clock::now() < deadline) {
			if (auto result = renderer.TakeResourceImages(std::span(tokens).first(count))) {
				for (const auto &image : *result) {
					REQUIRE(image.Status == ResourceImageStatus::Ok);
					REQUIRE(image.CaptureFrame == result->front().CaptureFrame);
				}
				NativeImage image{std::move(result->front()), {}};
				if (glass) image.Glass = std::move(result->back());
				return image;
			}
			SDL_Delay(1);
		}
		FAIL("native room capture did not complete");
		return {};
	}
	PortalShadowImage SourceShadow(
		Renderer &renderer,
		View view,
		std::span<const scene::DrawInstance> rows,
		const core::AABB &domain,
		PortalShadowSnapshot identity
	) {
		view.Pipeline = NATIVE_PIPELINE;
		view.Instances = rows;
		view.DirectionalShadowBounds = domain;
		const core::Name node("source-shadow-export");
		const auto token = renderer.QueueResourceImage(view.Pipeline, node, view.Slot);
		REQUIRE(token != 0);
		OverlayImage overlay;
		REQUIRE(renderer.Render(std::span(&view, 1), overlay, nullptr, false).Ran(node));
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while (std::chrono::steady_clock::now() < deadline) {
			if (auto captured = renderer.TakeResourceImage(token)) {
				REQUIRE(captured->Status == ResourceImageStatus::Ok);
				REQUIRE(captured->Shadow);
				const auto bounds = [](const core::AABB &box) {
					return std::array{
						box.Minimum.X,
						box.Minimum.Y,
						box.Minimum.Z,
						box.Maximum.X,
						box.Maximum.Y,
						box.Maximum.Z
					};
				};
				identity.SourceEmpty = captured->Shadow->SourceEmpty;
				identity.SourceBounds = bounds(captured->Shadow->SourceBounds);
				identity.DomainBounds = bounds(captured->Shadow->DomainBounds);
				identity.LightViewProjection = captured->Shadow->LightViewProjection;
				identity.DepthHash = assets::Hasher::Of(captured->Depth);
				PortalShadowImage image{std::move(identity), std::move(captured->Depth)};
				REQUIRE(ValidPortalShadowImage(image));
				return image;
			}
			SDL_Delay(1);
		}
		FAIL("source shadow capture did not complete");
		return {};
	}
	std::vector<float> Colour(std::span<const std::byte> bytes) {
		std::vector<float> colour;
		core::ByteReader reader(bytes);
		while (!reader.AtEnd()) {
			const auto pair = glm::unpackHalf2x16(reader.ReadUInt32());
			colour.push_back(pair.x);
			colour.push_back(pair.y);
		}
		return colour;
	}
	std::vector<std::byte> ReadComposed(Renderer &renderer, const char *resource, uint32_t pixelBytes) {
		auto *device = static_cast<SDL_GPUDevice *>(renderer.Backend().Device);
		auto *texture = static_cast<SDL_GPUTexture *>(renderer.ResourceTexture(core::Name(resource), 0));
		REQUIRE(texture);
		const uint32_t stride = (EXTENT * pixelBytes + 255) / 256 * 256;
		SDL_GPUTransferBufferCreateInfo info{};
		info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
		info.size = stride * EXTENT;
		const auto releaseTransfer = [device](SDL_GPUTransferBuffer *transfer) {
			gpu::ReleaseTransferBuffer(device, transfer);
		};
		std::unique_ptr<SDL_GPUTransferBuffer, decltype(releaseTransfer)> transfer(
			gpu::CreateTransferBuffer(device, &info), releaseTransfer
		);
		REQUIRE(transfer);
		const auto cancel = [](SDL_GPUCommandBuffer *command) { SDL_CancelGPUCommandBuffer(command); };
		std::unique_ptr<SDL_GPUCommandBuffer, decltype(cancel)> command(
			SDL_AcquireGPUCommandBuffer(device), cancel
		);
		REQUIRE(command);
		auto *copy = SDL_BeginGPUCopyPass(command.get());
		REQUIRE(copy);
		SDL_GPUTextureRegion from{};
		from.texture = texture;
		from.w = from.h = EXTENT;
		from.d = 1;
		SDL_GPUTextureTransferInfo to{};
		to.transfer_buffer = transfer.get();
		to.pixels_per_row = stride / pixelBytes;
		to.rows_per_layer = EXTENT;
		SDL_DownloadFromGPUTexture(copy, &from, &to);
		SDL_EndGPUCopyPass(copy);
		const auto releaseFence = [device](SDL_GPUFence *fence) { SDL_ReleaseGPUFence(device, fence); };
		std::unique_ptr<SDL_GPUFence, decltype(releaseFence)> fence(
			SDL_SubmitGPUCommandBufferAndAcquireFence(command.release()), releaseFence
		);
		REQUIRE(fence);
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while (!SDL_QueryGPUFence(device, fence.get()) && std::chrono::steady_clock::now() < deadline)
			SDL_Delay(1);
		REQUIRE(SDL_QueryGPUFence(device, fence.get()));
		const auto *mapped =
			static_cast<const std::byte *>(SDL_MapGPUTransferBuffer(device, transfer.get(), false));
		REQUIRE(mapped);
		std::vector<std::byte> result(size_t(EXTENT) * EXTENT * pixelBytes);
		for (size_t y = 0; y < EXTENT; ++y)
			std::memcpy(result.data() + y * EXTENT * pixelBytes, mapped + y * stride, EXTENT * pixelBytes);
		SDL_UnmapGPUTransferBuffer(device, transfer.get());
		return result;
	}
	PortalCaptureTreeEdge Edge(uint8_t parent, float mouthDepth, float half, float scale) {
		PortalCaptureTreeEdge edge;
		edge.Parent = parent;
		edge.Child = parent + 1;
		edge.PortalKey = "mouth-" + std::to_string(parent);
		edge.Centre = {0, 0, -mouthDepth};
		edge.First = {half, 0, 0};
		edge.Second = {0, half, 0};
		edge.Scale = scale;
		PortalGeometry geometry;
		PortalGeometryRow row;
		row.Assets[0] = std::string(MESH.Text());
		row.Pose[2] = -mouthDepth;
		row.HalfExtent = {half, half, .001f};
		row.CastShadow = false;
		geometry.Rows.push_back(row);
		std::string error;
		REQUIRE(EncodePortalGeometry(geometry, edge.Geometry, error));
		return edge;
	}
}

TEST_CASE(
	"nested current body uses each retained room's scalar and local lighting",
	"[render][gpu][portal-tree-lighting][.]"
) {
	// The native room and body share SSAO before child apertures enter composition.
	// Retained room captures must preserve this interaction as lighting changes.
	test::FixtureDevice fixture;
	fixture.Initialise();
	auto &renderer = fixture.Render;
	const SceneTarget target{EXTENT, EXTENT};
	View base;
	base.World = 17;
	base.WorldName = core::Name("lighting-consumer");
	base.Target = &target;
	const auto scenario = GENERATE(
		LightingScenario::Original,
		LightingScenario::Contact,
		LightingScenario::FogMaterial,
		LightingScenario::BrightAmbient,
		LightingScenario::GlassBeforeBody,
		LightingScenario::GlassBehindBody,
		LightingScenario::Shadows,
		LightingScenario::FirstPersonRig,
		LightingScenario::FirstPersonRows
	);
	const bool glass =
		scenario == LightingScenario::GlassBeforeBody || scenario == LightingScenario::GlassBehindBody;
	InstallNative(renderer, glass);
	const bool nearbyReceivers = scenario != LightingScenario::Original;
	const bool materialEffects = scenario == LightingScenario::FogMaterial;
	const bool brightAmbient = scenario == LightingScenario::BrightAmbient;
	const bool firstPerson =
		scenario == LightingScenario::FirstPersonRig || scenario == LightingScenario::FirstPersonRows;
	const bool shadows = scenario == LightingScenario::Shadows || firstPerson;
	const bool prepared = shadows ? GENERATE(false, true) : false;
	CAPTURE(prepared);
	const std::array<uint32_t, 3> hiddenBodyRows{0, 1, 2};
	const auto selectPrimary = [&](View &view) {
		if (!firstPerson) return;
		view.EyePlayer = 91;
		if (scenario == LightingScenario::FirstPersonRig)
			view.EyeRig = 400;
		else
			view.EyeHiddenRows = hiddenBodyRows;
	};
	const std::array scenarioNames{
		"original",
		"contact",
		"fog-material",
		"ambient-8",
		"glass-before-body",
		"glass-behind-body",
		"shadows",
		"first-person-rig",
		"first-person-rows"
	};
	const std::string scenarioName = scenarioNames[static_cast<size_t>(scenario)];
	CAPTURE(scenarioName);
	const std::array<float, 3> scales{1, 2, .5f};
	std::array<View, 3> views;
	std::array<std::vector<scene::DrawInstance>, 3> rooms{
		Frame(100, 2, 1.6f, {.65f, .65f, .65f}),
		Frame(200, 8, 2.4f, {.65f, .65f, .65f}),
		std::vector{Panel(300, 0, nearbyReceivers ? 4.2f : 6.f, 30, 30, {.65f, .65f, .65f})}
	};
	if (nearbyReceivers) rooms[2].push_back(Panel(301, .85f, 3.85f, .18f, 1.2f, {.35f, .35f, .35f}));
	if (shadows) {
		// Keep the light-space texel small enough to resolve both cast silhouettes.
		rooms[2][0].HalfExtent = {4, 4, .001f};
		rooms[2][1].CastShadow = true;
		// Shadow rendering culls front faces; a closed slab supplies its back face.
		rooms[2][1].Mesh = {};
		rooms[2][1].HalfExtent.Z = .02f;
		rooms[2][1].Frame.Position.Z -= .02f;
	}
	if (glass) {
		auto panel = Panel(
			302, 0, scenario == LightingScenario::GlassBeforeBody ? 3.9f : 4.1f, 1.2f, 1.2f, {.8f, .2f, .7f}
		);
		panel.Transparency = .5f;
		panel.Alpha = scene::AlphaMode::Transparency;
		rooms[2].push_back(panel);
	}
	auto rootBody = std::array{
		Panel(400, -.7f, 1.5f, .2f, .2f, {.8f, .8f, .8f}),
		Panel(401, .7f, 3, .3f, .3f, {.8f, .8f, .8f}),
		Panel(402, 0, 8, 1.4f, 1.4f, {.8f, .8f, .8f})
	};
	if (firstPerson)
		for (auto &row : rootBody)
			row.Rig = 400;
	if (shadows) {
		rootBody[2].CastShadow = true;
		rootBody[2].Mesh = {};
		rootBody[2].HalfExtent.Z = .04f;
		rootBody[2].Frame.Position.Z -= .04f;
	}
	if (materialEffects) {
		const auto material = [](scene::DrawInstance &row) {
			row.OcclusionMap = MATERIAL_AO;
			row.EmissiveMap = EMISSION;
			row.EmissiveTint = {.2f, .6f, 1};
			row.EmissiveStrength = .12f;
		};
		for (auto &room : rooms)
			for (auto &row : room)
				material(row);
		for (auto &row : rootBody)
			material(row);
	}
	std::array<std::array<std::vector<scene::DrawInstance>, 3>, 2> bodies;
	PortalCaptureTree shape;
	for (size_t index = 0; index < views.size(); ++index) {
		PortalCaptureTreeNode node;
		node.Producer = {"lighting-room-" + std::to_string(index), "portal-image-requests", 1, index + 1};
		const float near = .1f * scales[index];
		node.Camera.Frustum = {-near, near, -near, near, near, 100 * scales[index]};
		node.Camera.Projection = index == 0 ? PortalImageProjection::Eye : PortalImageProjection::Seam;
		if (index != 0) {
			const float entrance = index == 1 ? 4.f : 2.f;
			node.Camera.ClipPlane = {0, 0, -1, -entrance + scene::PortalClipBias(entrance)};
		}
		views[index] = base;
		REQUIRE(ResolvePortalCaptureCamera(
			{node.Camera.Position, node.Camera.Orientation, node.Camera.Frustum, node.Camera.ClipPlane},
			node.Camera.Projection,
			views[index]
		));
		for (size_t pose = 0; pose < bodies.size(); ++pose)
			for (auto row : rootBody) {
				row.Frame.Position.X += float(pose) * .25f;
				bodies[pose][index].push_back(Scaled(row, scales[index]));
			}
		shape.Nodes.push_back(std::move(node));
	}
	shape.Edges = {Edge(0, 2, 1.6f, 2), Edge(1, 8, 2.4f, .25f)};
	const auto bindingFor = [&](const PortalCaptureTreeNode &node, size_t index) {
		PortalImageBinding binding;
		binding.World = base.World;
		binding.WorldName = base.WorldName;
		binding.Portal = core::Name(node.Layers.Opaque.Key.PortalKey);
		binding.Expected = node.Layers.Opaque.Key;
		binding.ExpectedScope = PortalImageScope::OpaqueLighting;
		binding.ExpectedProjection = node.Camera.Projection;
		binding.Sampling = *views[index].Projection * views[index].CameraFrame.Inverse().ToMatrix();
		return binding;
	};
	const auto replyFor = [&](ResourceImage image, size_t index, int revision, PortalImageScope scope) {
		PortalImageReply reply;
		reply.Key = {uint64_t(revision + 1), "lighting-room-" + std::to_string(index), 1, 1};
		reply.Scope = scope;
		reply.Status = PortalImageStatus::Ok;
		reply.CaptureTick = revision + 1;
		reply.ContentRevision = 1;
		reply.LightingRevision = revision + 1;
		reply.Width = image.Width;
		reply.Height = image.Height;
		reply.RowStride = image.RowStride;
		reply.Pixels = std::move(image.Pixels);
		reply.Depth = std::move(image.Depth);
		reply.PixelHash = assets::Hasher::Of(reply.Pixels);
		reply.DepthHash = assets::Hasher::Of(reply.Depth);
		if (scope == PortalImageScope::OpaqueLighting && !image.Normal.empty()) {
			REQUIRE(image.Normal.size() == size_t(EXTENT) * EXTENT * 4);
			REQUIRE(image.AmbientResponse.size() == size_t(EXTENT) * EXTENT * 16);
			REQUIRE(image.LightingBaseline.size() == size_t(EXTENT) * EXTENT * 16);
			REQUIRE(image.DirectionalResponse.size() == size_t(EXTENT) * EXTENT * 16);
			reply.Normal = std::move(image.Normal);
			reply.AmbientResponse = std::move(image.AmbientResponse);
			reply.LightingBaseline = std::move(image.LightingBaseline);
			reply.DirectionalResponse = std::move(image.DirectionalResponse);
			reply.NormalHash = assets::Hasher::Of(reply.Normal);
			reply.AmbientResponseHash = assets::Hasher::Of(reply.AmbientResponse);
			reply.LightingBaselineHash = assets::Hasher::Of(reply.LightingBaseline);
			reply.DirectionalResponseHash = assets::Hasher::Of(reply.DirectionalResponse);
		}
		return reply;
	};
	const auto compare = [&](const std::vector<float> &expected,
							 const std::vector<float> &actual,
							 const std::string &label) {
		const auto image = [](const std::vector<float> &pixels) {
			return test::ImageView{
				EXTENT, EXTENT, test::ImageFormat::Rgba32Float, std::as_bytes(std::span(pixels)), EXTENT * 16
			};
		};
		test::CheckImage(
			renderer,
			"nested-room-lighting",
			scenarioName + "-" + label,
			"Native complete rooms versus retained room tree with current body",
			image(expected),
			image(actual),
			{.Absolute = .002, .Region = {}}
		);
	};
	std::array<std::array<std::vector<float>, 3>, 2> oldExpected;
	uint64_t retainedTree = 0;
	std::array<std::array<PortalCaptureLighting, 3>, 2> sourceLighting;
	bool checkedJobCancellation = false, checkedJobBudget = false, checkedJobAssembly = false;
	std::array<std::pair<uint64_t, uint64_t>, 2> preparations{};
	const auto prepare = [&](uint64_t token, const View &referenceBody) {
		uint64_t handle = 0;
		REQUIRE(
			renderer.BeginPortalCaptureTreePreparation(token, referenceBody, handle) ==
			PortalTreeCompositionStatus::Pending
		);
		REQUIRE(handle != 0);
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
		while (std::chrono::steady_clock::now() < deadline) {
			const auto progress = renderer.PollPortalCaptureTreePreparation(handle);
			if (progress.Status == PortalTreeCompositionStatus::Complete) {
				const auto slot =
					std::find_if(preparations.begin(), preparations.end(), [](const auto &entry) {
						return entry.first == 0;
					});
				REQUIRE(slot != preparations.end());
				*slot = {token, handle};
				return;
			}
			REQUIRE(progress.Status == PortalTreeCompositionStatus::Pending);
			if (!progress.Request) {
				SDL_Delay(1);
				continue;
			}
			const auto &request = *progress.Request;
			const auto source = std::find_if(shape.Nodes.begin(), shape.Nodes.end(), [&](const auto &node) {
				return node.Producer == request.Producer;
			});
			REQUIRE(source != shape.Nodes.end());
			const size_t index = size_t(source - shape.Nodes.begin());
			REQUIRE(request.CaptureTick >= 1);
			REQUIRE(request.CaptureTick <= 2);
			auto sourceView = views[index];
			std::array<SceneLight, MAX_PORTAL_CAPTURE_LIGHTS> lights;
			REQUIRE(ResolvePortalCaptureLighting(
				sourceLighting[request.CaptureTick - 1][index], sourceView, lights
			));
			const auto domain = graph::BoundsOfAll(rooms[index]).Union(graph::BoundsOfAll(bodies[0][index]));
			PortalShadowSnapshot identity;
			identity.Producer = request.Producer;
			identity.Eye = request.Eye;
			identity.CaptureTick = request.CaptureTick;
			identity.ContentRevision = request.ContentRevision;
			identity.LightingRevision = request.LightingRevision;
			identity.EyePixelHash = request.EyePixelHash;
			identity.ExcludedPlayer = request.ExcludedPlayer;
			auto image = SourceShadow(renderer, sourceView, rooms[index], domain, std::move(identity));
			REQUIRE(
				renderer.AcceptPortalPreparedShadow(handle, std::move(image)) ==
				PortalTreeCompositionStatus::Pending
			);
		}
		renderer.CancelPortalCaptureTreePreparation(handle);
		FAIL("retained shadow preparation did not complete");
	};
	const auto cancelPreparation = [&](uint64_t token) {
		for (auto &entry : preparations) {
			if (entry.first != token) continue;
			renderer.CancelPortalCaptureTreePreparation(entry.second);
			entry = {};
		}
	};
	const auto compose = [&](uint64_t token, const View &body) -> uint64_t {
		if (!shadows) return renderer.ComposePortalCaptureTree(token, body);
		uint64_t job = 0;
		auto temporaryRows = std::vector(body.Instances.begin(), body.Instances.end());
		auto temporaryJoints = std::vector(body.JointFrames.begin(), body.JointFrames.end());
		auto temporaryHidden = std::vector(body.EyeHiddenRows.begin(), body.EyeHiddenRows.end());
		auto temporaryTarget = *body.Target;
		auto temporaryView = body;
		temporaryView.Instances = temporaryRows;
		temporaryView.JointFrames = temporaryJoints;
		temporaryView.EyeHiddenRows = temporaryHidden;
		temporaryView.Target = &temporaryTarget;
		if (!prepared && !checkedJobCancellation) {
			for (const auto &hidden : std::array{
					 std::vector<uint32_t>{static_cast<uint32_t>(temporaryRows.size())},
					 std::vector<uint32_t>{1, 0},
					 std::vector<uint32_t>{0, 0}
				 }) {
				auto invalid = temporaryView;
				invalid.EyeHiddenRows = hidden;
				uint64_t rejected = 0;
				CHECK(
					renderer.BeginPortalCaptureTreeComposition(token, invalid, rejected) ==
					PortalTreeCompositionStatus::Invalid
				);
				CHECK(rejected == 0);
			}
			uint64_t cancelled = 0;
			REQUIRE(
				renderer.BeginPortalCaptureTreeComposition(token, temporaryView, cancelled) ==
				PortalTreeCompositionStatus::Pending
			);
			REQUIRE(renderer.PollPortalCaptureTreeComposition(cancelled).Request);
			renderer.CancelPortalCaptureTreeComposition(cancelled);
			CHECK(
				renderer.PollPortalCaptureTreeComposition(cancelled).Status ==
				PortalTreeCompositionStatus::Invalid
			);
			checkedJobCancellation = true;
		}
		if (prepared) {
			const auto entry = std::find_if(preparations.begin(), preparations.end(), [&](const auto &value) {
				return value.first == token;
			});
			REQUIRE(entry != preparations.end());
			if (!checkedJobCancellation) {
				auto outsideRows = temporaryRows;
				outsideRows.back().Frame.Position.X += 10000;
				auto outside = temporaryView;
				outside.Instances = outsideRows;
				const auto before = renderer.PortalImageUsage();
				uint64_t rejected = 0;
				CHECK(
					renderer.BeginPreparedPortalCaptureTreeComposition(entry->second, outside, rejected) ==
					PortalTreeCompositionStatus::Invalid
				);
				CHECK(rejected == 0);
				CHECK(renderer.PortalImageUsage().Uploads == before.Uploads);
				CHECK(renderer.PortalImageUsage().TextureBytes == before.TextureBytes);
				CHECK(renderer.PortalImageUsage().PendingCpuBytes == before.PendingCpuBytes);
				checkedJobCancellation = true;
			}
			REQUIRE(
				renderer.BeginPreparedPortalCaptureTreeComposition(entry->second, temporaryView, job) ==
				PortalTreeCompositionStatus::Pending
			);
		} else {
			REQUIRE(
				renderer.BeginPortalCaptureTreeComposition(token, temporaryView, job) ==
				PortalTreeCompositionStatus::Pending
			);
		}
		// A job must own geometry and target dimensions across asynchronous source waits.
		decltype(temporaryRows){}.swap(temporaryRows);
		decltype(temporaryJoints){}.swap(temporaryJoints);
		decltype(temporaryHidden){}.swap(temporaryHidden);
		temporaryTarget = {};
		REQUIRE(job != 0);
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
		while (std::chrono::steady_clock::now() < deadline) {
			auto progress = renderer.PollPortalCaptureTreeComposition(job);
			if (progress.Status == PortalTreeCompositionStatus::Complete) {
				REQUIRE(progress.Image != 0);
				return progress.Image;
			}
			REQUIRE(progress.Status == PortalTreeCompositionStatus::Pending);
			if (!progress.Request) {
				SDL_Delay(1);
				continue;
			}
			REQUIRE_FALSE(prepared);
			const auto &request = *progress.Request;
			CHECK(request.Job == job);
			const auto source = std::find_if(shape.Nodes.begin(), shape.Nodes.end(), [&](const auto &node) {
				return node.Producer == request.Producer;
			});
			REQUIRE(source != shape.Nodes.end());
			const size_t index = size_t(source - shape.Nodes.begin());
			REQUIRE(request.CaptureTick >= 1);
			REQUIRE(request.CaptureTick <= 2);
			// This test source retains immutable scene/material inputs for both captured revisions.
			// Production must refuse a refit when it cannot retain that exact source snapshot.
			auto sourceView = views[index];
			std::array<SceneLight, MAX_PORTAL_CAPTURE_LIGHTS> sourceLights;
			REQUIRE(ResolvePortalCaptureLighting(
				sourceLighting[request.CaptureTick - 1][index], sourceView, sourceLights
			));
			auto domain = graph::BoundsOfAll(rooms[index]);
			if (request.BodyBounds) domain = domain.Union(*request.BodyBounds);
			PortalShadowSnapshot identity;
			identity.Producer = request.Producer;
			identity.Eye = request.Eye;
			identity.CaptureTick = request.CaptureTick;
			identity.ContentRevision = request.ContentRevision;
			identity.LightingRevision = request.LightingRevision;
			identity.EyePixelHash = request.EyePixelHash;
			identity.ExcludedPlayer = request.ExcludedPlayer;
			auto image = SourceShadow(renderer, sourceView, rooms[index], domain, std::move(identity));
			if (!checkedJobBudget) {
				PortalShadowImageBinding occupied{body.World, body.WorldName, image.Snapshot};
				const auto blocker = renderer.QueuePortalShadowImage(occupied, PortalShadowImage(image));
				REQUIRE(blocker != 0);
				CHECK(
					renderer.PollPortalCaptureTreeComposition(job).Status ==
					PortalTreeCompositionStatus::BudgetExceeded
				);
				CHECK(
					renderer.AcceptPortalCaptureTreeShadow(job, std::move(image)) ==
					PortalTreeCompositionStatus::BudgetExceeded
				);
				CHECK(image.Depth.size() == PORTAL_SHADOW_BYTES);
				REQUIRE(renderer.DropPortalShadowImage(blocker));
				REQUIRE(renderer.PollPortalCaptureTreeComposition(job).Request);
				checkedJobBudget = true;
			}
			++image.Snapshot.CaptureTick;
			CHECK(
				renderer.AcceptPortalCaptureTreeShadow(job, std::move(image)) ==
				PortalTreeCompositionStatus::Invalid
			);
			CHECK(image.Depth.size() == PORTAL_SHADOW_BYTES);
			--image.Snapshot.CaptureTick;
			std::optional<PortalShadowImage> lateResponse;
			if (request.Node == 0) lateResponse = image;
			if (!checkedJobAssembly) {
				const size_t before = renderer.PortalImageUsage().PendingCpuBytes;
				auto wrong = image.Snapshot;
				++wrong.CaptureTick;
				CHECK(
					renderer.BeginPortalCaptureTreeShadowAssembly(job, wrong) ==
					PortalTreeCompositionStatus::Invalid
				);
				CHECK(renderer.PortalImageUsage().PendingCpuBytes == before);
				REQUIRE(
					renderer.BeginPortalCaptureTreeShadowAssembly(job, image.Snapshot) ==
					PortalTreeCompositionStatus::Pending
				);
				CHECK(renderer.PortalImageUsage().PendingCpuBytes == before + PORTAL_SHADOW_BYTES);
				CHECK(
					renderer.BeginPortalCaptureTreeShadowAssembly(job, image.Snapshot) ==
					PortalTreeCompositionStatus::Pending
				);
				CHECK(renderer.PortalImageUsage().PendingCpuBytes == before + PORTAL_SHADOW_BYTES);
				CHECK(
					renderer.AcceptPortalCaptureTreeShadow(job, std::move(image)) ==
					PortalTreeCompositionStatus::Invalid
				);
				CHECK(image.Depth.size() == PORTAL_SHADOW_BYTES);
				const uint64_t cancelledAssembly = job;
				renderer.CancelPortalCaptureTreeComposition(job);
				CHECK(renderer.PortalImageUsage().PendingCpuBytes == before);
				REQUIRE(
					renderer.BeginPortalCaptureTreeComposition(token, body, job) ==
					PortalTreeCompositionStatus::Pending
				);
				REQUIRE(
					renderer.BeginPortalCaptureTreeShadowAssembly(job, image.Snapshot) ==
					PortalTreeCompositionStatus::Pending
				);
				PortalShadowImageBinding binding{body.World, body.WorldName, image.Snapshot};
				{
					auto tooLarge = image;
					tooLarge.Depth.reserve(PORTAL_SHADOW_BYTES + 1);
					CHECK(renderer.QueuePortalShadowImage(binding, std::move(tooLarge)) == 0);
					CHECK(tooLarge.Depth.size() == PORTAL_SHADOW_BYTES);
				}
				const auto blocker = renderer.QueuePortalShadowImage(binding, PortalShadowImage(image));
				REQUIRE(blocker != 0);
				CHECK(renderer.PortalImageUsage().PendingCpuBytes == before + 2 * PORTAL_SHADOW_BYTES);
				std::vector<std::byte> packet;
				std::string error;
				for (uint8_t tile = 0; tile < PORTAL_SHADOW_TILE_COUNT; ++tile) {
					REQUIRE(EncodePortalShadowTile(image, tile, packet, error));
					if (tile == 0) {
						CHECK(
							renderer.AcceptPortalCaptureTreeShadowTile(
								cancelledAssembly, request.Node, packet
							) == PortalTreeCompositionStatus::Invalid
						);
						CHECK(
							renderer.AcceptPortalCaptureTreeShadowTile(job, request.Node + 1, packet) ==
							PortalTreeCompositionStatus::Invalid
						);
						auto damaged = packet;
						damaged.back() ^= std::byte{1};
						CHECK(
							renderer.AcceptPortalCaptureTreeShadowTile(job, request.Node, damaged) ==
							PortalTreeCompositionStatus::Invalid
						);
					}
					CHECK(
						renderer.AcceptPortalCaptureTreeShadowTile(job, request.Node, packet) ==
						(tile + 1 == PORTAL_SHADOW_TILE_COUNT ? PortalTreeCompositionStatus::BudgetExceeded
															  : PortalTreeCompositionStatus::Pending)
					);
				}
				CHECK(renderer.PortalImageUsage().PendingCpuBytes == before + 2 * PORTAL_SHADOW_BYTES);
				CHECK(
					renderer.CommitPortalCaptureTreeShadowAssembly(job, request.Node) ==
					PortalTreeCompositionStatus::BudgetExceeded
				);
				REQUIRE(renderer.DropPortalShadowImage(blocker));
				CHECK(renderer.PortalImageUsage().PendingCpuBytes == before + PORTAL_SHADOW_BYTES);
				REQUIRE(
					renderer.CommitPortalCaptureTreeShadowAssembly(job, request.Node) ==
					PortalTreeCompositionStatus::Pending
				);
				checkedJobAssembly = true;
			} else {
				REQUIRE(
					renderer.AcceptPortalCaptureTreeShadow(job, std::move(image)) ==
					PortalTreeCompositionStatus::Pending
				);
			}
			if (lateResponse) {
				// A late reply must never consume the completed output instead of Poll.
				SDL_Delay(1);
				renderer.AcceptPortalCaptureTreeShadow(job, std::move(*lateResponse));
			}
			CHECK(renderer.PortalImageUsage().TextureBytes <= MAX_IMPORTED_PORTAL_TEXTURE_BYTES);
			CHECK(renderer.PortalImageUsage().PendingCpuBytes <= MAX_IMPORTED_PORTAL_CPU_BYTES);
		}
		renderer.CancelPortalCaptureTreeComposition(job);
		FAIL("retained shadow composition did not complete");
		return 0;
	};
	for (int revision = 0; revision < 2; ++revision) {
		CAPTURE(revision);
		auto tree = shape;
		std::array<PortalCaptureLighting, 3> lighting;
		std::array<std::array<SceneLight, MAX_PORTAL_CAPTURE_LIGHTS>, 3> lightStorage;
		for (size_t index = 0; index < views.size(); ++index) {
			auto &light = lighting[index];
			light.Direction = shadows ? std::array{-.8f, 0.f, -.6f} : std::array{0.f, 0.f, -1.f};
			const float ambient = brightAmbient ? 8.f : materialEffects ? .3f : .04f;
			light.Ambient = {ambient, ambient, ambient};
			if (materialEffects) {
				light.FogColour = {.03f, .07f, .11f};
				light.FogStart = scales[index];
				light.FogEnd = 16 * scales[index];
			}
			light.OutdoorAmbient = light.Ambient;
			light.Direct[index] = .3f;
			light.LightCount = 1;
			light.Lights[0].Position = {0, 0, 0};
			light.Lights[0].Range = 24 * scales[index];
			light.Lights[0].Colour = {.1f, .1f, .1f};
			if (index == 2 && revision != 0) {
				if (shadows) light.Direction = {-.6f, 0.f, -.8f};
				light.Lights[0].Position = {1, 0, 1};
				light.Lights[0].Colour = {2, .1f, .1f};
			}
			sourceLighting[revision][index] = light;
			REQUIRE(ResolvePortalCaptureLighting(light, views[index], lightStorage[index]));
			auto captured =
				NativeCapture(renderer, views[index], rooms[index], true, false, glass && index == 2);
			auto reply =
				replyFor(std::move(captured.Export), index, revision, PortalImageScope::OpaqueLighting);
			reply.CaptureLighting = light;
			tree.Nodes[index].Layers.Opaque = std::move(reply);
			if (glass && index == 2) {
				REQUIRE(captured.Glass);
				auto layer =
					replyFor(std::move(*captured.Glass), index, revision, PortalImageScope::OpaqueLighting);
				layer.CaptureLighting = light;
				tree.Nodes[index].Layers.Transparent.push_back(std::move(layer));
			}
		}
		REQUIRE(ValidPortalCaptureTree(tree));

		// Native reference renders complete room geometry and body in one normal
		// view. Only complete native child images pass through the ordinary pane.
		std::array<std::array<std::vector<float>, 3>, 2> expected;
		for (size_t pose = 0; pose < bodies.size(); ++pose) {
			uint64_t nativeChild = 0;
			for (int index = 2; index >= 0; --index) {
				auto rows = rooms[index];
				rows.insert(rows.end(), bodies[pose][index].begin(), bodies[pose][index].end());
				auto view = views[index];
				PortalView aperture;
				if (index < 2) {
					const auto &edge = tree.Edges[index];
					auto pane =
						Panel(500 + index, 0, -edge.Centre[2], edge.First[0], edge.Second[1], {1, 1, 1});
					pane.Surface = 0;
					rows.push_back(pane);
					aperture.Index = 0;
					aperture.ExternalImage = true;
					aperture.ImportedImage = nativeChild;
					aperture.ImagePortal = core::Name(edge.PortalKey);
					aperture.Centre = {0, 0, edge.Centre[2]};
					aperture.Normal = {0, 0, 1};
					aperture.First = {edge.First[0], 0, 0};
					aperture.Second = {0, edge.Second[1], 0};
					aperture.Warp.Scale = edge.Scale;
					view.Portals = std::span(&aperture, 1);
				}
				std::array<uint32_t, 3> nativeHidden{};
				if (index == 0 && firstPerson) {
					selectPrimary(view);
					if (scenario == LightingScenario::FirstPersonRows) {
						for (size_t row = 0; row < nativeHidden.size(); ++row)
							nativeHidden[row] = static_cast<uint32_t>(rooms[index].size() + row);
						view.EyeHiddenRows = nativeHidden;
					}
				}
				auto captured = NativeCapture(renderer, view, rows);
				if (index == 0 && firstPerson) {
					auto visible = view;
					visible.EyeRig = 0;
					visible.EyePlayer.reset();
					visible.EyeHiddenRows = {};
					const auto control = NativeCapture(renderer, visible, rows);
					CHECK_FALSE(std::ranges::equal(control.Export.Pixels, captured.Export.Pixels));
				}
				expected[pose][index] = Colour(captured.Export.Pixels);
				if (index == 2 && nearbyReceivers) {
					// Observe SSAO before glass attenuates the underlying opaque radiance.
					auto opaque = glass ? NativeCapture(renderer, view, rows, true) : NativeImage{};
					auto unoccluded = NativeCapture(renderer, view, rows, glass, true);
					const auto control = Colour(unoccluded.Export.Pixels);
					const auto occluded = glass ? Colour(opaque.Export.Pixels) : expected[pose][index];
					size_t darkerRoom = 0, darkerBody = 0;
					core::ByteReader depths(captured.Export.Depth);
					for (size_t pixel = 0; pixel < control.size(); pixel += 4) {
						const float depth = depths.ReadFloat();
						const float reduction = control[pixel] - occluded[pixel];
						if (std::abs(depth - 4.f) < .001f)
							darkerBody += reduction > .0001f;
						else
							darkerRoom += reduction > .002f;
					}
					CHECK(darkerRoom > 0);
					CHECK(darkerBody > 0);
				}
				if (index == 2 && shadows) {
					// Toggle only casting, so depth, material and SSAO remain the same.
					for (const bool bodyCaster : {false, true}) {
						CAPTURE(bodyCaster);
						auto controlRows = rows;
						for (auto &row : controlRows)
							if (row.Source == (bodyCaster ? 402u : 301u)) row.CastShadow = false;
						const auto control = Colour(NativeCapture(renderer, view, controlRows).Export.Pixels);
						core::ByteReader depths(captured.Export.Depth);
						size_t shadowedReceiverPixels = 0;
						float maximumReduction = 0;
						for (size_t pixel = 0; pixel < control.size(); pixel += 4) {
							const float depth = depths.ReadFloat();
							if (std::abs(depth - (bodyCaster ? 4.2f : 4.f)) >= .001f) continue;
							float reduction = 0;
							for (size_t channel = 0; channel < 3; ++channel)
								reduction = std::max(
									reduction,
									control[pixel + channel] - expected[pose][index][pixel + channel]
								);
							maximumReduction = std::max(maximumReduction, reduction);
							shadowedReceiverPixels += reduction > .002f;
						}
						CAPTURE(shadowedReceiverPixels, maximumReduction);
						REQUIRE(shadowedReceiverPixels > 0);
					}
				}
				if (index == 2 && glass) {
					auto withoutGlass = rows;
					std::erase_if(withoutGlass, [](const auto &row) { return row.Source == 302; });
					const auto control = Colour(NativeCapture(renderer, view, withoutGlass).Export.Pixels);
					const size_t centre = (EXTENT / 2 * EXTENT + EXTENT / 2) * 4;
					float bodyDifference = 0;
					size_t changedRoom = 0;
					core::ByteReader depths(captured.Export.Depth);
					for (size_t pixel = 0; pixel < control.size(); pixel += 4) {
						const float depth = depths.ReadFloat();
						float difference = 0;
						for (size_t channel = 0; channel < 3; ++channel)
							difference = std::max(
								difference,
								std::abs(control[pixel + channel] - expected[pose][index][pixel + channel])
							);
						if (pixel == centre) bodyDifference = difference;
						if (std::abs(depth - 4.2f) < .001f) changedRoom += difference > .002f;
					}
					CHECK(changedRoom > 0);
					if (scenario == LightingScenario::GlassBeforeBody)
						CHECK(bodyDifference > .002f);
					else
						CHECK(bodyDifference <= .002f);
				}
				if (index == 2 && materialEffects) {
					// Each authored contribution must affect the independent native result.
					for (int removed = 0; removed < 3; ++removed) {
						CAPTURE(removed);
						auto controlView = view;
						auto controlRows = rows;
						if (removed == 0) {
							controlView.Lighting.FogStart = 100000;
							controlView.Lighting.FogEnd = 100001;
						}
						for (auto &row : controlRows) {
							if (removed == 1) row.EmissiveStrength = 0;
							if (removed == 2) row.OcclusionMap = {};
						}
						const auto control =
							Colour(NativeCapture(renderer, controlView, controlRows).Export.Pixels);
						size_t changedRoom = 0, changedBody = 0;
						core::ByteReader depths(captured.Export.Depth);
						for (size_t pixel = 0; pixel < control.size(); pixel += 4) {
							const float depth = depths.ReadFloat();
							float difference = 0;
							for (size_t channel = 0; channel < 3; ++channel)
								difference = std::max(
									difference,
									std::abs(
										control[pixel + channel] - expected[pose][index][pixel + channel]
									)
								);
							if (std::abs(depth - 4.f) < .001f)
								changedBody += difference > .002f;
							else if (depth > 0)
								changedRoom += difference > .002f;
						}
						CHECK(changedRoom > 0);
						CHECK(changedBody > 0);
					}
				}
				if (index == 2 && brightAmbient) {
					const size_t centre = (EXTENT / 2 * EXTENT + EXTENT / 2) * 4;
					CHECK(expected[pose][index][centre] > 4.f);
				}
				if (nativeChild) REQUIRE(renderer.DropPortalImage(nativeChild));
				nativeChild = 0;
				if (index > 0) {
					const auto &edge = tree.Edges[index - 1];
					auto reply = replyFor(
						std::move(captured.Export), index, revision, PortalImageScope::CompleteWorld
					);
					reply.Key.PortalKey = edge.PortalKey;
					auto binding = bindingFor(tree.Nodes[index], index);
					binding.Portal = core::Name(edge.PortalKey);
					binding.Expected = reply.Key;
					binding.ExpectedScope = PortalImageScope::CompleteWorld;
					glm::mat4 warp{1};
					for (size_t axis = 0; axis < 3; ++axis)
						warp[axis] *= edge.Scale;
					binding.Sampling *= warp;
					nativeChild = renderer.QueuePortalImage(binding, std::move(reply));
					REQUIRE(nativeChild != 0);
				}
			}
		}

		for (int root = 2; root >= 0; --root) {
			CAPTURE(root);
			auto subtree = tree;
			subtree.Nodes.erase(subtree.Nodes.begin(), subtree.Nodes.begin() + root);
			subtree.Edges.clear();
			for (auto edge : tree.Edges) {
				if (edge.Parent < root) continue;
				edge.Parent -= root;
				edge.Child -= root;
				subtree.Edges.push_back(std::move(edge));
			}
			const auto token =
				renderer.QueuePortalCaptureTree(bindingFor(tree.Nodes[root], root), std::move(subtree));
			REQUIRE(token != 0);
			OverlayImage overlay;
			renderer.Render(std::span(&views[root], 1), overlay, nullptr, false);
			REQUIRE(renderer.PortalCaptureTreeReady(token));
			if (revision == 1 && root == 0) {
				REQUIRE(renderer.PortalCaptureTreeReady(retainedTree));
				CHECK(renderer.PortalImageUsage().Images == (glass ? 8 : prepared ? 9 : 6));
			}
			if (prepared) {
				auto referenceBody = views[root];
				referenceBody.Instances = bodies[0][root];
				if (root == 0) selectPrimary(referenceBody);
				prepare(token, referenceBody);
			}
			for (size_t pose = 0; pose < bodies.size(); ++pose) {
				CAPTURE(pose);
				auto bodyView = views[root];
				bodyView.Instances = bodies[pose][root];
				if (root == 0) selectPrimary(bodyView);
				bodyView.Lighting.Ambient = {4, 0, 4};
				bodyView.Lighting.Direct = {0, 4, 0};
				bodyView.Lights = {};
				const auto before = renderer.PortalImageUsage();
				const auto composed = compose(token, bodyView);
				CHECK(
					renderer.PortalImageUsage().Uploads ==
					before.Uploads + (shadows && !prepared ? 3 - root : 0)
				);
				CHECK(
					renderer.PortalImageUsage().UploadedBytes ==
					before.UploadedBytes + (shadows && !prepared ? (3 - root) * PORTAL_SHADOW_BYTES : 0)
				);
				REQUIRE(composed != 0);
				const auto actual = Colour(ReadComposed(
					renderer, glass && root == 2 ? "transparent-0-composed-colour" : "composed-colour", 8
				));
				compare(
					expected[pose][root],
					actual,
					"revision-" + std::to_string(revision) + "-room-" + std::to_string(root) + "-pose-" +
						std::to_string(pose)
				);
				const size_t centre = (EXTENT / 2 * EXTENT + EXTENT / 2) * 4;
				REQUIRE(actual[centre + 2] > .01f);
				const std::array<size_t, 3> ownBodyX{17, 40, 32};
				const size_t ownBodyPixel = (EXTENT / 2 * EXTENT + ownBodyX[root]) * 4;
				if (revision == 0 && pose == 0 && !materialEffects && !brightAmbient && !glass &&
					!(firstPerson && root == 0))
					CHECK(actual[ownBodyPixel + root] > actual[ownBodyPixel + (root + 1) % 3] + .01f);
				if (revision == 1)
					CHECK(expected[pose][root][centre] > oldExpected[pose][root][centre] + .01f);
				REQUIRE(renderer.DropPortalImage(composed));
			}
			if (root == 0 && revision == 0) {
				retainedTree = token;
				if (prepared) {
					renderer.ReleasePortalCaptureTree(token);
					REQUIRE(renderer.PortalCaptureTreeReady(token));
				}
			} else {
				if (prepared) cancelPreparation(token);
				renderer.DropPortalCaptureTree(token);
			}
		}
		if (revision == 0)
			oldExpected = expected;
		else {
			REQUIRE(renderer.PortalCaptureTreeReady(retainedTree));
			for (size_t pose = 0; pose < bodies.size(); ++pose) {
				auto oldBody = views[0];
				oldBody.Instances = bodies[pose][0];
				selectPrimary(oldBody);
				const auto before = renderer.PortalImageUsage();
				const auto composed = compose(retainedTree, oldBody);
				REQUIRE(composed != 0);
				CHECK(renderer.PortalImageUsage().Uploads == before.Uploads + (shadows && !prepared ? 3 : 0));
				CHECK(
					renderer.PortalImageUsage().UploadedBytes ==
					before.UploadedBytes + (shadows && !prepared ? 3 * PORTAL_SHADOW_BYTES : 0)
				);
				compare(
					oldExpected[pose][0],
					Colour(ReadComposed(renderer, "composed-colour", 8)),
					"retained-old-lighting-pose-" + std::to_string(pose)
				);
				REQUIRE(renderer.DropPortalImage(composed));
			}
		}
	}
	uint64_t retiredJob = 0;
	if (shadows) {
		auto body = views[0];
		body.Instances = bodies[0][0];
		REQUIRE(
			renderer.BeginPortalCaptureTreeComposition(retainedTree, body, retiredJob) ==
			PortalTreeCompositionStatus::Pending
		);
		REQUIRE(renderer.PollPortalCaptureTreeComposition(retiredJob).Request);
	}
	renderer.DropPortalCaptureTree(retainedTree);
	if (prepared) {
		for (const auto &entry : preparations)
			if (entry.second)
				CHECK(
					renderer.PollPortalCaptureTreePreparation(entry.second).Status ==
					PortalTreeCompositionStatus::Invalid
				);
	}
	if (retiredJob)
		CHECK(
			renderer.PollPortalCaptureTreeComposition(retiredJob).Status ==
			PortalTreeCompositionStatus::Invalid
		);
	CHECK(renderer.PortalImageUsage().Images == 0);
}
