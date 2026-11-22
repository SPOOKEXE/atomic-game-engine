// Shared helpers for the ResourceImage test files: graph image capture
// installs, image await loops, the forced G-buffer failure guard and the body
// aperture comparison.
#pragma once

#include "../RenderFixture.hpp"
#include "../portal/PortalRendererTerminalTrace.hpp"
#include "DisplayColour.hpp"
#include "RendererTestHooks.hpp"
#include "capture/AmbientOcclusionCapture.hpp"
#include "capture/DataCaptureCompact.hpp"
#include "portal/PortalResidentImages.hpp"

#include <engine/core/Bytes.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/core/Paths.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/graph/PipelineCatalogue.hpp>
#include <engine/graph/PipelineDocument.hpp>
#include <engine/gui/Registration.hpp>
#include <engine/render/capture/DataFactoryHookBind.hpp>
#include <engine/render/capture/ScriptDataCaptureBridge.hpp>
#include <engine/render/interface/InterfacePass.hpp>
#include <engine/render/portal/PortalGeometryDraw.hpp>
#include <engine/render/portal/PortalImageImport.hpp>
#include <engine/render/portal/PortalImageRuntime.hpp>
#include <engine/render/presentation/WorldPresentation.hpp>
#include <engine/render/presentation/WorldView.hpp>
#include <engine/render/resources/EditableMeshes.hpp>
#include <engine/render/resources/ResourceImage.hpp>
#include <engine/render/resources/ShaderCompiler.hpp>
#include <engine/scene/Interpolation.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scene/cameras/ActiveCamera.hpp>
#include <engine/scene/cameras/Visibility.hpp>
#include <engine/scene/characters/Characters.hpp>
#include <engine/scene/characters/Controls.hpp>
#include <engine/scene/editable/EditableMesh.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/DataFactory.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <glm/packing.hpp>

#include <algorithm>
#include <array>
#include <cfenv>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <string>

namespace resource_image_test {
	using namespace engine;

	struct ForcedGBufferFailure {
		ForcedGBufferFailure() {
			render::test_support::SetForceGBufferPipelineFailure(true);
		}

		~ForcedGBufferFailure() {
			render::test_support::SetForceGBufferPipelineFailure(false);
		}
	};

	inline void InstallImageCapture(
		render::Renderer &renderer,
		std::string_view resource = "lit",
		bool depth = false,
		bool ambient = false,
		bool directional = false,
		std::string_view pipelineName = "image-export-pipeline"
	) {
		graph::PipelineDocument document, frameTail;
		if (directional)
			document.Record(
				{.Kind = graph::EditKind::AddResource,
				 .Name = core::Name("directional-response"),
				 .Resource = graph::ResourceKind::Colour,
				 .Format = graph::ResourceFormat::RGBA32F}
			);
		if (ambient)
			document.Record(
				{.Kind = graph::EditKind::AddResource,
				 .Name = core::Name("lighting-baseline"),
				 .Resource = graph::ResourceKind::Colour,
				 .Format = graph::ResourceFormat::RGBA32F}
			);
		const auto base = graph::DefaultPbrDocument();
		bool shared = false;
		for (const auto &edit : base.Edits()) {
			if (edit.Kind == graph::EditKind::AddNode)
				shared = ambient && edit.Scope == graph::NodeScope::Frame;
			(shared ? frameTail : document).Record(edit);
			if (directional && edit.Kind == graph::EditKind::Writes && edit.Target == core::Name("lit"))
				document.Record(
					{.Kind = graph::EditKind::Writes,
					 .Target = core::Name("directional-response"),
					 .Key = core::Name("directional-response")}
				);
			if (ambient && edit.Kind == graph::EditKind::Writes && edit.Target == core::Name("lit"))
				document.Record(
					{.Kind = graph::EditKind::Writes,
					 .Target = core::Name("lighting-baseline"),
					 .Key = core::Name("lighting-baseline")}
				);
		}
		if (ambient) {
			document.Record(
				{.Kind = graph::EditKind::AddResource,
				 .Name = core::Name("ambient-response"),
				 .Resource = graph::ResourceKind::Colour,
				 .Format = graph::ResourceFormat::RGBA32F}
			);
			document.Record(
				{.Kind = graph::EditKind::AddNode,
				 .Name = core::Name("ambient-response"),
				 .NodeKind = core::Name("ambient-response")}
			);
			for (const auto &[port, input] : std::array<std::pair<const char *, const char *>, 5>{
					 {{"albedo", "albedo"},
					  {"normal", "normal"},
					  {"material", "material"},
					  {"depth", "linear-depth"},
					  {"occlusion", "occlusion"}}
				 })
				document.Record(
					{.Kind = graph::EditKind::Reads, .Target = core::Name(input), .Key = core::Name(port)}
				);
			document.Record(
				{.Kind = graph::EditKind::Writes,
				 .Target = core::Name("ambient-response"),
				 .Key = core::Name("response")}
			);
		}
		for (const auto &edit : frameTail.Edits())
			document.Record(edit);
		document.Record(
			{.Kind = graph::EditKind::AddNode,
			 .Name = core::Name("image-export"),
			 .NodeKind = core::Name("capture"),
			 .Scope = graph::NodeScope::Frame}
		);
		document.Record(
			{.Kind = graph::EditKind::Reads, .Target = core::Name(resource), .Key = core::Name("source")}
		);
		if (depth)
			document.Record(
				{.Kind = graph::EditKind::Reads,
				 .Target = core::Name("linear-depth"),
				 .Key = core::Name("depth")}
			);
		if (ambient)
			for (const char *resource : {"normal", "ambient-response", "lighting-baseline"})
				document.Record(
					{.Kind = graph::EditKind::Reads,
					 .Target = core::Name(resource),
					 .Key = core::Name(resource)}
				);
		if (directional)
			document.Record(
				{.Kind = graph::EditKind::Reads,
				 .Target = core::Name("directional-response"),
				 .Key = core::Name("directional-response")}
			);
		document.Record(
			{.Kind = graph::EditKind::AddNode,
			 .Name = core::Name("image-export-object-ids"),
			 .NodeKind = core::Name("capture"),
			 .Scope = graph::NodeScope::Frame}
		);
		document.Record(
			{.Kind = graph::EditKind::Reads, .Target = core::Name("object-ids"), .Key = core::Name("source")}
		);
		for (const auto &[node, resource] : std::array<std::pair<const char *, const char *>, 2>{
				 {{"image-export-semantic-ids", "semantic-ids"}, {"image-export-part-ids", "part-ids"}}
			 }) {
			document.Record(
				{.Kind = graph::EditKind::AddNode,
				 .Name = core::Name(node),
				 .NodeKind = core::Name("capture"),
				 .Scope = graph::NodeScope::Frame}
			);
			document.Record(
				{.Kind = graph::EditKind::Reads, .Target = core::Name(resource), .Key = core::Name("source")}
			);
		}
		graph::RenderGraph pipeline;
		core::Name offender;
		REQUIRE(graph::Build(document, pipeline, offender) == graph::PipelineDocumentStatus::Ok);
		REQUIRE(renderer.SetPipeline(core::Name(pipelineName), pipeline));
	}

	inline void InstallFailedImageCapture(render::Renderer &renderer) {
		const core::Name failure("fixture-after-image-export");
		graph::NodeKindSpec spec;
		spec.Kind = failure;
		spec.Scope = graph::NodeScope::Frame;
		spec.Queue = graph::ExecutionQueue::Cpu;
		spec.Inputs.push_back({.Name = core::Name("source"), .Kind = graph::ResourceKind::Texture});
		REQUIRE(graph::RegisterNodeKind(std::move(spec)));
		REQUIRE(renderer.InstallNodeHandler(failure, [](const graph::RunContext &) { return false; }));
		graph::PipelineDocument document = graph::DefaultPbrDocument();
		for (const core::Name name : {core::Name("image-export"), failure}) {
			document.Record(
				{.Kind = graph::EditKind::AddNode,
				 .Name = name,
				 .NodeKind = name == failure ? failure : core::Name("capture"),
				 .Scope = graph::NodeScope::Frame}
			);
			document.Record(
				{.Kind = graph::EditKind::Reads, .Target = core::Name("lit"), .Key = core::Name("source")}
			);
		}
		graph::RenderGraph pipeline;
		core::Name offender;
		REQUIRE(graph::Build(document, pipeline, offender) == graph::PipelineDocumentStatus::Ok);
		REQUIRE(renderer.SetPipeline(core::Name("image-export-pipeline"), pipeline));
	}

	inline std::vector<render::ResourceImage> AwaitImages(render::Renderer &renderer, size_t wanted) {
		std::vector<render::ResourceImage> images;
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while (images.size() < wanted && std::chrono::steady_clock::now() < deadline) {
			for (auto &image : renderer.TakeResourceImages()) {
				images.push_back(std::move(image));
			}
			if (images.size() < wanted) {
				SDL_Delay(1);
			}
		}
		REQUIRE(images.size() == wanted);
		return images;
	}

	inline std::vector<render::ResourceImage>
	AwaitImageGroup(render::Renderer &renderer, std::span<const uint64_t> tokens) {
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while (std::chrono::steady_clock::now() < deadline) {
			if (auto images = renderer.TakeResourceImages(tokens)) return std::move(*images);
			SDL_Delay(1);
		}
		FAIL("resource image group did not complete");
		return {};
	}

	inline render::ResourceImage AwaitImage(render::Renderer &renderer, uint64_t token) {
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while (std::chrono::steady_clock::now() < deadline) {
			if (auto image = renderer.TakeResourceImage(token)) {
				return std::move(*image);
			}
			SDL_Delay(1);
		}
		FAIL("resource image fence did not complete");
		return {};
	}

	inline void CheckBodyAperture(
		render::Renderer &renderer,
		render::View view,
		std::span<const scene::DrawInstance> bodyAndWall,
		bool oblique,
		float scale,
		bool resident
	) {
		using namespace graph;
		const bool firstPerson = view.EyeRig != 0;
		CAPTURE(oblique, firstPerson, scale, resident);
		const auto sourceView = view;
		auto document = DefaultWorldHdrDocument();
		document.Record(
			{.Kind = EditKind::AddNode,
			 .Name = core::Name("aperture-export"),
			 .NodeKind = core::Name("capture"),
			 .Scope = NodeScope::Frame}
		);
		document.Record(
			{.Kind = EditKind::Reads, .Target = core::Name("lens-b"), .Key = core::Name("source")}
		);
		RenderGraph pipeline;
		core::Name offender;
		REQUIRE(Build(document, pipeline, offender) == PipelineDocumentStatus::Ok);
		REQUIRE(renderer.SetPipeline(core::Name("body-aperture"), pipeline));
		assets::MeshData plane;
		plane.Vertices = {
			{{-1, -1, 0}, {0, 0, 1}, {0, 1}},
			{{1, -1, 0}, {0, 0, 1}, {1, 1}},
			{{1, 1, 0}, {0, 0, 1}, {1, 0}},
			{{-1, 1, 0}, {0, 0, 1}, {0, 0}}
		};
		plane.Indices = {0, 1, 2, 0, 2, 3};
		plane.ComputeBounds();
		const core::Name mesh("body-aperture-plane");
		REQUIRE(renderer.AddMesh(mesh, plane));
		const auto mouth = view.CameraFrame * core::CFrame(core::Vector3{.35f, 0, -2}) *
						   core::CFrame::Angles(0, oblique ? .4f : 0, 0);
		constexpr float halfWidth = .6f, halfHeight = .5f;
		const auto normal = mouth.VectorToWorldSpace({0, 0, 1});
		const float offset = normal.Dot(mouth.Position);
		std::vector<scene::DrawInstance> body(bodyAndWall.begin(), bodyAndWall.end() - 1);
		for (auto &row : body) {
			row.SeamNormal = {};
			row.SeamOffset = 0;
		}
		render::PortalBodyDraws split;
		const auto sourceJoints = view.JointFrames;
		scene::SeamTransform through;
		through.Frame = core::CFrame(core::Vector3{7, -3, 11}) * core::CFrame::Angles(.2f, .7f, -.3f);
		through.Origin = {2, 1, -4};
		through.Scale = scale;
		REQUIRE(render::SplitPortalBodyDraws(body, view.JointFrames, through, normal, offset, split));
		const auto cameraFrame = through.Place(sourceView.CameraFrame);
		const auto cameraRotation = cameraFrame.Rotation();
		render::PortalCaptureCamera camera;
		camera.Position = {cameraFrame.Position.X, cameraFrame.Position.Y, cameraFrame.Position.Z};
		camera.Orientation = {cameraRotation.x, cameraRotation.y, cameraRotation.z, cameraRotation.w};
		const float near = sourceView.Camera.NearPlane * scale;
		const float vertical = std::tan(sourceView.Camera.FieldOfViewRadians / 2) * near;
		const float horizontal = vertical * 65 / 37;
		camera.Frustum = {
			-horizontal, horizontal, -vertical, vertical, near, sourceView.Camera.FarPlane * scale
		};
		const auto farNormal = through.Rotate(-normal);
		camera.ClipPlane = {
			farNormal.X, farNormal.Y, farNormal.Z, -farNormal.Dot(through.Point(mouth.Position))
		};
		REQUIRE(render::ResolvePortalCaptureCamera(camera, render::PortalImageProjection::Seam, view));
		const auto sampling = scene::ResolveSurfaceCamera(view.CameraFrame, *view.Projection).ViewProjection *
							  scene::SeamMatrix(through);
		auto wall = bodyAndWall.back();
		wall.Frame = through.Place(wall.Frame);
		wall.HalfExtent = wall.HalfExtent * scale;
		view.Pipeline = core::Name("body-reference");
		view.Instances = std::span(&wall, 1);
		view.EyeImage = 0;
		const auto residentToken =
			resident ? renderer.QueueResourceImage(
						   view.Pipeline, core::Name("export"), 0, render::ResourceImageDelivery::Resident
					   )
					 : 0;
		if (resident) REQUIRE(residentToken != 0);
		const auto roomToken = renderer.QueueResourceImage(view.Pipeline, core::Name("export"));
		REQUIRE(roomToken != 0);
		render::OverlayImage bodyOverlay;
		REQUIRE(renderer.Render(std::span(&view, 1), bodyOverlay, nullptr, false).Ran(core::Name("export")));
		const auto room = AwaitImage(renderer, roomToken);
		render::PortalImageBinding roomBinding;
		roomBinding.World = view.World;
		roomBinding.WorldName = view.WorldName;
		roomBinding.Portal = core::Name("mapped-body-room");
		roomBinding.Expected = {1, "mapped-body-room", 1, 1};
		roomBinding.ExpectedProjection = render::PortalImageProjection::Seam;
		roomBinding.Sampling = sampling;
		render::PortalImageReply roomReply;
		roomReply.Key = roomBinding.Expected;
		roomReply.Status = render::PortalImageStatus::Ok;
		roomReply.Width = room.Width;
		roomReply.Height = room.Height;
		roomReply.RowStride = room.RowStride;
		roomReply.Pixels = room.Pixels;
		roomReply.Depth = room.Depth;
		roomReply.PixelHash = assets::Hasher::Of(roomReply.Pixels);
		roomReply.DepthHash = assets::Hasher::Of(roomReply.Depth);
		const auto uploads = renderer.PortalImageUsage().Uploads;
		const auto roomImage = resident ? renderer.AdoptResourceImage(residentToken, roomBinding)
										: renderer.QueuePortalImage(roomBinding, std::move(roomReply));
		REQUIRE(roomImage != 0);
		view.EyeImage = roomImage;
		view.EyeImageKey = roomBinding.Portal;
		view.JointFrames = split.Joints;
		view.Pipeline = core::Name("body-compose");
		view.Instances = split.Far;
		CHECK_FALSE(
			renderer.Render(std::span(&view, 1), bodyOverlay, nullptr, false).Ran(core::Name("body-compose"))
		);
		CHECK(renderer.PortalImageUsage().Uploads == uploads + (resident ? 0 : 2));
		view.Pipeline = core::Name("body-seam-compose");
		const auto composedResident =
			resident ? renderer.QueueResourceImage(
						   view.Pipeline, core::Name("export"), 0, render::ResourceImageDelivery::Resident
					   )
					 : 0;
		if (resident) REQUIRE(composedResident != 0);
		const auto composedToken = renderer.QueueResourceImage(view.Pipeline, core::Name("export"));
		REQUIRE(composedToken != 0);
		REQUIRE(renderer.Render(std::span(&view, 1), bodyOverlay, nullptr, false).Ran(core::Name("export")));
		const auto composed = AwaitImage(renderer, composedToken);
		view = sourceView;
		view.EyeImage = 0;
		view.Pipeline = core::Name("body-aperture");
		std::vector<scene::DrawInstance> frame;
		const auto panel = [&](float x, float y, float z, float width, float height) {
			scene::DrawInstance row;
			row.Source = 1000 + frame.size();
			row.Frame = mouth * core::CFrame(core::Vector3{x, y, z});
			row.HalfExtent = {width, height, .001f};
			row.Mesh = mesh;
			row.CastShadow = false;
			row.EmissiveMap = core::Name("body-emission");
			row.EmissiveTint = {0, .25f, 0};
			row.EmissiveStrength = 1.0f;
			frame.push_back(row);
		};
		panel(-10 - halfWidth, 0, 0, 10, 20);
		panel(10 + halfWidth, 0, 0, 10, 20);
		panel(0, -10 - halfHeight, 0, halfWidth, 10);
		panel(0, 10 + halfHeight, 0, halfWidth, 10);
		// A nearer strip crosses the opening and must occlude both imported layers.
		panel(-.15f, 0, .2f, .05f, halfHeight);
		auto directRows = frame;
		directRows.insert(directRows.end(), bodyAndWall.begin(), bodyAndWall.end());
		for (auto &row : directRows) {
			row.SeamNormal = {};
			row.SeamOffset = 0;
		}
		const auto capture = [&](std::span<const scene::DrawInstance> rows) {
			view.Instances = rows;
			const auto token = renderer.QueueResourceImage(view.Pipeline, core::Name("aperture-export"));
			REQUIRE(token != 0);
			render::OverlayImage overlay;
			REQUIRE(renderer.Render(std::span(&view, 1), overlay, nullptr, false)
						.Ran(core::Name("aperture-export")));
			return AwaitImage(renderer, token);
		};
		view.JointFrames = sourceJoints;
		const auto direct = capture(directRows);
		view.JointFrames = split.Joints;
		render::PortalImageBinding binding;
		binding.World = view.World;
		binding.WorldName = view.WorldName;
		binding.Portal = core::Name("body-aperture");
		binding.Expected = {1, "body-aperture", 1, 1};
		binding.Sampling = sampling;
		render::PortalImageReply reply;
		reply.Key = binding.Expected;
		reply.Status = render::PortalImageStatus::Ok;
		reply.Width = composed.Width;
		reply.Height = composed.Height;
		reply.RowStride = composed.RowStride;
		reply.Pixels = composed.Pixels;
		reply.PixelHash = assets::Hasher::Of(reply.Pixels);
		render::PortalView portal;
		portal.ExternalImage = true;
		portal.ImagePortal = binding.Portal;
		const auto composedUploads = renderer.PortalImageUsage().Uploads;
		portal.ImportedImage = resident ? renderer.AdoptResourceImage(composedResident, binding)
										: renderer.QueuePortalImage(binding, std::move(reply));
		REQUIRE(portal.ImportedImage != 0);
		portal.Centre = mouth.Position;
		portal.Normal = mouth.VectorToWorldSpace({0, 0, 1});
		portal.First = mouth.VectorToWorldSpace({halfWidth, 0, 0});
		portal.Second = mouth.VectorToWorldSpace({0, halfHeight, 0});
		panel(0, 0, 0, halfWidth, halfHeight);
		frame.back().Surface = 0;
		frame.insert(frame.end(), split.Near.begin(), split.Near.end());
		view.Portals = std::span(&portal, 1);
		const auto actual = capture(frame);
		CHECK(renderer.PortalImageUsage().Uploads == composedUploads + (resident ? 0 : 1));
		const auto unpack = [](const render::ResourceImage &image) {
			std::vector<glm::vec4> pixels;
			core::ByteReader reader(image.Pixels);
			while (!reader.AtEnd()) {
				const auto rg = glm::unpackHalf2x16(reader.ReadUInt32());
				const auto ba = glm::unpackHalf2x16(reader.ReadUInt32());
				pixels.emplace_back(rg, ba);
			}
			return pixels;
		};
		const auto expectedPixels = unpack(direct), actualPixels = unpack(actual);
		size_t bodyInside = 0, bodyOutside = 0, wallPixels = 0, framePixels = 0;
		const float tangent = std::tan(view.Camera.FieldOfViewRadians / 2);
		for (uint32_t y = 0; y < actual.Height; ++y) {
			for (uint32_t x = 0; x < actual.Width; ++x) {
				const auto &pixel = actualPixels[size_t(y) * actual.Width + x];
				wallPixels += pixel.r > .5f && pixel.b < .1f;
				framePixels += pixel.g > .2f && pixel.b < .1f;
				if (pixel.b < .5f || pixel.r > .1f) continue;
				const auto ray = view.CameraFrame.VectorToWorldSpace(
					{(2 * (x + .5f) / actual.Width - 1) * tangent * actual.Width / actual.Height,
					 (1 - 2 * (y + .5f) / actual.Height) * tangent,
					 -1}
				);
				const float distance = (offset - normal.Dot(view.CameraFrame.Position)) / normal.Dot(ray);
				const auto hit = mouth.PointToObjectSpace(view.CameraFrame.Position + ray * distance);
				if (distance > 0 && std::abs(hit.X) < halfWidth && std::abs(hit.Y) < halfHeight)
					++bodyInside;
				else
					++bodyOutside;
			}
		}
		CHECK(wallPixels > 0);
		CHECK(framePixels > 0);
		if (firstPerson) {
			CHECK(bodyInside == 0);
			CHECK(bodyOutside == 0);
		} else {
			CHECK(bodyInside > 0);
			if (oblique) CHECK(bodyOutside > 0);
		}
		const render::test::ImageView expectedView{
			direct.Width,
			direct.Height,
			render::test::ImageFormat::Rgba32Float,
			std::as_bytes(std::span(expectedPixels)),
			0
		};
		const render::test::ImageView actualView{
			actual.Width,
			actual.Height,
			render::test::ImageFormat::Rgba32Float,
			std::as_bytes(std::span(actualPixels)),
			0
		};
		render::test::ImageTolerance tolerance;
		tolerance.Absolute = .002;
		render::test::CheckImage(
			renderer,
			"body-aperture",
			"radiance",
			"direct body through opening",
			expectedView,
			actualView,
			tolerance
		);
		if (const char *output = std::getenv("MONO_RENDER_PREVIEW_DIR")) {
			const auto directory = std::filesystem::path(output);
			std::filesystem::create_directories(directory);
			const std::string stem = std::string(oblique ? "body-aperture-oblique" : "body-aperture-front") +
									 (firstPerson ? "-first" : "-third") + "-" + std::to_string(scale);
			render::test::WriteImagePreview(directory / (stem + "-direct.ppm"), expectedView);
			render::test::WriteImagePreview(directory / (stem + "-composed.ppm"), actualView);
		}
		REQUIRE(renderer.DropPortalImage(portal.ImportedImage));
		REQUIRE(renderer.DropPortalImage(roomImage));
	}
}

using namespace resource_image_test;
