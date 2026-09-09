#include "RenderFixture.hpp"

#include <engine/assets/Mesh.hpp>
#include <engine/assets/Texture.hpp>
#include <engine/core/Bytes.hpp>
#include <engine/graph/PipelineDocument.hpp>
#include <engine/render/PortalImageRuntime.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/generators/catch_generators.hpp>
#include <glm/packing.hpp>

TEST_SUITE_ID("engine.render.portaldirectionalresponse")

namespace {
	using namespace engine;
	using namespace engine::render;
	constexpr uint32_t EXTENT = 65;
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

	std::vector<float> Samples(const std::vector<std::byte> &bytes) {
		core::ByteReader reader(bytes);
		std::vector<float> samples;
		while (!reader.AtEnd())
			samples.push_back(reader.ReadFloat());
		return samples;
	}
	std::vector<float> HalfSamples(const std::vector<std::byte> &bytes) {
		core::ByteReader reader(bytes);
		std::vector<float> samples;
		while (!reader.AtEnd()) {
			const auto pair = glm::unpackHalf2x16(reader.ReadUInt32());
			samples.push_back(pair.x);
			samples.push_back(pair.y);
		}
		return samples;
	}
	void Install(Renderer &renderer) {
		using namespace graph;
		PipelineDocument document;
		for (const char *name : {"lighting-baseline", "directional-response"})
			document.Record(
				{.Kind = EditKind::AddResource,
				 .Name = core::Name(name),
				 .Resource = ResourceKind::Colour,
				 .Format = ResourceFormat::RGBA32F}
			);
		const auto base = DefaultPbrDocument();
		PipelineDocument nativeDocument;
		bool deferredReached = false;
		// stop after deferred writes; later volumetrics reuses the native lit texture.
		for (const auto &edit : base.Edits()) {
			if (edit.Kind == EditKind::AddNode) {
				if (deferredReached) break;
				deferredReached = edit.Name == core::Name("deferred-lighting");
			}
			nativeDocument.Record(edit);
			document.Record(edit);
			if (edit.Kind == EditKind::Writes && edit.Target == core::Name("lit"))
				for (const char *name : {"lighting-baseline", "directional-response"})
					document.Record(
						{.Kind = EditKind::Writes, .Target = core::Name(name), .Key = core::Name(name)}
					);
		}

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
		for (const auto &[resource, port] :
			 {std::pair{"albedo", "albedo"},
			  {"normal", "normal"},
			  {"material", "material"},
			  {"linear-depth", "depth"},
			  {"occlusion", "occlusion"}})
			document.Record(
				{.Kind = EditKind::Reads, .Target = core::Name(resource), .Key = core::Name(port)}
			);
		document.Record(
			{.Kind = EditKind::Writes,
			 .Target = core::Name("ambient-response"),
			 .Key = core::Name("response")}
		);
		const auto correctionBase = document;
		// The readback oracle owns all six planes after the last native producer.
		// Declare their final consumer so the graph cannot alias a response plane.
		document.Record(
			{.Kind = EditKind::AddNode,
			 .Name = core::Name("retain-native"),
			 .NodeKind = core::Name("capture"),
			 .Scope = NodeScope::Frame}
		);
		for (const auto &[resource, port] :
			 {std::pair{"lit", "source"},
			  {"linear-depth", "depth"},
			  {"normal", "normal"},
			  {"ambient-response", "ambient-response"},
			  {"lighting-baseline", "lighting-baseline"},
			  {"directional-response", "directional-response"}})
			document.Record(
				{.Kind = EditKind::Reads, .Target = core::Name(resource), .Key = core::Name(port)}
			);
		RenderGraph graph;
		core::Name offender;
		REQUIRE(Build(document, graph, offender) == PipelineDocumentStatus::Ok);
		REQUIRE(renderer.SetPipeline(core::Name("directional-oracle"), graph));
		document = correctionBase;
		const std::array retainedNames{
			"colour", "depth", "normal", "ambient-response", "lighting-baseline", "directional-response"
		};
		const std::array retainedFormats{
			ResourceFormat::RGBA16F,
			ResourceFormat::R32F,
			ResourceFormat::RGB10A2,
			ResourceFormat::RGBA32F,
			ResourceFormat::RGBA32F,
			ResourceFormat::RGBA32F
		};
		for (size_t plane = 0; plane < retainedNames.size(); ++plane)
			document.Record(
				{.Kind = EditKind::AddResource,
				 .Name = core::Name(std::string("retained-") + retainedNames[plane]),
				 .Resource = ResourceKind::Colour,
				 .Format = retainedFormats[plane]}
			);
		document.Record(
			{.Kind = EditKind::AddResource,
			 .Name = core::Name("corrected"),
			 .Resource = ResourceKind::Colour,
			 .Format = ResourceFormat::RGBA16F}
		);
		document.Record(
			{.Kind = EditKind::AddNode,
			 .Name = core::Name("retained"),
			 .NodeKind = core::Name("eye-image"),
			 .Scope = NodeScope::View}
		);
		document.Record({.Kind = EditKind::Set, .Key = core::Name("scope"), .Value = "opaque-lighting"});
		for (const char *name : retainedNames)
			document.Record(
				{.Kind = EditKind::Writes,
				 .Target = core::Name(std::string("retained-") + name),
				 .Key = core::Name(name)}
			);
		document.Record(
			{.Kind = EditKind::AddNode,
			 .Name = core::Name("correct-retained"),
			 .NodeKind = core::Name("ambient-correct"),
			 .Scope = NodeScope::View}
		);
		for (const auto &[resource, port] :
			 {std::pair{"retained-lighting-baseline", "lighting-baseline"},
			  {"retained-ambient-response", "response"},
			  {"occlusion", "occlusion"},
			  {"retained-directional-response", "directional-response"},
			  {"retained-depth", "room-depth"},
			  {"retained-normal", "room-normal"},
			  {"shadow", "shadow"}})
			document.Record(
				{.Kind = EditKind::Reads, .Target = core::Name(resource), .Key = core::Name(port)}
			);
		document.Record(
			{.Kind = EditKind::Writes, .Target = core::Name("corrected"), .Key = core::Name("colour")}
		);
		RenderGraph correction;
		REQUIRE(Build(document, correction, offender) == PipelineDocumentStatus::Ok);
		REQUIRE(renderer.SetPipeline(core::Name("directional-correction"), correction));

		RenderGraph ordinary;
		REQUIRE(Build(nativeDocument, ordinary, offender) == PipelineDocumentStatus::Ok);
		REQUIRE(renderer.SetPipeline(core::Name("directional-ordinary"), ordinary));
	}
	PortalImageReply Retained(Renderer &renderer, const PortalCaptureLighting &lighting, uint64_t request) {
		PortalImageReply reply;
		reply.Key = {request, "directional-retained", 1, 1};
		reply.Status = PortalImageStatus::Ok;
		reply.Scope = PortalImageScope::OpaqueLighting;
		reply.CaptureLighting = lighting;
		reply.Width = reply.Height = EXTENT;
		reply.RowStride = EXTENT * 8;
		const std::array names{
			"lit", "linear-depth", "normal", "ambient-response", "lighting-baseline", "directional-response"
		};
		const std::array sizes{8u, 4u, 4u, 16u, 16u, 16u};
		const std::array planes{
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
		for (size_t plane = 0; plane < names.size(); ++plane) {
			*planes[plane] = ReadComposed(renderer, names[plane], sizes[plane]);
			*hashes[plane] = assets::Hasher::Of(*planes[plane]);
		}
		return reply;
	}
	void CheckCorrection(
		Renderer &renderer, View &view, PortalImageReply reply, const std::vector<float> &native
	) {
		PortalImageBinding binding;
		binding.World = view.World;
		binding.WorldName = view.WorldName;
		binding.ViewSlot = view.Slot;
		binding.Portal = core::Name(reply.Key.PortalKey);
		binding.Expected = reply.Key;
		binding.ExpectedScope = reply.Scope;
		binding.ExpectedProjection = PortalImageProjection::Eye;
		view.EyeImageKey = binding.Portal;
		view.EyeImage = renderer.QueuePortalImage(binding, std::move(reply));
		REQUIRE(view.EyeImage != 0);
		view.Pipeline = core::Name("directional-correction");
		OverlayImage overlay;
		REQUIRE(
			renderer.Render(std::span(&view, 1), overlay, nullptr, false).Ran(core::Name("correct-retained"))
		);
		const auto corrected = HalfSamples(ReadComposed(renderer, "corrected", 8));
		REQUIRE(corrected.size() == native.size());
		float maxError = 0;
		for (size_t sample = 0; sample < corrected.size(); ++sample) {
			REQUIRE(std::isfinite(corrected[sample]));
			maxError = std::max(maxError, std::abs(corrected[sample] - native[sample]));
		}
		CAPTURE(maxError);
		CHECK(maxError < .002f);
		renderer.DropPortalImage(view.EyeImage);
		view.EyeImage = 0;
		view.Pipeline = core::Name("directional-oracle");
	}

}

TEST_CASE(
	"native directional response restores changed shadow visibility", "[render][gpu][directional-response][.]"
) {
	const bool materialFog = GENERATE(false, true);
	CAPTURE(materialFog);
	test::FixtureDevice fixture;
	fixture.Initialise();
	auto &renderer = fixture.Render;
	Install(renderer);
	const core::Name planeName("directional-receiver");
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
	std::array<scene::DrawInstance, 2> rows;
	rows[0].Source = 1;
	rows[0].Mesh = planeName;
	rows[0].Frame.Position = {0, 0, -4.2f};
	rows[0].HalfExtent = {4, 4, .001f};
	rows[0].Tint = {.65f, .45f, .8f};
	// Keep the native map cleared even when the closed caster is disabled.
	// The front-culled receiver plane contributes no shadow depth itself.
	rows[0].CastShadow = true;
	rows[1].Source = 2;
	rows[1].Frame.Position = {.85f, 0, -3.87f};
	rows[1].HalfExtent = {.18f, 1.2f, .02f};
	rows[1].Tint = {.35f, .35f, .35f};
	rows[1].CastShadow = true;
	if (materialFog) {
		assets::TextureData texture;
		texture.Width = texture.Height = 1;
		texture.Format = assets::TextureFormat::RGBA8;
		texture.Pixels.assign(4, std::byte{255});
		const core::Name emission("directional-emission"), ao("directional-ao");
		REQUIRE(renderer.AddTexture(emission, texture));
		texture.Pixels[0] = std::byte{128};
		REQUIRE(renderer.AddTexture(ao, texture));
		for (auto &row : rows) {
			row.EmissiveMap = emission;
			row.EmissiveTint = {.2f, .6f, 1};
			row.EmissiveStrength = .3f;
			row.OcclusionMap = ao;
		}
	}
	SceneTarget target{EXTENT, EXTENT};
	View view;
	view.Target = &target;
	view.Pipeline = core::Name("directional-oracle");
	view.World = 17;
	view.WorldName = core::Name("directional-oracle");
	view.Instances = rows;
	PortalCaptureLighting lighting;
	lighting.Direction = {-.8f, 0, -.6f};
	lighting.Ambient = {.1f, .1f, .1f};
	lighting.OutdoorAmbient = lighting.Ambient;
	lighting.Direct = {8, 5, 3};
	if (materialFog) {
		lighting.FogColour = {.03f, .07f, .11f};
		lighting.FogStart = 1;
		lighting.FogEnd = 16;
	}
	std::array<SceneLight, MAX_PORTAL_CAPTURE_LIGHTS> localLights;
	REQUIRE(ResolvePortalCaptureLighting(lighting, view, localLights));
	OverlayImage overlay;
	REQUIRE(
		renderer.Render(std::span(&view, 1), overlay, nullptr, false).Ran(core::Name("deferred-lighting"))
	);
	const auto shadowed = Samples(ReadComposed(renderer, "lighting-baseline", 16));
	const auto responseOn = Samples(ReadComposed(renderer, "directional-response", 16));
	const auto colourOn = HalfSamples(ReadComposed(renderer, "lit", 8));
	const auto retainedOn = Retained(renderer, lighting, 1);
	rows[1].CastShadow = false;
	REQUIRE(
		renderer.Render(std::span(&view, 1), overlay, nullptr, false).Ran(core::Name("deferred-lighting"))
	);
	const auto unshadowed = Samples(ReadComposed(renderer, "lighting-baseline", 16));
	const auto responseOff = Samples(ReadComposed(renderer, "directional-response", 16));
	const auto colourOff = HalfSamples(ReadComposed(renderer, "lit", 8));
	const auto retainedOff = Retained(renderer, lighting, 2);
	CheckCorrection(renderer, view, retainedOn, colourOff);
	rows[1].CastShadow = true;
	CheckCorrection(renderer, view, retainedOff, colourOn);
	REQUIRE(shadowed.size() == size_t(EXTENT) * EXTENT * 4);
	REQUIRE(unshadowed.size() == shadowed.size());
	REQUIRE(responseOn.size() == shadowed.size());
	REQUIRE(responseOff.size() == shadowed.size());
	size_t changedPixels = 0, fractionalPixels = 0, hdrPixels = 0;
	float maxRecoveryError = 0, maxResponseError = 0;
	for (size_t pixel = 0; pixel < shadowed.size(); pixel += 4) {
		const float visibility = responseOn[pixel + 3];
		REQUIRE(std::isfinite(visibility));
		CHECK(visibility >= 0);
		CHECK(visibility <= 1);
		CHECK(std::abs(visibility * 4 - std::round(visibility * 4)) < .0001f);
		CHECK(responseOff[pixel + 3] == 1);
		if (visibility > 0 && visibility < 1) ++fractionalPixels;
		float delta = 0;
		for (size_t channel = 0; channel < 3; ++channel) {
			const size_t sample = pixel + channel;
			REQUIRE(std::isfinite(shadowed[sample]));
			REQUIRE(std::isfinite(unshadowed[sample]));
			REQUIRE(std::isfinite(responseOn[sample]));
			REQUIRE(std::isfinite(responseOff[sample]));
			CHECK(responseOn[sample] >= 0);
			maxResponseError = std::max(maxResponseError, std::abs(responseOn[sample] - responseOff[sample]));
			const float correction = responseOn[sample] * (1 - visibility);
			maxRecoveryError =
				std::max(maxRecoveryError, std::abs(shadowed[sample] + correction - unshadowed[sample]));
			maxRecoveryError =
				std::max(maxRecoveryError, std::abs(unshadowed[sample] - correction - shadowed[sample]));
			delta = std::max(delta, std::abs(unshadowed[sample] - shadowed[sample]));
		}
		if (delta > .002f) ++changedPixels;
		if (unshadowed[pixel] > 1) ++hdrPixels;
	}
	CAPTURE(changedPixels, fractionalPixels, hdrPixels, maxRecoveryError, maxResponseError);
	CHECK(changedPixels > 0);
	CHECK(hdrPixels > 0);
	CHECK(maxResponseError < .0001f);
	CHECK(maxRecoveryError < .002f);

	view.Pipeline = core::Name("directional-ordinary");
	for (const bool cast : {false, true}) {
		CAPTURE(cast);
		rows[1].CastShadow = cast;
		REQUIRE(
			renderer.Render(std::span(&view, 1), overlay, nullptr, false).Ran(core::Name("deferred-lighting"))
		);
		const auto ordinary = HalfSamples(ReadComposed(renderer, "lit", 8));
		const auto &exported = cast ? colourOn : colourOff;
		const auto &baseline = cast ? shadowed : unshadowed;
		REQUIRE(ordinary.size() == baseline.size());
		REQUIRE(exported.size() == ordinary.size());
		float colourError = 0, baselineError = 0;
		for (size_t sample = 0; sample < ordinary.size(); ++sample) {
			REQUIRE(std::isfinite(ordinary[sample]));
			colourError = std::max(colourError, std::abs(ordinary[sample] - exported[sample]));
			baselineError = std::max(baselineError, std::abs(ordinary[sample] - baseline[sample]));
		}
		CAPTURE(colourError, baselineError);
		CHECK(colourError < .002f);
		CHECK(baselineError < .002f);
	}
	view.Pipeline = core::Name("directional-oracle");
	if (!materialFog) {
		bool foundFractional = false;
		for (size_t phase = 1; phase <= 32 && !foundFractional; ++phase) {
			view.CameraFrame.Position.X = static_cast<float>(phase) * .004f;
			rows[1].CastShadow = true;
			REQUIRE(renderer.Render(std::span(&view, 1), overlay, nullptr, false)
						.Ran(core::Name("deferred-lighting")));
			const auto phaseResponse = Samples(ReadComposed(renderer, "directional-response", 16));
			size_t partialCount = 0;
			for (size_t pixel = 0; pixel < phaseResponse.size(); pixel += 4)
				if (phaseResponse[pixel + 3] > 0 && phaseResponse[pixel + 3] < 1) ++partialCount;
			if (partialCount == 0) continue;
			foundFractional = true;
			const auto phaseShadowed = Samples(ReadComposed(renderer, "lighting-baseline", 16));
			const auto phaseRetained = Retained(renderer, lighting, 3);
			rows[1].CastShadow = false;
			REQUIRE(renderer.Render(std::span(&view, 1), overlay, nullptr, false)
						.Ran(core::Name("deferred-lighting")));
			const auto phaseUnshadowed = Samples(ReadComposed(renderer, "lighting-baseline", 16));
			CheckCorrection(renderer, view, phaseRetained, HalfSamples(ReadComposed(renderer, "lit", 8)));
			float partialRecoveryError = 0;
			for (size_t pixel = 0; pixel < phaseResponse.size(); pixel += 4) {
				const float visibility = phaseResponse[pixel + 3];
				if (visibility <= 0 || visibility >= 1) continue;
				CHECK(std::abs(visibility * 4 - std::round(visibility * 4)) < .0001f);
				for (size_t channel = 0; channel < 3; ++channel) {
					const size_t sample = pixel + channel;
					const float restored = phaseShadowed[sample] + phaseResponse[sample] * (1 - visibility);
					REQUIRE(std::isfinite(restored));
					REQUIRE(std::isfinite(phaseUnshadowed[sample]));
					partialRecoveryError =
						std::max(partialRecoveryError, std::abs(restored - phaseUnshadowed[sample]));
				}
			}
			CAPTURE(phase, partialCount, partialRecoveryError);
			CHECK(partialRecoveryError < .002f);
		}
		CHECK(foundFractional);
	}
	view.CameraFrame = {};
	rows[1].CastShadow = false;
	view.Instances = {};
	REQUIRE(
		renderer.Render(std::span(&view, 1), overlay, nullptr, false).Ran(core::Name("deferred-lighting"))
	);
	const auto empty = Samples(ReadComposed(renderer, "directional-response", 16));
	for (size_t pixel = 0; pixel < empty.size(); pixel += 4) {
		CHECK(empty[pixel] == 0);
		CHECK(empty[pixel + 1] == 0);
		CHECK(empty[pixel + 2] == 0);
		CHECK(empty[pixel + 3] == 1);
	}
}

TEST_CASE(
	"directional response extent validation follows colour in either output order",
	"[render][gpu][directional-response-extent][.]"
) {
	using namespace engine;
	using namespace engine::render;
	using namespace engine::graph;
	const bool responseFirst = GENERATE(false, true);
	const bool matchingExtent = GENERATE(false, true);
	CAPTURE(responseFirst, matchingExtent);
	test::FixtureDevice fixture;
	fixture.Initialise();
	PipelineDocument document;
	for (const char *name : {"lighting-baseline", "directional-response"}) {
		const bool response = core::Name(name) == core::Name("directional-response");
		document.Record(
			{.Kind = EditKind::AddResource,
			 .Name = core::Name(name),
			 .Resource = ResourceKind::Colour,
			 .Format = ResourceFormat::RGBA32F,
			 .Width = response && !matchingExtent ? 33u : 65u,
			 .Height = response && !matchingExtent ? 19u : 37u}
		);
	}
	const Edit responseWrite{
		.Kind = EditKind::Writes,
		.Target = core::Name("directional-response"),
		.Key = core::Name("directional-response")
	};
	const auto base = DefaultPbrDocument();
	for (auto edit : base.Edits()) {
		if (edit.Kind == EditKind::AddResource && edit.Name == core::Name("lit")) {
			edit.Width = 65;
			edit.Height = 37;
		}
		const bool colourWrite = edit.Kind == EditKind::Writes && edit.Target == core::Name("lit");
		if (colourWrite && responseFirst) document.Record(responseWrite);
		document.Record(edit);
		if (colourWrite) {
			document.Record(
				{.Kind = EditKind::Writes,
				 .Target = core::Name("lighting-baseline"),
				 .Key = core::Name("lighting-baseline")}
			);
			if (!responseFirst) document.Record(responseWrite);
		}
	}
	RenderGraph pipeline;
	core::Name offender;
	REQUIRE(Build(document, pipeline, offender) == PipelineDocumentStatus::Ok);
	const core::Name pipelineName("directional-extent");
	REQUIRE(fixture.Render.SetPipeline(pipelineName, pipeline));
	SceneTarget target{65, 37};
	View view;
	view.Pipeline = pipelineName;
	view.Target = &target;
	OverlayImage overlay;
	const auto report = fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false);
	CHECK(report.Ran(core::Name("deferred-lighting")) == matchingExtent);
}

TEST_CASE(
	"directional response requires paired distinct outputs", "[render][gpu][directional-response-extent][.]"
) {
	using namespace engine;
	using namespace engine::graph;
	const bool includeBaseline = GENERATE(false, true);
	CAPTURE(includeBaseline);
	render::test::FixtureDevice fixture;
	fixture.Initialise();
	PipelineDocument document;
	document.Record(
		{.Kind = EditKind::AddResource,
		 .Name = core::Name("shared-response"),
		 .Resource = ResourceKind::Colour,
		 .Format = ResourceFormat::RGBA32F}
	);
	const auto base = DefaultPbrDocument();
	for (const auto &edit : base.Edits()) {
		document.Record(edit);
		if (edit.Kind == EditKind::Writes && edit.Target == core::Name("lit"))
			for (const char *port : {"lighting-baseline", "directional-response"}) {
				if (!includeBaseline && core::Name(port) == core::Name("lighting-baseline")) continue;
				document.Record(
					{.Kind = EditKind::Writes,
					 .Target = core::Name("shared-response"),
					 .Key = core::Name(port)}
				);
			}
	}
	RenderGraph graph;
	core::Name offender;
	REQUIRE(Build(document, graph, offender) == PipelineDocumentStatus::Ok);
	CHECK_FALSE(fixture.Render.SetPipeline(core::Name("directional-shared-response"), graph));
}
