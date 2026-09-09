#include "RenderFixture.hpp"

#include <engine/assets/Mesh.hpp>
#include <engine/core/Bytes.hpp>
#include <engine/graph/PipelineDocument.hpp>
#include <engine/graph/Shadow.hpp>
#include <engine/render/PortalImageRuntime.hpp>
#include <engine/render/PortalShadowImageImport.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/generators/catch_generators.hpp>
#include <glm/packing.hpp>

TEST_SUITE_ID("engine.render.portalshadowimageimport")

namespace {
	using namespace engine;
	using namespace engine::render;
	constexpr uint32_t EXTENT = 65;
	const core::Name PIPELINE("shadow-import-native"), COLOUR_CAPTURE("capture-native"),
		SHADOW("capture-shadow");
	const PortalCaptureTreeEndpoint PRODUCER{"shadow-room", "capture", 7, 9};

	void Install(Renderer &renderer) {
		using namespace graph;
		PipelineDocument document;
		for (const char *name : {"baseline", "directional", "ambient"})
			document.Record(
				{.Kind = EditKind::AddResource,
				 .Name = core::Name(name),
				 .Resource = ResourceKind::Colour,
				 .Format = ResourceFormat::RGBA32F}
			);
		document.Record(
			{.Kind = EditKind::AddResource,
			 .Name = core::Name("capture-depth"),
			 .Resource = ResourceKind::Colour,
			 .Format = ResourceFormat::R32F}
		);
		const auto base = DefaultPbrDocument();
		for (const auto &edit : base.Edits()) {
			if (edit.Kind == EditKind::AddNode && edit.Name == core::Name("sky")) break;
			document.Record(edit);
			if (edit.Kind == EditKind::Writes && edit.Target == core::Name("lit")) {
				document.Record(
					{.Kind = EditKind::Writes,
					 .Target = core::Name("baseline"),
					 .Key = core::Name("lighting-baseline")}
				);
				document.Record(
					{.Kind = EditKind::Writes,
					 .Target = core::Name("directional"),
					 .Key = core::Name("directional-response")}
				);
			}
		}
		const auto node = [&](core::Name name, const char *kind, NodeScope scope = NodeScope::View) {
			document.Record(
				{.Kind = EditKind::AddNode, .Name = name, .NodeKind = core::Name(kind), .Scope = scope}
			);
		};
		const auto edge = [&](EditKind kind, const char *resource, const char *port) {
			document.Record({.Kind = kind, .Target = core::Name(resource), .Key = core::Name(port)});
		};
		node(core::Name("ambient-response"), "ambient-response");
		for (const auto &[resource, port] :
			 {std::pair{"albedo", "albedo"},
			  {"normal", "normal"},
			  {"material", "material"},
			  {"linear-depth", "depth"},
			  {"occlusion", "occlusion"}})
			edge(EditKind::Reads, resource, port);
		edge(EditKind::Writes, "ambient", "response");
		node(core::Name("export-depth"), "depth-linearise");
		edge(EditKind::Reads, "depth", "depth");
		edge(EditKind::Writes, "capture-depth", "linear");
		document.Record({.Kind = EditKind::Set, .Key = core::Name("background"), .Value = "zero"});
		node(SHADOW, "shadow-capture");
		edge(EditKind::Reads, "shadow", "shadow");
		node(COLOUR_CAPTURE, "capture", NodeScope::Frame);
		for (const auto &[resource, port] :
			 {std::pair{"lit", "source"},
			  {"capture-depth", "depth"},
			  {"normal", "normal"},
			  {"ambient", "ambient-response"},
			  {"baseline", "lighting-baseline"},
			  {"directional", "directional-response"}})
			edge(EditKind::Reads, resource, port);
		RenderGraph graph;
		core::Name offender;
		REQUIRE(Build(document, graph, offender) == PipelineDocumentStatus::Ok);
		REQUIRE(renderer.SetPipeline(PIPELINE, graph));
	}

	ResourceImage Take(Renderer &renderer, uint64_t token) {
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while (std::chrono::steady_clock::now() < deadline) {
			if (auto result = renderer.TakeResourceImage(token)) {
				REQUIRE(result->Status == ResourceImageStatus::Ok);
				return std::move(*result);
			}
			SDL_Delay(1);
		}
		FAIL("shadow import fixture capture timed out");
		return {};
	}
	std::array<float, 6> Bounds(const core::AABB &bounds) {
		return {
			bounds.Minimum.X,
			bounds.Minimum.Y,
			bounds.Minimum.Z,
			bounds.Maximum.X,
			bounds.Maximum.Y,
			bounds.Maximum.Z
		};
	}
	PortalImageReply Reply(ResourceImage image, const PortalCaptureLighting &lighting) {
		PortalImageReply reply;
		reply.Key = {31, "retained-shadow-room", 2, 3};
		reply.Status = PortalImageStatus::Ok;
		reply.Scope = PortalImageScope::OpaqueLighting;
		reply.CaptureTick = 1;
		reply.ContentRevision = 11;
		reply.LightingRevision = 13;
		reply.Width = reply.Height = EXTENT;
		reply.RowStride = EXTENT * 8;
		reply.CaptureLighting = lighting;
		const std::array source{
			&image.Pixels,
			&image.Depth,
			&image.Normal,
			&image.AmbientResponse,
			&image.LightingBaseline,
			&image.DirectionalResponse
		};
		const std::array target{
			&reply.Pixels,
			&reply.Depth,
			&reply.Normal,
			&reply.AmbientResponse,
			&reply.LightingBaseline,
			&reply.DirectionalResponse
		};
		const std::array hashes{
			&reply.PixelHash,
			&reply.DepthHash,
			&reply.NormalHash,
			&reply.AmbientResponseHash,
			&reply.LightingBaselineHash,
			&reply.DirectionalResponseHash
		};
		for (size_t plane = 0; plane < source.size(); ++plane) {
			*target[plane] = std::move(*source[plane]);
			*hashes[plane] = assets::Hasher::Of(*target[plane]);
		}
		return reply;
	}
	PortalShadowImage ShadowImage(ResourceImage image, const PortalImageReply &eye) {
		REQUIRE(image.Shadow.has_value());
		PortalShadowImage shadow;
		shadow.Snapshot.Producer = PRODUCER;
		shadow.Snapshot.Eye = eye.Key;
		shadow.Snapshot.CaptureTick = eye.CaptureTick;
		shadow.Snapshot.ContentRevision = eye.ContentRevision;
		shadow.Snapshot.LightingRevision = eye.LightingRevision;
		shadow.Snapshot.EyePixelHash = eye.PixelHash;
		shadow.Snapshot.SourceEmpty = image.Shadow->SourceEmpty;
		shadow.Snapshot.SourceBounds = Bounds(image.Shadow->SourceBounds);
		shadow.Snapshot.DomainBounds = Bounds(image.Shadow->DomainBounds);
		shadow.Snapshot.LightViewProjection = image.Shadow->LightViewProjection;
		shadow.Depth = std::move(image.Depth);
		shadow.Snapshot.DepthHash = assets::Hasher::Of(shadow.Depth);
		return shadow;
	}
	PortalShadowImage Delivered(const PortalShadowImage &source) {
		PortalShadowAssembly assembly;
		std::string error;
		REQUIRE(assembly.Begin(source.Snapshot, PORTAL_SHADOW_BYTES, error));
		for (uint8_t tile = PORTAL_SHADOW_TILE_COUNT; tile-- > 0;) {
			std::vector<std::byte> wire;
			REQUIRE(EncodePortalShadowTile(source, tile, wire, error));
			REQUIRE(wire.size() <= MAX_PORTAL_EXCHANGE_BYTES);
			REQUIRE(assembly.Accept(wire, error));
		}
		auto image = assembly.Take();
		REQUIRE(image.has_value());
		CHECK(image->Depth == source.Depth);
		return std::move(*image);
	}
	float ColourError(std::span<const std::byte> expected, std::span<const std::byte> actual) {
		REQUIRE(expected.size() == actual.size());
		core::ByteReader left(expected), right(actual);
		float maximum = 0;
		while (!left.AtEnd()) {
			const auto a = glm::unpackHalf2x16(left.ReadUInt32());
			const auto b = glm::unpackHalf2x16(right.ReadUInt32());
			REQUIRE(std::isfinite(b.x));
			REQUIRE(std::isfinite(b.y));
			maximum = std::max(maximum, std::max(std::abs(a.x - b.x), std::abs(a.y - b.y)));
		}
		return maximum;
	}
}

TEST_CASE(
	"delivered source shadows and current body reproduce native room lighting",
	"[render][gpu][shadow-import][.]"
) {
	const bool packed = GENERATE(false, true);
	CAPTURE(packed);
	test::FixtureDevice fixture;
	fixture.Initialise();
	auto &renderer = fixture.Render;
	const auto queue = [&](const PortalShadowImageBinding &binding, PortalShadowImage &&image) {
		return packed ? renderer.QueuePackedPortalShadowImage(binding, std::move(image))
					  : renderer.QueuePortalShadowImage(binding, std::move(image));
	};
	Install(renderer);
	const core::Name planeName("shadow-import-receiver");
	assets::MeshData plane;
	plane.Vertices = {
		{{-1, -1, 0}, {0, 0, 1}, {0, 1}},
		{{1, -1, 0}, {0, 0, 1}, {1, 1}},
		{{1, 1, 0}, {0, 0, 1}, {1, 0}},
		{{-1, 1, 0}, {0, 0, 1}, {0, 0}}
	};
	plane.Indices = {0, 1, 2, 0, 2, 3};
	plane.ComputeBounds();
	REQUIRE(renderer.AddMesh(planeName, plane));
	std::array<scene::DrawInstance, 3> rows;
	rows[0].Mesh = planeName;
	rows[0].Frame.Position = {0, 0, -4.2f};
	rows[0].HalfExtent = {4, 4, .001f};
	rows[0].CastShadow = false;
	rows[0].Tint = {.65f, .45f, .8f};
	rows[1].Frame.Position = {.85f, 0, -3.87f};
	rows[1].HalfExtent = {.18f, 1.2f, .02f};
	rows[2].Frame.Position = {.52f, 0, -4.04f};
	rows[2].HalfExtent = {.3f, .6f, .025f};
	for (size_t index = 0; index < rows.size(); ++index)
		rows[index].Source = index + 1;
	SceneTarget target{EXTENT, EXTENT};
	View view;
	view.Target = &target;
	view.Pipeline = PIPELINE;
	view.World = 17;
	view.WorldName = core::Name(PRODUCER.World);
	view.Instances = rows;
	PortalCaptureCamera camera;
	camera.Frustum = {-1, 1, -1, 1, 1, 1000};
	REQUIRE(ResolvePortalCaptureCamera(camera, PortalImageProjection::Eye, view));
	PortalCaptureLighting lighting;
	lighting.Direction = {-.8f, 0, -.6f};
	lighting.Ambient = {.1f, .1f, .1f};
	lighting.OutdoorAmbient = lighting.Ambient;
	lighting.Direct = {3, 2, 1};
	std::array<SceneLight, MAX_PORTAL_CAPTURE_LIGHTS> localLights;
	REQUIRE(ResolvePortalCaptureLighting(lighting, view, localLights));
	OverlayImage overlay;
	const auto nativeToken = renderer.QueueResourceImage(PIPELINE, COLOUR_CAPTURE);
	REQUIRE(nativeToken != 0);
	renderer.Render(std::span(&view, 1), overlay, nullptr, false);
	const auto native = Take(renderer, nativeToken);
	const auto nativeDepth = test::CaptureResource(
		renderer,
		core::Name("shadow"),
		0,
		PORTAL_SHADOW_EXTENT,
		PORTAL_SHADOW_EXTENT,
		test::ImageFormat::R32Float
	);
	// Both casters must change visible lighting, or native parity could pass with no shadows.
	for (const size_t caster : {size_t(1), size_t(2)}) {
		rows[caster].CastShadow = false;
		const auto unshadowedToken = renderer.QueueResourceImage(PIPELINE, COLOUR_CAPTURE);
		REQUIRE(unshadowedToken != 0);
		renderer.Render(std::span(&view, 1), overlay, nullptr, false);
		const auto unshadowed = Take(renderer, unshadowedToken);
		CAPTURE(caster);
		CHECK(ColourError(native.Pixels, unshadowed.Pixels) > .002f);
		rows[caster].CastShadow = true;
	}
	view.DirectionalShadowBounds = graph::BoundsOfAll(rows);
	view.Instances = std::span(rows).first(2);
	const std::array nodes{SHADOW, COLOUR_CAPTURE};
	std::array<uint64_t, 2> tokens{};
	REQUIRE(renderer.QueueResourceImages(PIPELINE, nodes, 0, ResourceImageDelivery::CopiedPixels, tokens));
	renderer.Render(std::span(&view, 1), overlay, nullptr, false);
	auto shadowCapture = Take(renderer, tokens[0]);
	auto room = Take(renderer, tokens[1]);
	CHECK(shadowCapture.CaptureFrame == room.CaptureFrame);
	auto reply = Reply(std::move(room), lighting);
	const auto shadow = ShadowImage(std::move(shadowCapture), reply);
	PortalShadowImageBinding shadowBinding{view.World, view.WorldName, shadow.Snapshot};
	const auto handle = queue(shadowBinding, Delivered(shadow));
	REQUIRE(handle != 0);
	CHECK_FALSE(renderer.IsPortalShadowImageReady(handle));
	const auto initialUsage = renderer.PortalImageUsage();
	if (packed) {
		CHECK(initialUsage.TextureBytes < PORTAL_SHADOW_BYTES);
		CHECK(initialUsage.PendingCpuBytes == initialUsage.TextureBytes);
	} else {
		CHECK(initialUsage.TextureBytes == PORTAL_SHADOW_BYTES);
		CHECK(initialUsage.PendingCpuBytes == PORTAL_SHADOW_BYTES);
	}
	// Shadow maps share admission with ordinary eye imports and leave rejected input owned by callers.
	const auto secondHandle = renderer.QueuePortalShadowImage(shadowBinding, PortalShadowImage(shadow));
	REQUIRE(secondHandle != 0);
	CHECK(renderer.PortalImageUsage().TextureBytes == initialUsage.TextureBytes + PORTAL_SHADOW_BYTES);
	CHECK(renderer.PortalImageUsage().PendingCpuBytes == initialUsage.PendingCpuBytes + PORTAL_SHADOW_BYTES);
	auto refusedShadow = shadow;
	CHECK(renderer.QueuePortalShadowImage(shadowBinding, std::move(refusedShadow)) == 0);
	CHECK(refusedShadow.Depth == shadow.Depth);
	PortalImageBinding refusedEye;
	refusedEye.World = view.World;
	refusedEye.WorldName = view.WorldName;
	refusedEye.Portal = core::Name(reply.Key.PortalKey);
	refusedEye.Expected = reply.Key;
	refusedEye.ExpectedScope = PortalImageScope::OpaqueLighting;
	refusedEye.ExpectedProjection = PortalImageProjection::Eye;
	auto refusedReply = reply;
	if (!packed) CHECK(renderer.QueuePortalImage(refusedEye, std::move(refusedReply)) == 0);
	REQUIRE(renderer.DropPortalShadowImage(secondHandle));
	CHECK_FALSE(renderer.DropPortalShadowImage(secondHandle));
	CHECK(renderer.PortalImageUsage().TextureBytes == initialUsage.TextureBytes);
	CHECK(renderer.PortalImageUsage().PendingCpuBytes == initialUsage.PendingCpuBytes);
	view.Instances = std::span(rows).last(1);
	view.ImportedDirectionalShadow = handle;
	renderer.Render(std::span(&view, 1), overlay, nullptr, false);
	CHECK(renderer.IsPortalShadowImageReady(handle));
	const auto mergedDepth = test::CaptureResource(
		renderer,
		core::Name("shadow"),
		0,
		PORTAL_SHADOW_EXTENT,
		PORTAL_SHADOW_EXTENT,
		test::ImageFormat::R32Float
	);
	CHECK(mergedDepth.Bytes == nativeDepth.Bytes);
	const auto uploaded = renderer.PortalImageUsage();
	CHECK(uploaded.PendingCpuBytes == 0);
	CHECK(uploaded.UploadedBytes == initialUsage.TextureBytes);
	renderer.Render(std::span(&view, 1), overlay, nullptr, false);
	CHECK(renderer.PortalImageUsage().Uploads == uploaded.Uploads);
	if (packed) {
		const auto originalPosition = rows[2].Frame.Position;
		for (const float offset : {-.15f, .15f}) {
			rows[2].Frame.Position.X = originalPosition.X + offset;
			auto direct = view;
			direct.Instances = rows;
			direct.ImportedDirectionalShadow = 0;
			renderer.Render(std::span(&direct, 1), overlay, nullptr, false);
			const auto expected = test::CaptureResource(
				renderer,
				core::Name("shadow"),
				0,
				PORTAL_SHADOW_EXTENT,
				PORTAL_SHADOW_EXTENT,
				test::ImageFormat::R32Float
			);
			renderer.Render(std::span(&view, 1), overlay, nullptr, false);
			const auto actual = test::CaptureResource(
				renderer,
				core::Name("shadow"),
				0,
				PORTAL_SHADOW_EXTENT,
				PORTAL_SHADOW_EXTENT,
				test::ImageFormat::R32Float
			);
			CHECK(actual.Bytes == expected.Bytes);
			CHECK(renderer.PortalImageUsage().Uploads == uploaded.Uploads);
			CHECK(renderer.PortalImageUsage().UploadedBytes == uploaded.UploadedBytes);
		}
		rows[2].Frame.Position = originalPosition;
	}
	PortalImageCapture capture;
	capture.Producer = {PRODUCER.World, PRODUCER.Channel, PRODUCER.Session, PRODUCER.Generation};
	capture.Binding.World = view.World;
	capture.Binding.WorldName = view.WorldName;
	capture.Binding.Portal = core::Name(reply.Key.PortalKey);
	capture.Binding.Expected = reply.Key;
	capture.Binding.ExpectedScope = PortalImageScope::OpaqueLighting;
	capture.Binding.ExpectedProjection = PortalImageProjection::Eye;
	capture.Camera = camera;
	capture.Width = capture.Height = EXTENT;
	capture.CaptureLighting = lighting;
	PortalImageLayerSet layers;
	layers.Opaque = reply;
	REQUIRE(
		renderer.QueuePortalImageLayerSet(capture.Binding, std::move(layers), std::span(&capture.Image, 1))
	);
	REQUIRE(capture.Image != 0);
	// Upload the room group before requesting the production body compositor.
	renderer.Render(std::span(&view, 1), overlay, nullptr, false);
	REQUIRE(renderer.PortalImageLayerSetReady(capture.Image));
	// A valid map from another tick cannot inherit this retained eye image's identity.
	auto staleShadow = shadow;
	++staleShadow.Snapshot.CaptureTick;
	auto staleBinding = shadowBinding;
	staleBinding.ExpectedSnapshot = staleShadow.Snapshot;
	// Free the matching map temporarily so refusal is checked independently of the byte limit.
	REQUIRE(renderer.DropPortalShadowImage(handle));
	const auto staleHandle = queue(staleBinding, std::move(staleShadow));
	REQUIRE(staleHandle != 0);
	view.ImportedDirectionalShadow = staleHandle;
	CHECK(renderer.ComposePortalBodyImage(capture, view) == 0);
	REQUIRE(renderer.DropPortalShadowImage(staleHandle));
	const auto matchingHandle = queue(shadowBinding, PortalShadowImage(shadow));
	REQUIRE(matchingHandle != 0);
	view.ImportedDirectionalShadow = matchingHandle;
	const auto composed = renderer.ComposePortalBodyImage(capture, view);
	REQUIRE(composed != 0);
	const auto copied = renderer.QueueResourceImage(
		core::Name("portal-body-eye-image/ambient/shadow/layers-0/0"), core::Name("export")
	);
	REQUIRE(copied != 0);
	const auto repeated = renderer.ComposePortalBodyImage(capture, view);
	REQUIRE(repeated != 0);
	const auto actual = Take(renderer, copied);
	const float error = ColourError(native.Pixels, actual.Pixels);
	CAPTURE(error);
	CHECK(error < .002f);
	renderer.DropPortalImage(composed);
	if (repeated != composed) renderer.DropPortalImage(repeated);
	renderer.DropPortalImage(capture.Image);
	renderer.DropPortalShadowImage(matchingHandle);
	CHECK_FALSE(renderer.IsPortalShadowImageReady(matchingHandle));
	CHECK(renderer.PortalImageUsage().TextureBytes == 0);
	CHECK(renderer.PortalImageUsage().PendingCpuBytes == 0);
	if (packed) {
		auto subnormal = shadow;
		constexpr std::array<uint32_t, 5> boundaries{0, 1, 0x007fffffu, 0x00800000u, 0x3f800000u};
		for (uint32_t index = 0; index < PORTAL_SHADOW_BYTES / sizeof(float); ++index) {
			uint32_t bits = (index * 2654435761u) & 0x007fffffu;
			if (index % 64 < boundaries.size()) bits = boundaries[index % 64];
			for (size_t byte = 0; byte < sizeof(uint32_t); ++byte)
				subnormal.Depth[size_t(index) * sizeof(uint32_t) + byte] = std::byte(bits >> (8 * byte));
		}
		subnormal.Snapshot.DepthHash = assets::Hasher::Of(subnormal.Depth);
		auto binding = shadowBinding;
		binding.ExpectedSnapshot = subnormal.Snapshot;
		const auto expected = subnormal.Depth;
		const auto fallback = renderer.QueuePackedPortalShadowImage(binding, std::move(subnormal));
		REQUIRE(fallback != 0);
		CHECK(renderer.PortalImageUsage().TextureBytes == PORTAL_SHADOW_BYTES);
		view.Instances = {};
		view.ImportedDirectionalShadow = fallback;
		renderer.Render(std::span(&view, 1), overlay, nullptr, false);
		const auto actual = test::CaptureResource(
			renderer,
			core::Name("shadow"),
			0,
			PORTAL_SHADOW_EXTENT,
			PORTAL_SHADOW_EXTENT,
			test::ImageFormat::R32Float
		);
		CHECK(std::ranges::equal(actual.Bytes, expected));
		REQUIRE(renderer.DropPortalShadowImage(fallback));
		CHECK(renderer.PortalImageUsage().TextureBytes == 0);
		CHECK(renderer.PortalImageUsage().PendingCpuBytes == 0);
	}
}

TEST_CASE(
	"empty retained source preserves a non-origin native shadow domain", "[render][gpu][shadow-import][.]"
) {
	test::FixtureDevice fixture;
	fixture.Initialise();
	auto &renderer = fixture.Render;
	Install(renderer);
	const SceneTarget target{EXTENT, EXTENT};
	View source;
	source.World = 17;
	source.WorldName = core::Name(PRODUCER.World);
	source.Target = &target;
	source.Pipeline = PIPELINE;
	source.DirectionalShadowBounds = core::AABB{{10, 9, -20}, {11, 10, -19}};
	const std::array nodes{
		SHADOW, COLOUR_CAPTURE, COLOUR_CAPTURE, COLOUR_CAPTURE, COLOUR_CAPTURE, COLOUR_CAPTURE
	};
	std::array<uint64_t, 6> tokens{};
	REQUIRE(renderer.QueueResourceImages(PIPELINE, nodes, 0, ResourceImageDelivery::CopiedPixels, tokens));
	CHECK(renderer.QueueResourceImage(PIPELINE, COLOUR_CAPTURE, 0) == 0);
	OverlayImage overlay;
	REQUIRE(renderer.Render(std::span(&source, 1), overlay, nullptr, false).Ran(SHADOW));
	std::optional<std::vector<ResourceImage>> images;
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	while (!images && std::chrono::steady_clock::now() < deadline) {
		images = renderer.TakeResourceImages(tokens);
		if (!images) SDL_Delay(1);
	}
	REQUIRE(images);
	REQUIRE(images->size() == tokens.size());
	for (size_t index = 0; index < images->size(); ++index) {
		CHECK((*images)[index].Request.Token == tokens[index]);
		CHECK((*images)[index].Status == ResourceImageStatus::Ok);
		CHECK((*images)[index].CaptureFrame == images->front().CaptureFrame);
	}
	auto captured = std::move(images->front());
	REQUIRE(captured.Shadow);
	CHECK(captured.Shadow->SourceEmpty);
	CHECK(captured.Shadow->SourceBounds == core::AABB{});
	CHECK(captured.Shadow->DomainBounds == *source.DirectionalShadowBounds);
	core::ByteReader depth(captured.Depth);
	bool clear = true;
	while (!depth.AtEnd())
		clear = depth.ReadFloat() == 1.f && clear;
	CHECK(clear);
	for (size_t index = 2; index < images->size(); ++index) {
		CHECK((*images)[index].Width == EXTENT);
		CHECK((*images)[index].Height == EXTENT);
		CHECK(std::ranges::equal((*images)[index].Pixels, (*images)[1].Pixels));
		CHECK(std::ranges::equal((*images)[index].DirectionalResponse, (*images)[1].DirectionalResponse));
	}
	auto eye = Reply(std::move((*images)[1]), PortalCaptureLighting{});
	auto shadow = ShadowImage(std::move(captured), eye);
	REQUIRE(ValidPortalShadowImage(shadow));
	PortalShadowImageBinding binding{source.World, source.WorldName, shadow.Snapshot};
	const auto imported = renderer.QueuePortalShadowImage(binding, std::move(shadow));
	REQUIRE(imported != 0);
	CHECK(renderer.DropPortalShadowImage(imported));
}
