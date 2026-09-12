#include "RenderFixture.hpp"

#include <engine/core/Bytes.hpp>
#include <engine/core/Paths.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/graph/PipelineCatalogue.hpp>
#include <engine/graph/PipelineDocument.hpp>
#include <engine/render/PortalGeometryDraw.hpp>
#include <engine/render/PortalImageImport.hpp>
#include <engine/render/PortalImageRuntime.hpp>
#include <engine/render/PortalResidentImages.hpp>
#include <engine/render/ResourceImage.hpp>
#include <engine/render/ScriptDataCaptureBridge.hpp>
#include <engine/render/ShaderCompiler.hpp>
#include <engine/render/WorldPresentation.hpp>
#include <engine/render/WorldView.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/scene/Characters.hpp>
#include <engine/scene/Controls.hpp>
#include <engine/scene/Interpolation.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scene/Visibility.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/DataFactory.hpp>

#include <catch2/generators/catch_generators.hpp>
#include <glm/packing.hpp>

#include <array>

TEST_SUITE_ID("engine.render.resourceimage")

namespace {
	using namespace engine;

	void InstallImageCapture(
		render::Renderer &renderer,
		std::string_view resource = "lit",
		bool depth = false,
		bool ambient = false,
		bool directional = false
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
		graph::RenderGraph pipeline;
		core::Name offender;
		REQUIRE(graph::Build(document, pipeline, offender) == graph::PipelineDocumentStatus::Ok);
		REQUIRE(renderer.SetPipeline(core::Name("image-export-pipeline"), pipeline));
	}

	void InstallFailedImageCapture(render::Renderer &renderer) {
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

	std::vector<render::ResourceImage> AwaitImages(render::Renderer &renderer, size_t wanted) {
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

	std::vector<render::ResourceImage>
	AwaitImageGroup(render::Renderer &renderer, std::span<const uint64_t> tokens) {
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while (std::chrono::steady_clock::now() < deadline) {
			if (auto images = renderer.TakeResourceImages(tokens)) return std::move(*images);
			SDL_Delay(1);
		}
		FAIL("resource image group did not complete");
		return {};
	}

	render::ResourceImage AwaitImage(render::Renderer &renderer, uint64_t token) {
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

	void CheckBodyAperture(
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

TEST_CASE("resource image requests require a live device", "[render][resourceimage]") {
	engine::render::Renderer renderer;
	CHECK_FALSE(renderer.RequestResourceImage({.Token = 1, .Node = engine::core::Name("capture")}));
	CHECK(renderer.TakeResourceImages().empty());
	CHECK_FALSE(renderer.CancelResourceImage(1));
	CHECK_FALSE(renderer.TakeResourceImage(1));
	CHECK(renderer.QueueResourceImage({}, engine::core::Name("capture")) == 0);
}

TEST_CASE(
	"resident graph image adoption transfers ownership without readback", "[render][gpu][resourceimage][.]"
) {
	render::test::FixtureDevice fixture;
	fixture.Initialise();
	auto &renderer = fixture.Render;
	const int planeSet = GENERATE(0, 1, 2, 3);
	const bool pairedDepth = planeSet != 0;
	const bool retainedAmbient = planeSet >= 2;
	const bool retainedDirectional = planeSet == 3;
	const bool rotatedCamera = GENERATE(false, true);
	CAPTURE(planeSet, rotatedCamera);
	InstallImageCapture(renderer, "lit", pairedDepth, retainedAmbient, retainedDirectional);
	const auto token = renderer.QueueResourceImage(
		core::Name("image-export-pipeline"),
		core::Name("image-export"),
		0,
		render::ResourceImageDelivery::Resident
	);
	REQUIRE(token != 0);
	render::PortalImageBinding binding;
	binding.WorldName = core::Name("resident-source");
	if (retainedAmbient) binding.ExpectedScope = render::PortalImageScope::OpaqueLighting;
	binding.Portal = core::Name("Door");
	binding.Expected.RequestId = 1;
	binding.Expected.PortalKey = "Door";
	CHECK(renderer.AdoptResourceImage(token, binding) == 0);
	CHECK_FALSE(renderer.CanPublishResourceImage(token, 65, 37));
	render::SceneTarget target{65, 37};
	render::View view;
	view.Pipeline = core::Name("image-export-pipeline");
	view.Target = &target;
	if (rotatedCamera)
		view.CameraFrame =
			core::CFrame(core::Vector3{7, 2, 3}, core::CFrame::Angles(-.2f, .37f, .1f).Rotation());
	assets::MeshData plane;
	plane.Vertices = {
		{{-1, -1, 0}, {0, 0, 1}, {0, 1}},
		{{1, -1, 0}, {0, 0, 1}, {1, 1}},
		{{1, 1, 0}, {0, 0, 1}, {1, 0}},
		{{-1, 1, 0}, {0, 0, 1}, {0, 0}}
	};
	plane.Indices = {0, 1, 2, 0, 2, 3};
	plane.ComputeBounds();
	REQUIRE(renderer.AddMesh(core::Name("resident-plane"), plane));
	assets::TextureData white;
	white.Width = white.Height = 1;
	white.Format = assets::TextureFormat::RGBA8;
	white.Pixels.assign(4, std::byte{255});
	REQUIRE(renderer.AddTexture(core::Name("resident-white"), white));
	scene::DrawInstance wall;
	wall.Source = 1;
	wall.Frame = view.CameraFrame * core::CFrame(core::Vector3{0, 0, -4});
	wall.HalfExtent = {100, 100, .01f};
	wall.Mesh = core::Name("resident-plane");
	wall.Texture = core::Name("resident-white");
	wall.Tint = {.5f, .125f, .25f};
	wall.CastShadow = false;
	view.Instances = std::span(&wall, 1);
	view.OverrideLighting = true;
	view.Lighting.Ambient = {1, 1, 1};
	view.Lighting.OutdoorAmbient = {1, 1, 1};
	view.Lighting.Direct = {};
	const auto copiedToken =
		renderer.QueueResourceImage(core::Name("image-export-pipeline"), core::Name("image-export"));
	REQUIRE(copiedToken != 0);
	binding.Sampling = scene::ResolveCamera(view.CameraFrame, view.Camera, 65.0f / 37.0f).ViewProjection;
	render::OverlayImage overlay;
	REQUIRE(renderer.Render(std::span(&view, 1), overlay, nullptr, false).Ran(core::Name("image-export")));
	CHECK_FALSE(renderer.TakeResourceImage(token));
	CHECK_FALSE(renderer.TakeResourceImages(std::array{copiedToken, token}));
	CHECK(renderer.CanPublishResourceImage(token, 65, 37));
	CHECK_FALSE(renderer.CanPublishResourceImage(token, 37, 65));
	CHECK_FALSE(renderer.CanPublishResourceImage(copiedToken, 65, 37));
	auto invalid = binding;
	invalid.Expected.PortalKey = "Other";
	CHECK(renderer.AdoptResourceImage(token, invalid) == 0);
	const auto handle = renderer.AdoptResourceImage(token, binding);
	REQUIRE(handle != 0);
	CHECK_FALSE(renderer.CanPublishResourceImage(token, 65, 37));
	CHECK(renderer.AdoptResourceImage(token, binding) == 0);
	CHECK(renderer.PortalImageUsage().Images == 1);
	CHECK(
		renderer.PortalImageUsage().TextureBytes == 65 * 37 *
														(retainedDirectional ? 64
														 : retainedAmbient	 ? 48
														 : pairedDepth		 ? 12
																			 : 8)
	);
	CHECK(renderer.PortalImageUsage().UploadedBytes == 0);
	CHECK(renderer.PortalImageUsage().PendingCpuBytes == 0);
	CHECK(renderer.PortalImageUsage().StagingBytes == 0);
	const auto copied = AwaitImage(renderer, copiedToken);
	if (retainedDirectional) {
		CHECK(copied.DirectionalResponseResource == core::Name("directional-response"));
		REQUIRE(copied.DirectionalResponse.size() == 65 * 37 * 16);
		core::ByteReader samples(copied.DirectionalResponse);
		while (!samples.AtEnd()) {
			CHECK(samples.ReadFloat() == 0);
			CHECK(samples.ReadFloat() == 0);
			CHECK(samples.ReadFloat() == 0);
			const float visibility = samples.ReadFloat();
			CHECK(visibility >= 0);
			CHECK(visibility <= 1);
		}
	} else
		CHECK(copied.DirectionalResponse.empty());
	REQUIRE(copied.Status == render::ResourceImageStatus::Ok);
	if (pairedDepth) {
		CHECK(copied.DepthResource == core::Name("linear-depth"));
		REQUIRE(copied.Depth.size() == 65 * 37 * 4);
		core::ByteReader depths(copied.Depth);
		float minimum = 1000, maximum = 0;
		while (!depths.AtEnd()) {
			const float distance = depths.ReadFloat();
			REQUIRE(std::isfinite(distance));
			minimum = std::min(minimum, distance);
			maximum = std::max(maximum, distance);
		}
		CHECK(std::abs(minimum - 4.f) < .01f);
		CHECK(std::abs(maximum - 4.f) < .01f);
	} else {
		CHECK_FALSE(copied.DepthResource.IsValid());
		CHECK(copied.Depth.empty());
	}
	if (retainedAmbient) {
		CHECK(copied.NormalResource == core::Name("normal"));
		CHECK(copied.AmbientResponseResource == core::Name("ambient-response"));
		REQUIRE(copied.Normal.size() == 65 * 37 * 4);
		REQUIRE(copied.AmbientResponse.size() == 65 * 37 * 16);
		CHECK(copied.LightingBaselineResource == core::Name("lighting-baseline"));
		REQUIRE(copied.LightingBaseline.size() == 65 * 37 * 16);
		core::ByteReader baseline(copied.LightingBaseline), colour(copied.Pixels);
		while (!baseline.AtEnd()) {
			const float first = baseline.ReadFloat(), second = baseline.ReadFloat();
			REQUIRE(std::isfinite(first));
			REQUIRE(std::isfinite(second));
			const uint32_t packed = colour.ReadUInt32();
			// The observed attachment conversion and CPU packing differ by one half
			// step. Bound storage conversion locally without changing image parity.
			for (size_t channel = 0; channel < 2; ++channel) {
				const uint32_t half = (packed >> (channel * 16)) & 0xffffu;
				const float value = channel == 0 ? first : second;
				REQUIRE(value >= 0);
				REQUIRE(half < 0x7c00u);
				const float lower = glm::unpackHalf2x16(half == 0 ? 0 : half - 1).x;
				const float upper = glm::unpackHalf2x16(half + 1).x;
				CHECK(value >= lower);
				CHECK(value <= upper);
			}
		}
		core::ByteReader normals(copied.Normal);
		const auto expectedNormal = wall.Frame.VectorToWorldSpace({0, 0, 1});
		while (!normals.AtEnd()) {
			const uint32_t packed = normals.ReadUInt32();
			const float normalX = float(packed & 1023u) / 1023.f * 2 - 1;
			const float normalY = float((packed >> 10) & 1023u) / 1023.f * 2 - 1;
			const float normalZ = float((packed >> 20) & 1023u) / 1023.f * 2 - 1;
			CHECK(std::abs(normalX - expectedNormal.X) < .003f);
			CHECK(std::abs(normalY - expectedNormal.Y) < .003f);
			CHECK(std::abs(normalZ - expectedNormal.Z) < .003f);
		}
		core::ByteReader response(copied.AmbientResponse);
		float maximum = 0;
		while (!response.AtEnd()) {
			const float value = response.ReadFloat();
			REQUIRE(std::isfinite(value));
			maximum = std::max(maximum, value);
		}
		CHECK(maximum > 0);
	} else {
		CHECK(copied.Normal.empty());
		CHECK(copied.AmbientResponse.empty());
		CHECK(copied.LightingBaseline.empty());
	}
	render::PortalView portal;
	portal.ExternalImage = true;
	portal.ImagePortal = binding.Portal;
	portal.ImportedImage = handle;
	portal.Centre = wall.Frame.Position;
	portal.Normal = wall.Frame.VectorToWorldSpace({0, 0, 1});
	portal.First = wall.Frame.VectorToWorldSpace({2, 0, 0});
	portal.Second = wall.Frame.VectorToWorldSpace({0, 2, 0});
	wall.HalfExtent = {2, 2, .01f};
	wall.Surface = 0;
	wall.Tint = {.01f, .01f, .01f};
	view.WorldName = binding.WorldName;
	view.Portals = std::span(&portal, 1);
	// Adopted residency survives replacing the source graph with a retained display capture.
	InstallImageCapture(renderer, "tonemapped");
	REQUIRE(renderer.Render(std::span(&view, 1), overlay, nullptr, false).Ran(core::Name("image-export")));
	const auto residentPixels = render::test::CaptureResource(
		renderer, core::Name("tonemapped"), 0, 65, 37, render::test::ImageFormat::Rgba8Unorm
	);
	render::PortalImageReply reply;
	reply.Key = binding.Expected;
	reply.Scope = binding.ExpectedScope;
	reply.Status = render::PortalImageStatus::Ok;
	reply.Width = copied.Width;
	reply.Height = copied.Height;
	reply.RowStride = copied.RowStride;
	reply.Pixels = copied.Pixels;
	reply.PixelHash = assets::Hasher::Of(reply.Pixels);
	if (retainedAmbient) {
		reply.CaptureLighting.emplace();
		reply.Normal = copied.Normal;
		reply.AmbientResponse = copied.AmbientResponse;
		reply.LightingBaseline = copied.LightingBaseline;
		reply.NormalHash = assets::Hasher::Of(reply.Normal);
		reply.AmbientResponseHash = assets::Hasher::Of(reply.AmbientResponse);
		reply.LightingBaselineHash = assets::Hasher::Of(reply.LightingBaseline);
		if (retainedDirectional) {
			reply.DirectionalResponse = copied.DirectionalResponse;
			reply.DirectionalResponseHash = assets::Hasher::Of(reply.DirectionalResponse);
		}
	}
	if (pairedDepth) {
		reply.Depth = copied.Depth;
		reply.DepthHash = assets::Hasher::Of(reply.Depth);
	}
	portal.ImportedImage = renderer.QueuePortalImage(binding, std::move(reply));
	REQUIRE(portal.ImportedImage != 0);
	CHECK(
		renderer.PortalImageUsage().TextureBytes == 65 * 37 *
														(retainedDirectional ? 64
														 : retainedAmbient	 ? 48
														 : pairedDepth		 ? 12
																			 : 8)
	);
	REQUIRE(renderer.Render(std::span(&view, 1), overlay, nullptr, false).Ran(core::Name("image-export")));
	const auto copiedPixels = render::test::CaptureResource(
		renderer, core::Name("tonemapped"), 0, 65, 37, render::test::ImageFormat::Rgba8Unorm
	);
	CHECK(residentPixels.Bytes == copiedPixels.Bytes);
	const size_t centre = 18 * residentPixels.RowStrideBytes + 32 * 4;
	REQUIRE(residentPixels.Bytes.size() > centre + 3);
	CHECK(
		std::to_integer<uint8_t>(residentPixels.Bytes[centre]) >
		std::to_integer<uint8_t>(residentPixels.Bytes[centre + 2])
	);
	CHECK(
		std::to_integer<uint8_t>(residentPixels.Bytes[centre + 2]) >
		std::to_integer<uint8_t>(residentPixels.Bytes[centre + 1])
	);
	if (pairedDepth) {
		render::PortalImageReply colorOnly;
		colorOnly.Key = binding.Expected;
		colorOnly.Scope = binding.ExpectedScope;
		colorOnly.Status = render::PortalImageStatus::Ok;
		colorOnly.Width = copied.Width;
		colorOnly.Height = copied.Height;
		colorOnly.RowStride = copied.RowStride;
		colorOnly.Pixels = copied.Pixels;
		colorOnly.PixelHash = assets::Hasher::Of(colorOnly.Pixels);
		auto repeatedColor = colorOnly;
		portal.ImportedImage = renderer.QueuePortalImage(binding, std::move(colorOnly));
		REQUIRE(portal.ImportedImage != 0);
		CHECK(renderer.QueuePortalImage(binding, std::move(repeatedColor)) == portal.ImportedImage);
		renderer.Render(std::span(&view, 1), overlay, nullptr, false);
		CHECK(renderer.PortalImageUsage().TextureBytes == 65 * 37 * 8);
	}
	REQUIRE(renderer.DropPortalImage(portal.ImportedImage));
	CHECK(renderer.PortalImageUsage().Images == 0);
	CHECK(renderer.PortalImageUsage().TextureBytes == 0);
	CHECK_FALSE(renderer.DropPortalImage(handle));
}

TEST_CASE("failed graphs cannot hand out resident portal images", "[render][gpu][resourceimage][.]") {
	render::test::FixtureDevice fixture;
	fixture.Initialise();
	auto &renderer = fixture.Render;
	InstallFailedImageCapture(renderer);
	const auto token = renderer.QueueResourceImage(
		core::Name("image-export-pipeline"),
		core::Name("image-export"),
		0,
		render::ResourceImageDelivery::Resident
	);
	REQUIRE(token != 0);
	render::SceneTarget target{16, 16};
	render::View view;
	view.Pipeline = core::Name("image-export-pipeline");
	view.Target = &target;
	render::OverlayImage overlay;
	(void)renderer.Render(std::span(&view, 1), overlay, nullptr, false);
	render::PortalImageBinding binding;
	binding.WorldName = core::Name("resident-source");
	binding.Portal = core::Name("Door");
	binding.Expected = {1, "Door", 1, 1};
	CHECK(renderer.AdoptResourceImage(token, binding) == 0);
	render::PortalResidentImages images(renderer);
	const world::PresentationAddress source{"resident-source", "replies", 1, 1};
	const world::PresentationAddress producer{"destination", "requests", 1, 2};
	render::PortalImageRequest request;
	request.Key = binding.Expected;
	request.Width = request.Height = 16;
	request.PixelBudget = 256;
	const render::PortalResidentImages::Time now{};
	REQUIRE(images.Reserve(source, producer, request, binding, now));
	const render::PortalResidentReceipt receipt{request.Key, request.Scope, 0, 0, 0, 16, 16};
	CHECK_FALSE(images.Publish(source, producer, receipt, token, now));
	images.Clear();
	CHECK(renderer.PortalImageUsage().Images == 0);
	REQUIRE(renderer.CancelResourceImage(token));
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	bool reused = false;
	while (!reused && std::chrono::steady_clock::now() < deadline) {
		(void)renderer.TakeResourceImages();
		reused = renderer.RequestResourceImage(
			{token,
			 core::Name("image-export-pipeline"),
			 core::Name("image-export"),
			 0,
			 render::ResourceImageDelivery::Resident}
		);
		if (!reused) {
			SDL_Delay(1);
		}
	}
	REQUIRE(reused);
	CHECK(renderer.CancelResourceImage(token));
}

TEST_CASE(
	"graph image export owns HDR pixels and bounds queued and completed work",
	"[render][gpu][resourceimage][.]"
) {
	using namespace engine;
	render::test::FixtureDevice fixture;
	fixture.Initialise();
	auto &renderer = fixture.Render;
	InstallImageCapture(renderer);
	const auto request = [](uint64_t token) {
		return render::ResourceImageRequest{
			token, core::Name("image-export-pipeline"), core::Name("image-export"), 0
		};
	};
	CHECK_FALSE(renderer.RequestResourceImage(request(0)));
	// Generated groups skip explicit tokens and leave outputs untouched on refusal.
	REQUIRE(renderer.RequestResourceImage(request(1)));
	REQUIRE(renderer.RequestResourceImage(request(3)));
	const std::array groupNodes{core::Name("image-export"), core::Name("image-export")};
	std::array<uint64_t, 2> generatedGroup{777, 888};
	REQUIRE(renderer.QueueResourceImages(
		core::Name("image-export-pipeline"),
		groupNodes,
		0,
		render::ResourceImageDelivery::CopiedPixels,
		generatedGroup
	));
	CHECK(generatedGroup == std::array<uint64_t, 2>{2, 4});
	REQUIRE(renderer.RequestResourceImage(request(99)));
	std::array<uint64_t, 2> refusedGroup{777, 888};
	CHECK_FALSE(renderer.QueueResourceImages(
		core::Name("image-export-pipeline"),
		groupNodes,
		0,
		render::ResourceImageDelivery::CopiedPixels,
		refusedGroup
	));
	CHECK(refusedGroup == std::array<uint64_t, 2>{777, 888});
	for (uint64_t token = 1; token <= 4; ++token)
		REQUIRE(renderer.CancelResourceImage(token));
	REQUIRE(renderer.CancelResourceImage(99));
	const std::array invalidNodes{core::Name("image-export"), core::Name("missing-capture")};
	CHECK_FALSE(renderer.QueueResourceImages(
		core::Name("image-export-pipeline"),
		invalidNodes,
		0,
		render::ResourceImageDelivery::CopiedPixels,
		refusedGroup
	));
	CHECK(refusedGroup == std::array<uint64_t, 2>{777, 888});
	const auto validGroup = std::array{request(100), request(101)};
	CHECK_FALSE(renderer.RequestResourceImages({}));
	CHECK_FALSE(renderer.RequestResourceImages(
		std::array{
			request(100), request(101), request(102), request(103), request(104), request(105), request(106)
		}
	));
	for (const int invalid : {0, 1, 2, 3, 4, 5}) {
		auto group = validGroup;
		switch (invalid) {
		case 0:
			group[1].Token = group[0].Token;
			break;
		case 1:
			group[1].Node = core::Name("missing-capture");
			break;
		case 2:
			group[1].Pipeline = core::Name("missing-pipeline");
			break;
		case 3:
			group[1].ViewSlot = 1;
			break;
		case 4:
			group[1].Delivery = render::ResourceImageDelivery::Resident;
			break;
		case 5:
			group[1].Token = 0;
			break;
		}
		CHECK_FALSE(renderer.RequestResourceImages(group));
		CHECK_FALSE(renderer.CancelResourceImage(100));
		CHECK_FALSE(renderer.CancelResourceImage(101));
	}

	auto absent = request(1);
	absent.Node = core::Name("absent");
	CHECK_FALSE(renderer.RequestResourceImage(absent));
	absent = request(1);
	absent.ViewSlot = 3;
	CHECK_FALSE(renderer.RequestResourceImage(absent));
	REQUIRE(renderer.RequestResourceImage(request(99)));
	REQUIRE(renderer.RequestResourceImage(request(98)));
	for (uint64_t token = 1; token <= 4; token++) {
		REQUIRE(renderer.RequestResourceImage(request(token)));
	}
	CHECK_FALSE(renderer.RequestResourceImage(request(1)));
	CHECK_FALSE(renderer.RequestResourceImage(request(5)));
	REQUIRE(renderer.CancelResourceImage(4));
	CHECK_FALSE(renderer.RequestResourceImages(validGroup));
	CHECK_FALSE(renderer.CancelResourceImage(100));
	CHECK_FALSE(renderer.CancelResourceImage(101));
	CHECK_FALSE(renderer.RequestResourceImages(std::array{request(100), request(1)}));
	CHECK_FALSE(renderer.CancelResourceImage(100));

	REQUIRE(renderer.RequestResourceImage(request(5)));
	CHECK(renderer.TakeResourceImages().empty());

	assets::MeshData plane;
	plane.Vertices = {
		{{-1, -1, 0}, {0, 0, 1}, {0, 1}},
		{{1, -1, 0}, {0, 0, 1}, {1, 1}},
		{{1, 1, 0}, {0, 0, 1}, {1, 0}},
		{{-1, 1, 0}, {0, 0, 1}, {0, 0}}
	};
	plane.Indices = {0, 1, 2, 0, 2, 3};
	plane.ComputeBounds();
	REQUIRE(renderer.AddMesh(core::Name("export-plane"), plane));
	assets::TextureData white;
	white.Width = white.Height = 1;
	white.Format = assets::TextureFormat::RGBA8;
	white.Pixels.assign(4, std::byte{255});
	REQUIRE(renderer.AddTexture(core::Name("export-white"), white));
	scene::DrawInstance instance;
	instance.Source = 1;
	instance.Frame.Position = {0, 0, -4};
	instance.HalfExtent = {100, 100, 0.01f};
	instance.Tint = {1, 0, 0};
	instance.Mesh = core::Name("export-plane");
	instance.Texture = core::Name("export-white");
	instance.CastShadow = false;
	scene::WorldLighting lighting;
	lighting.Ambient = {2, 2, 2};
	lighting.OutdoorAmbient = {2, 2, 2};
	lighting.Direct = {};
	render::SceneTarget target{65, 37};
	render::View view;
	view.Pipeline = core::Name("image-export-pipeline");
	view.Target = &target;
	view.Instances = std::span(&instance, 1);
	view.OverrideLighting = true;
	view.Lighting = lighting;
	render::OverlayImage overlay;
	REQUIRE(renderer.Render(std::span(&view, 1), overlay, nullptr, false).Ran(core::Name("image-export")));
	// Pending cancellation suppresses bytes while retaining staging ownership until completion.
	REQUIRE(renderer.CancelResourceImage(5));
	REQUIRE(renderer.CancelResourceImage(99));
	REQUIRE(renderer.CancelResourceImage(98));
	auto firstImage = AwaitImage(renderer, 1);
	CHECK_FALSE(renderer.TakeResourceImage(1));
	// A completed member must survive a group with an unfinished or invalid member.
	REQUIRE(renderer.RequestResourceImage(request(6)));
	CHECK_FALSE(renderer.TakeResourceImages(std::array<uint64_t, 2>{3, 6}));
	CHECK_FALSE(renderer.TakeResourceImages(std::array<uint64_t, 2>{3, 999999}));
	CHECK_FALSE(renderer.TakeResourceImages(std::array<uint64_t, 2>{3, 3}));
	CHECK_FALSE(renderer.TakeResourceImages(std::span<const uint64_t>{}));
	CHECK_FALSE(renderer.TakeResourceImages(std::array<uint64_t, 5>{3, 2, 1, 5, 6}));
	REQUIRE(renderer.CancelResourceImage(6));
	auto images = AwaitImageGroup(renderer, std::array<uint64_t, 2>{3, 2});
	CHECK(images[0].Request.Token == 3);
	CHECK(images[1].Request.Token == 2);
	CHECK_FALSE(renderer.TakeResourceImages(std::array<uint64_t, 2>{3, 2}));
	images.push_back(std::move(firstImage));
	const std::array<std::byte, 8> red{
		std::byte{0},
		std::byte{0x40},
		std::byte{0},
		std::byte{0},
		std::byte{0},
		std::byte{0},
		std::byte{0},
		std::byte{0x3c}
	};
	for (const auto &image : images) {
		CHECK(image.Request.Token <= 3);
		CHECK(image.Status == render::ResourceImageStatus::Ok);
		CHECK(image.Width == 65);
		CHECK(image.Height == 37);
		CHECK(image.RowStride == 65 * 8);
		REQUIRE(image.Pixels.size() == size_t(65 * 37 * 8));
		for (size_t pixel = 0; pixel < 65 * 37; pixel++) {
			REQUIRE(std::equal(red.begin(), red.end(), image.Pixels.begin() + pixel * 8));
		}
	}
	CHECK(renderer.TakeResourceImages().empty());
	const auto firstPixels = images.front().Pixels;
	REQUIRE(renderer.RequestResourceImage(request(6)));
	instance.Tint = {0, 1, 0};
	REQUIRE(renderer.Render(std::span(&view, 1), overlay, nullptr, false).Ran(core::Name("image-export")));
	auto green = AwaitImages(renderer, 1);
	CHECK(green.front().Pixels != firstPixels);
	CHECK(green.front().CaptureFrame > images.front().CaptureFrame);
	CHECK(images.front().Pixels == firstPixels);
	// The renderer's ordinary upload ring warms independently of the export pool.
	for (uint64_t token = 10; token < 14; token++) {
		REQUIRE(renderer.RequestResourceImage(request(token)));
		REQUIRE(
			renderer.Render(std::span(&view, 1), overlay, nullptr, false).Ran(core::Name("image-export"))
		);
		(void)AwaitImages(renderer, 1);
	}
	const auto residency = renderer.MemoryStatistics();
	REQUIRE(renderer.RequestResourceImage(request(14)));
	REQUIRE(renderer.Render(std::span(&view, 1), overlay, nullptr, false).Ran(core::Name("image-export")));
	(void)AwaitImages(renderer, 1);
	CHECK(renderer.MemoryStatistics().TransferBuffers == residency.TransferBuffers);
	CHECK(renderer.MemoryStatistics().TransferBufferBytes == residency.TransferBufferBytes);
	const uint64_t generated =
		renderer.QueueResourceImage(core::Name("image-export-pipeline"), core::Name("image-export"));
	REQUIRE(generated != 0);
	const uint64_t other =
		renderer.QueueResourceImage(core::Name("image-export-pipeline"), core::Name("image-export"));
	REQUIRE(other != 0);
	REQUIRE(other != generated);
	REQUIRE(renderer.Render(std::span(&view, 1), overlay, nullptr, false).Ran(core::Name("image-export")));
	CHECK_FALSE(renderer.TakeResourceImage(999999));
	CHECK(AwaitImage(renderer, generated).Request.Token == generated);
	CHECK(AwaitImage(renderer, other).Request.Token == other);

	// A valid graph can expose a display target; the HDR export refuses that format explicitly.
	InstallImageCapture(renderer, "composed-image");
	REQUIRE(renderer.RequestResourceImage(request(7)));
	REQUIRE(renderer.Render(std::span(&view, 1), overlay, nullptr, false).Ran(core::Name("image-export")));
	auto refused = AwaitImages(renderer, 1);
	CHECK(refused.front().Status == render::ResourceImageStatus::Unsupported);
	CHECK(refused.front().Pixels.empty());

	InstallFailedImageCapture(renderer);
	REQUIRE(renderer.RequestResourceImages(std::array{request(20), request(22)}));
	const auto failed = renderer.Render(std::span(&view, 1), overlay, nullptr, false);
	REQUIRE(failed.Ran(core::Name("image-export")));
	auto failedImages = AwaitImageGroup(renderer, std::array<uint64_t, 2>{20, 22});
	CHECK(failedImages[1].Status == render::ResourceImageStatus::Failed);
	CHECK(failedImages.front().Status == render::ResourceImageStatus::Failed);
	CHECK(failedImages.front().Pixels.empty());
	CHECK(failedImages.front().Width == 0);
	// A failed frame must release queue capacity after its actual submission completes.
	InstallImageCapture(renderer);
	REQUIRE(renderer.RequestResourceImage(request(21)));
	REQUIRE(renderer.Render(std::span(&view, 1), overlay, nullptr, false).Ran(core::Name("image-export")));
	CHECK(AwaitImages(renderer, 1).front().Status == render::ResourceImageStatus::Ok);
}

TEST_CASE(
	"a resident eye image draws on the first use of a new viewport",
	"[render][gpu][resourceimage][eye-first-use][.]"
) {
	using namespace engine;
	using namespace graph;
	const size_t nextSlot = GENERATE(size_t(3), size_t(4));
	const bool sceneOnly = GENERATE(false, true);
	const bool present = GENERATE(false, true);
	const bool captureHdr = GENERATE(false, true);
	CAPTURE(nextSlot, sceneOnly, present, captureHdr);
	render::test::FixtureDevice fixture;
	fixture.Initialise();
	auto &renderer = fixture.Render;
	InstallImageCapture(renderer, "lit", true);
	auto document = DefaultEyeDocument();
	if (captureHdr) {
		for (const size_t slot : {size_t(3), size_t(4)}) {
			document.Record(
				{.Kind = EditKind::AddNode,
				 .Name = core::Name("eye-export-" + std::to_string(slot)),
				 .NodeKind = core::Name("capture"),
				 .Scope = NodeScope::Frame}
			);
			document.Record(
				{.Kind = EditKind::Set, .Key = core::Name("view"), .Value = std::to_string(slot)}
			);
			document.Record(
				{.Kind = EditKind::Reads, .Target = core::Name("eye-hdr"), .Key = core::Name("source")}
			);
		}
	}
	const auto displayPath =
		core::Paths::Base() / ("eye-first-use-" + std::to_string(nextSlot) + "-" + std::to_string(sceneOnly) +
							   "-" + std::to_string(present) + "-" + std::to_string(captureHdr) + ".bmp");
	if (!present) {
		document.Record(
			{.Kind = EditKind::AddNode,
			 .Name = core::Name("display-export"),
			 .NodeKind = core::Name("capture"),
			 .Scope = NodeScope::Frame}
		);
		document.Record({.Kind = EditKind::Set, .Key = core::Name("path"), .Value = displayPath.string()});
		document.Record({.Kind = EditKind::Set, .Key = core::Name("capture.mode"), .Value = "every-frame"});
		document.Record(
			{.Kind = EditKind::Reads, .Target = core::Name("scene-image"), .Key = core::Name("source")}
		);
	}
	RenderGraph pipeline;
	core::Name offender;
	REQUIRE(Build(document, pipeline, offender) == PipelineDocumentStatus::Ok);
	const core::Name eyePipeline("first-eye-export");
	REQUIRE(renderer.SetPipeline(eyePipeline, pipeline));
	assets::TextureData white;
	white.Width = white.Height = 1;
	white.Format = assets::TextureFormat::RGBA8;
	white.Pixels.assign(4, std::byte{255});
	const core::Name emission("first-eye-emission");
	REQUIRE(renderer.AddTexture(emission, white));
	scene::DrawInstance wall;
	wall.Source = 1;
	wall.Frame.Position = {0, 0, -4};
	wall.HalfExtent = {20, 20, .05f};
	wall.CastShadow = false;
	wall.EmissiveMap = emission;
	wall.EmissiveTint = {0, 0, 1};
	render::SceneTarget target{65, 37};
	render::View source;
	source.World = 22;
	source.WorldName = core::Name("first-eye-producer");
	source.Target = &target;
	source.Pipeline = core::Name("image-export-pipeline");
	source.Instances = std::span(&wall, 1);
	source.OverrideLighting = true;
	source.Lighting.Ambient = {};
	source.Lighting.OutdoorAmbient = {};
	source.Lighting.Direct = {};
	render::OverlayImage overlay;
	const auto capture = [&](render::ResourceImageDelivery delivery) {
		const auto token =
			renderer.QueueResourceImage(source.Pipeline, core::Name("image-export"), 0, delivery);
		REQUIRE(token != 0);
		REQUIRE(
			renderer.Render(std::span(&source, 1), overlay, nullptr, false).Ran(core::Name("image-export"))
		);
		return token;
	};
	const auto reference = AwaitImage(renderer, capture(render::ResourceImageDelivery::CopiedPixels));
	core::ByteReader centre(std::span(reference.Pixels).subspan((18 * 65 + 32) * 8, 8));
	(void)centre.ReadUInt32();
	REQUIRE(glm::unpackHalf2x16(centre.ReadUInt32()).x > .9f);
	uint64_t fallback = 0;
	for (const size_t slot : {size_t(3), nextSlot}) {
		render::PortalImageBinding binding;
		binding.World = 11;
		binding.WorldName = core::Name("first-eye-viewer");
		binding.ViewSlot = slot;
		binding.Portal = core::Name("viewport-eye");
		binding.Expected = {1, "viewport-eye", 1, 1};
		binding.ExpectedProjection = render::PortalImageProjection::Eye;
		binding.ExpectedScope = render::PortalImageScope::CompleteWorld;
		binding.Sampling =
			scene::ResolveCamera(source.CameraFrame, source.Camera, 65.f / 37.f).ViewProjection;
		const auto handle =
			renderer.AdoptResourceImage(capture(render::ResourceImageDelivery::Resident), binding);
		REQUIRE(handle != 0);
		render::View eye;
		eye.World = binding.World;
		eye.WorldName = binding.WorldName;
		eye.Slot = slot;
		eye.Target = &target;
		eye.Pipeline = eyePipeline;
		eye.EyeImage = handle;
		eye.EyeImageKey = binding.Portal;
		if (sceneOnly) eye.Damage = {.Scene = true};
		for (int frame = 0; frame < 2; ++frame) {
			CAPTURE(slot, frame);
			const core::Name exportNode("eye-export-" + std::to_string(slot));
			const auto token = captureHdr ? renderer.QueueResourceImage(eyePipeline, exportNode, slot) : 0;
			if (captureHdr) REQUIRE(token != 0);
			if (present) {
				REQUIRE(renderer.WaitForFrame());
				renderer.RequestSceneCapture(displayPath, slot);
			}
			const auto rendered = renderer.Render(std::span(&eye, 1), overlay, nullptr, present);
			REQUIRE(rendered.Ran(core::Name("eye-image")));
			if (captureHdr) {
				REQUIRE(rendered.Ran(exportNode));
				const auto actual = AwaitImage(renderer, token);
				CHECK(actual.Pixels == reference.Pixels);
			}
			std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)> displayed(
				SDL_LoadBMP(displayPath.string().c_str()), SDL_DestroySurface
			);
			REQUIRE(displayed);
			Uint8 red = 0, green = 0, blue = 0, alpha = 0;
			REQUIRE(SDL_ReadSurfacePixel(displayed.get(), 32, 18, &red, &green, &blue, &alpha));
			CAPTURE(red, green, blue);
			CHECK(blue > 90);
			if (blue <= 90)
				std::filesystem::copy_file(
					displayPath,
					displayPath.string() + ".slot-" + std::to_string(slot) + ".frame-" +
						std::to_string(frame) + ".bmp",
					std::filesystem::copy_options::overwrite_existing
				);
			std::filesystem::remove(displayPath);
		}
		if (slot == 3 && nextSlot != 3)
			fallback = handle;
		else
			CHECK(renderer.DropPortalImage(handle));
	}
	if (fallback != 0) CHECK(renderer.DropPortalImage(fallback));
}

TEST_CASE(
	"a retained world packet follows the current viewing camera",
	"[render][gpu][resourceimage][eye-current-camera][.]"
) {
	using namespace engine;
	using namespace graph;
	const int motion = GENERATE(0, 1, 2, 3);
	CAPTURE(motion);
	render::test::FixtureDevice fixture;
	fixture.Initialise();
	auto &renderer = fixture.Render;
	InstallImageCapture(renderer, "lit", true);
	auto document = DefaultEyeDocument();
	document.Record(
		{.Kind = EditKind::AddNode,
		 .Name = core::Name("image-export"),
		 .NodeKind = core::Name("capture"),
		 .Scope = NodeScope::Frame}
	);
	document.Record({.Kind = EditKind::Reads, .Target = core::Name("eye-hdr"), .Key = core::Name("source")});
	RenderGraph pipeline;
	core::Name offender;
	REQUIRE(Build(document, pipeline, offender) == PipelineDocumentStatus::Ok);
	const core::Name eyePipeline("current-eye-export");
	REQUIRE(renderer.SetPipeline(eyePipeline, pipeline));
	assets::TextureData white;
	white.Width = white.Height = 1;
	white.Format = assets::TextureFormat::RGBA8;
	white.Pixels.assign(4, std::byte{255});
	const core::Name emission("current-eye-emission");
	std::array<scene::DrawInstance, 3> room;
	for (size_t index = 0; index < room.size(); ++index) {
		room[index].Source = index + 1;
		room[index].CastShadow = false;
		room[index].EmissiveMap = emission;
	}
	room[0].Frame.Position = {0, 0, -8};
	room[0].HalfExtent = {8, 8, .05f};
	room[0].EmissiveTint = {0, 0, 1};
	room[1].Frame.Position = {0, 0, -4};
	room[1].HalfExtent = {.6f, 1.5f, .05f};
	room[1].EmissiveTint = {1, 0, 0};
	room[2].Frame.Position = {0, 0, -6};
	room[2].HalfExtent = {.2f, 1, .05f};
	room[2].EmissiveTint = {0, 1, 0};
	render::SceneTarget target{65, 37};
	render::View view;
	view.Target = &target;
	view.WorldName = core::Name("current-eye-room");
	view.ContentOwner = view.WorldName;
	REQUIRE(renderer.AddTexture(emission, white, view.ContentOwner));
	view.OverrideLighting = true;
	view.Lighting.Ambient = {};
	view.Lighting.OutdoorAmbient = {};
	view.Lighting.Direct = {};
	const auto capture = [&](core::Name selected, std::span<const scene::DrawInstance> rows) {
		view.Pipeline = selected;
		view.Instances = rows;
		const auto token = renderer.QueueResourceImage(selected, core::Name("image-export"));
		REQUIRE(token != 0);
		render::OverlayImage overlay;
		REQUIRE(
			renderer.Render(std::span(&view, 1), overlay, nullptr, false).Ran(core::Name("image-export"))
		);
		return AwaitImage(renderer, token);
	};
	const core::Name directPipeline("image-export-pipeline");
	ecs::Store packetStore("current-eye-room");
	render::WorldViewFrame retained;
	retained.Name = view.WorldName;
	retained.Identity = packetStore.Identity();
	retained.Instances.assign(room.begin(), room.end());
	retained.Lighting = view.Lighting;
	render::WorldCameraFrame cameraLayers;
	const std::array contentOwners{
		render::WorldContentOwner{core::Name("foreign-room"), core::Name("foreign-room-content")}
	};
	render::WorldViewBinding packetBinding{
		.World = view.World,
		.Name = view.WorldName,
		.Identity = packetStore.Identity(),
		.ContentOwner = view.ContentOwner,
		.ForeignContentOwners = contentOwners,
		.Pipeline = directPipeline
	};
	const auto accepted = capture(directPipeline, room);
	render::PortalImageBinding binding;
	binding.WorldName = view.WorldName;
	binding.Portal = core::Name("viewport-eye");
	binding.Expected = {1, "viewport-eye", 1, 1};
	binding.ExpectedProjection = render::PortalImageProjection::Eye;
	binding.ExpectedScope = render::PortalImageScope::CompleteWorld;
	binding.Sampling = scene::ResolveCamera(view.CameraFrame, view.Camera, 65.f / 37.f).ViewProjection;
	render::PortalImageReply reply;
	reply.Key = binding.Expected;
	reply.Scope = binding.ExpectedScope;
	reply.Status = render::PortalImageStatus::Ok;
	reply.Width = accepted.Width;
	reply.Height = accepted.Height;
	reply.RowStride = accepted.RowStride;
	reply.Pixels = accepted.Pixels;
	reply.Depth = accepted.Depth;
	reply.DepthHash = assets::Hasher::Of(reply.Depth);
	REQUIRE(reply.Depth.size() == size_t(accepted.Width) * accepted.Height * 4);
	reply.PixelHash = assets::Hasher::Of(reply.Pixels);
	view.EyeImage = renderer.QueuePortalImage(binding, std::move(reply));
	view.EyeImageKey = binding.Portal;
	REQUIRE(view.EyeImage != 0);
	const auto retainedImage = view.EyeImage;
	if (motion == 1 || motion == 3) view.CameraFrame.Position.X = 3.f;
	if (motion == 2 || motion == 3) view.CameraFrame = view.CameraFrame * core::CFrame::Angles(0, .2f, 0);
	const auto expected = capture(directPipeline, room);
	const auto staleImage = capture(eyePipeline, {});
	// Pixels alone retain their capture camera; the hidden post requires the world packet.
	CHECK(staleImage.Pixels == accepted.Pixels);
	const auto currentCamera = view.CameraFrame;
	const auto currentProjection = view.Projection;
	const auto currentTarget = view.Target;
	const auto currentSlot = view.Slot;
	auto wrongOwner = packetBinding;
	wrongOwner.Name = core::Name("unrelated-eye-room");
	CHECK_FALSE(render::BindWorldView(retained, cameraLayers, wrongOwner, view));
	CHECK(view.EyeImage == retainedImage);
	CHECK(view.Pipeline == eyePipeline);
	ecs::Store replacementStore("current-eye-room");
	auto wrongStore = packetBinding;
	wrongStore.Identity = replacementStore.Identity();
	CHECK_FALSE(render::BindWorldView(retained, cameraLayers, wrongStore, view));
	CHECK(view.EyeImage == retainedImage);
	CHECK(view.Pipeline == eyePipeline);
	REQUIRE(render::BindWorldView(retained, cameraLayers, packetBinding, view));
	CHECK(view.CameraFrame.Position == currentCamera.Position);
	CHECK(view.CameraFrame.Rotation() == currentCamera.Rotation());
	CHECK(view.Projection == currentProjection);
	CHECK(view.Target == currentTarget);
	CHECK(view.Slot == currentSlot);
	CHECK(view.EyeImage == 0);
	CHECK(view.EyeImageKey == core::Name{});
	CHECK(view.ContentOwner == packetBinding.ContentOwner);
	CHECK(view.ForeignContentOwners.data() == contentOwners.data());
	CHECK(view.Instances.data() == retained.Instances.data());
	const auto actual = capture(view.Pipeline, view.Instances);
	const auto unpack = [](const render::ResourceImage &image) {
		std::vector<float> samples;
		core::ByteReader reader(image.Pixels);
		while (!reader.AtEnd()) {
			const auto pair = glm::unpackHalf2x16(reader.ReadUInt32());
			samples.push_back(pair.x);
			samples.push_back(pair.y);
		}
		return samples;
	};
	const auto expectedSamples = unpack(expected), actualSamples = unpack(actual);
	if (motion == 1 || motion == 3) {
		const auto greenPixels = [](const std::vector<float> &samples) {
			size_t count = 0;
			for (size_t pixel = 0; pixel < samples.size(); pixel += 4)
				if (samples[pixel + 1] > .9f && samples[pixel] < .1f && samples[pixel + 2] < .1f) ++count;
			return count;
		};
		CHECK(greenPixels(unpack(accepted)) == 0);
		REQUIRE(greenPixels(expectedSamples) > 4);
	}
	const auto asImage = [](const render::ResourceImage &image, const std::vector<float> &samples) {
		return render::test::ImageView{
			image.Width,
			image.Height,
			render::test::ImageFormat::Rgba32Float,
			std::as_bytes(std::span(samples)),
			size_t(image.Width) * 16
		};
	};
	if (motion != 0) CHECK(expected.Pixels != accepted.Pixels);
	render::test::CheckImage(
		renderer,
		"eye-current-camera",
		"motion-" + std::to_string(motion),
		"retained world packet at current camera; red near occluder, green hidden post, blue rear wall",
		asImage(expected, expectedSamples),
		asImage(actual, actualSamples),
		{.Absolute = .002, .Region = {}}
	);
	CHECK(renderer.DropPortalImage(retainedImage));
}

TEST_CASE(
	"eye images preserve directional samples through upload and resident adoption",
	"[render][gpu][resourceimage][eye-directional-response][.]"
) {
	using namespace graph;
	render::test::FixtureDevice fixture;
	fixture.Initialise();
	auto &renderer = fixture.Render;
	const std::array names{
		"colour", "depth", "normal", "ambient-response", "lighting-baseline", "directional-response"
	};
	const std::array formats{
		ResourceFormat::RGBA16F,
		ResourceFormat::R32F,
		ResourceFormat::RGB10A2,
		ResourceFormat::RGBA32F,
		ResourceFormat::RGBA32F,
		ResourceFormat::RGBA32F
	};
	PipelineDocument document;
	for (size_t plane = 0; plane < names.size(); ++plane)
		document.Record(
			{.Kind = EditKind::AddResource,
			 .Name = core::Name(names[plane]),
			 .Resource = ResourceKind::Colour,
			 .Format = formats[plane]}
		);
	document.Record(
		{.Kind = EditKind::AddNode,
		 .Name = core::Name("eye"),
		 .NodeKind = core::Name("eye-image"),
		 .Scope = NodeScope::View}
	);
	document.Record({.Kind = EditKind::Set, .Key = core::Name("scope"), .Value = "opaque-lighting"});
	for (const char *name : names)
		document.Record({.Kind = EditKind::Writes, .Target = core::Name(name), .Key = core::Name(name)});
	document.Record(
		{.Kind = EditKind::AddNode,
		 .Name = core::Name("export"),
		 .NodeKind = core::Name("capture"),
		 .Scope = NodeScope::Frame}
	);
	for (size_t plane = 0; plane < names.size(); ++plane)
		document.Record(
			{.Kind = EditKind::Reads,
			 .Target = core::Name(names[plane]),
			 .Key = core::Name(plane == 0 ? "source" : names[plane])}
		);
	RenderGraph pipeline;
	core::Name offender;
	REQUIRE(Build(document, pipeline, offender) == PipelineDocumentStatus::Ok);
	const core::Name pipelineName("eye-directional-samples");
	REQUIRE(renderer.SetPipeline(pipelineName, pipeline));
	constexpr uint32_t width = 17, height = 9;
	render::SceneTarget target{width, height};
	render::View view;
	view.Target = &target;
	view.WorldName = core::Name("directional-samples-world");
	view.Pipeline = pipelineName;
	view.EyeImageKey = core::Name("directional-samples-image");
	render::PortalImageBinding binding;
	binding.WorldName = view.WorldName;
	binding.Portal = view.EyeImageKey;
	binding.Expected = {1, "directional-samples-image", 1, 1};
	binding.ExpectedScope = render::PortalImageScope::OpaqueLighting;
	binding.ExpectedProjection = render::PortalImageProjection::Eye;
	render::PortalImageReply reply;
	reply.Key = binding.Expected;
	reply.Status = render::PortalImageStatus::Ok;
	reply.Scope = binding.ExpectedScope;
	reply.CaptureLighting.emplace();
	reply.Width = width;
	reply.Height = height;
	reply.RowStride = width * 8;
	core::ByteWriter colour, depth, normal, ambient, baseline, directional;
	for (uint32_t y = 0; y < height; ++y)
		for (uint32_t x = 0; x < width; ++x) {
			colour.WriteUInt32(glm::packHalf2x16({.25f, .5f}));
			colour.WriteUInt32(glm::packHalf2x16({.75f, 1.f}));
			depth.WriteFloat(2.f + float(x + y) / 16.f);
			normal.WriteUInt32(0xfff80200u);
			for (float value : {.125f, .25f, .375f, .5f})
				ambient.WriteFloat(value);
			for (float value : {.25f, .5f, .75f, 1.f})
				baseline.WriteFloat(value);
			for (float value :
				 {float(x + 1) / 8.f,
				  float(y + 1) / 16.f,
				  float(x + y + 1) / 32.f,
				  float((x + 3 * y) % 17) / 16.f})
				directional.WriteFloat(value);
		}
	const std::array sources{&colour, &depth, &normal, &ambient, &baseline, &directional};
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
	for (size_t plane = 0; plane < planes.size(); ++plane) {
		planes[plane]->assign(sources[plane]->Bytes().begin(), sources[plane]->Bytes().end());
		*hashes[plane] = assets::Hasher::Of(*planes[plane]);
	}
	const auto expected = reply;
	view.EyeImage = renderer.QueuePortalImage(binding, std::move(reply));
	REQUIRE(view.EyeImage != 0);
	const auto resident = renderer.QueueResourceImage(
		pipelineName, core::Name("export"), 0, render::ResourceImageDelivery::Resident
	);
	REQUIRE(resident != 0);
	for (int delivery = 0; delivery < 2; ++delivery) {
		CAPTURE(delivery);
		const auto copied = renderer.QueueResourceImage(pipelineName, core::Name("export"));
		REQUIRE(copied != 0);
		render::OverlayImage overlay;
		REQUIRE(renderer.Render(std::span(&view, 1), overlay, nullptr, false).Ran(core::Name("export")));
		const auto actual = AwaitImage(renderer, copied);
		REQUIRE(actual.Status == render::ResourceImageStatus::Ok);
		CHECK(actual.Pixels == expected.Pixels);
		CHECK(actual.Depth == expected.Depth);
		CHECK(actual.Normal == expected.Normal);
		CHECK(actual.AmbientResponse == expected.AmbientResponse);
		CHECK(actual.LightingBaseline == expected.LightingBaseline);
		CHECK(actual.DirectionalResponseResource == core::Name("directional-response"));
		CHECK(actual.DirectionalResponse == expected.DirectionalResponse);
		CHECK(assets::Hasher::Of(actual.DirectionalResponse) == expected.DirectionalResponseHash);
		CHECK(renderer.PortalImageUsage().Uploads == 6);
		CHECK(renderer.PortalImageUsage().PendingCpuBytes == 0);
		if (delivery == 0) {
			binding.Expected.RequestId++;
			view.EyeImage = renderer.AdoptResourceImage(resident, binding);
			REQUIRE(view.EyeImage != 0);
		}
	}
	CHECK(renderer.DropPortalImage(view.EyeImage));
	CHECK(renderer.PortalImageUsage().TextureBytes == 0);
	CHECK(renderer.PortalImageUsage().CachedTextureBytes == 0);
}

TEST_CASE(
	"resizing an eye image preserves each color and depth sample pair",
	"[render][gpu][resourceimage][eye-pair-resize][.]"
) {
	using namespace graph;
	render::test::FixtureDevice fixture;
	fixture.Initialise();
	auto &renderer = fixture.Render;
	const bool resident = GENERATE(false, true);
	const bool reduce = GENERATE(false, true);
	CAPTURE(resident, reduce);
	PipelineDocument document;
	for (const auto &[name, format] :
		 {std::pair{"eye-colour", ResourceFormat::RGBA16F}, {"eye-depth", ResourceFormat::R32F}})
		document.Record(
			{.Kind = EditKind::AddResource,
			 .Name = core::Name(name),
			 .Resource = ResourceKind::Colour,
			 .Format = format}
		);
	document.Record(
		{.Kind = EditKind::AddNode,
		 .Name = core::Name("eye"),
		 .NodeKind = core::Name("eye-image"),
		 .Scope = NodeScope::View}
	);
	document.Record(
		{.Kind = EditKind::Writes, .Target = core::Name("eye-colour"), .Key = core::Name("colour")}
	);
	document.Record(
		{.Kind = EditKind::Writes, .Target = core::Name("eye-depth"), .Key = core::Name("depth")}
	);
	document.Record(
		{.Kind = EditKind::AddNode,
		 .Name = core::Name("export"),
		 .NodeKind = core::Name("capture"),
		 .Scope = NodeScope::Frame}
	);
	document.Record(
		{.Kind = EditKind::Reads, .Target = core::Name("eye-colour"), .Key = core::Name("source")}
	);
	document.Record({.Kind = EditKind::Reads, .Target = core::Name("eye-depth"), .Key = core::Name("depth")});
	RenderGraph pipeline;
	core::Name offender;
	REQUIRE(Build(document, pipeline, offender) == PipelineDocumentStatus::Ok);
	REQUIRE(renderer.SetPipeline(core::Name("eye-resize"), pipeline));
	const uint32_t width = reduce ? 3 : 13, height = reduce ? 3 : 7;
	render::SceneTarget target{width, height};
	render::View view;
	view.Target = &target;
	view.WorldName = core::Name("eye-room");
	view.Pipeline = core::Name("eye-resize");
	view.EyeImageKey = core::Name("eye-room-image");
	render::PortalImageBinding binding;
	binding.WorldName = view.WorldName;
	binding.Portal = view.EyeImageKey;
	binding.Expected = {1, "eye-room-image", 1, 1};
	binding.ExpectedProjection = render::PortalImageProjection::Eye;
	render::PortalImageReply reply;
	reply.Key = binding.Expected;
	reply.Status = render::PortalImageStatus::Ok;
	reply.Width = 8;
	reply.Height = 8;
	reply.RowStride = 64;
	const std::array colors{
		glm::vec4{1, 0, 0, 1}, glm::vec4{0, 0, 1, 1}, glm::vec4{0, 1, 0, 1}, glm::vec4{1, 1, 0, 1}
	};
	const std::array distances{2.f, 6.f, 10.f, 0.f};
	core::ByteWriter colorBytes, depthBytes;
	for (size_t pixel = 0; pixel < 64; ++pixel) {
		const size_t i = (pixel % 8 >= 4 ? 1 : 0) + (pixel / 8 >= 4 ? 2 : 0);
		colorBytes.WriteUInt32(glm::packHalf2x16({colors[i].r, colors[i].g}));
		colorBytes.WriteUInt32(glm::packHalf2x16({colors[i].b, colors[i].a}));
		depthBytes.WriteFloat(distances[i]);
	}
	reply.Pixels.assign(colorBytes.Bytes().begin(), colorBytes.Bytes().end());
	reply.Depth.assign(depthBytes.Bytes().begin(), depthBytes.Bytes().end());
	reply.PixelHash = assets::Hasher::Of(reply.Pixels);
	reply.DepthHash = assets::Hasher::Of(reply.Depth);
	view.EyeImage = renderer.QueuePortalImage(binding, std::move(reply));
	REQUIRE(view.EyeImage != 0);
	render::OverlayImage overlay;
	if (resident) {
		render::SceneTarget residentTarget{8, 8};
		view.Target = &residentTarget;
		const auto residentToken = renderer.QueueResourceImage(
			view.Pipeline, core::Name("export"), 0, render::ResourceImageDelivery::Resident
		);
		REQUIRE(residentToken != 0);
		REQUIRE(renderer.Render(std::span(&view, 1), overlay, nullptr, false).Ran(core::Name("export")));
		const auto image = renderer.AdoptResourceImage(residentToken, binding);
		REQUIRE(image != 0);
		CHECK(image != view.EyeImage);
		CHECK(renderer.PortalImageUsage().Images == 1);
		view.EyeImage = image;
		view.Target = &target;
	}
	const auto token = renderer.QueueResourceImage(view.Pipeline, core::Name("export"));
	REQUIRE(token != 0);
	REQUIRE(renderer.Render(std::span(&view, 1), overlay, nullptr, false).Ran(core::Name("export")));
	const auto resized = AwaitImage(renderer, token);
	REQUIRE(resized.Width == width);
	REQUIRE(resized.Height == height);
	core::ByteReader color(resized.Pixels), depth(resized.Depth);
	std::array<size_t, 4> selected{};
	std::vector<glm::vec4> samples;
	while (!depth.AtEnd()) {
		const float distance = depth.ReadFloat();
		const auto found = std::find(distances.begin(), distances.end(), distance);
		REQUIRE(found != distances.end());
		const auto index = size_t(found - distances.begin());
		++selected[index];
		const auto rg = glm::unpackHalf2x16(color.ReadUInt32());
		const auto ba = glm::unpackHalf2x16(color.ReadUInt32());
		samples.emplace_back(rg, ba);
		CHECK(glm::vec4(rg, ba) == colors[index]);
	}
	if (const char *output = std::getenv("MONO_RENDER_PREVIEW_DIR")) {
		const auto directory = std::filesystem::path(output);
		std::filesystem::create_directories(directory);
		const std::string stem =
			std::string(resident ? "resident-" : "copied-") + (reduce ? "reduced" : "enlarged");
		const render::test::ImageView preview{
			width, height, render::test::ImageFormat::Rgba32Float, std::as_bytes(std::span(samples)), 0
		};
		render::test::WriteImagePreview(directory / (stem + ".ppm"), preview);
	}
	CHECK(color.AtEnd());
	for (const auto count : selected)
		CHECK(count > 0);
	REQUIRE(renderer.DropPortalImage(view.EyeImage));
}

TEST_CASE(
	"a current opaque body composes against a retained destination wall",
	"[render][gpu][resourceimage][body-depth-compose][.]"
) {
	using namespace graph;
	const bool opaqueScope = GENERATE(false, true);
	const bool shaded = GENERATE(false, true);
	const bool humanoid = GENERATE(false, true);
	render::test::FixtureDevice fixture;
	fixture.Initialise();
	auto &renderer = fixture.Render;
	const auto install = [&](bool compose, bool opaque, bool seam = false) {
		PipelineDocument document;
		if (compose) {
			const auto shared = DefaultPortalBodyDocument(seam);
			for (auto edit : shared.Edits()) {
				// Complete-world fixtures contain only opaque geometry.
				if (!opaque && edit.Kind == EditKind::Set && edit.Key == core::Name("scope"))
					edit.Value = "complete-world";
				document.Record(std::move(edit));
			}
		} else {
			const auto resource = [&](const char *name, ResourceFormat format) {
				document.Record(
					{.Kind = EditKind::AddResource,
					 .Name = core::Name(name),
					 .Resource = ResourceKind::Colour,
					 .Format = format}
				);
			};
			const auto node = [&](const char *name, const char *kind) {
				document.Record(
					{.Kind = EditKind::AddNode,
					 .Name = core::Name(name),
					 .NodeKind = core::Name(kind),
					 .Scope = NodeScope::View}
				);
			};
			const auto edge = [&](EditKind kind, const char *name, const char *port) {
				document.Record({.Kind = kind, .Target = core::Name(name), .Key = core::Name(port)});
			};
			const auto base = DefaultPbrDocument();
			for (const auto &edit : base.Edits()) {
				if (edit.Kind == EditKind::AddNode && edit.Name == core::Name("present")) {
					resource("opaque-depth", ResourceFormat::R32F);
					node("opaque-depth-export", "depth-linearise");
					edge(EditKind::Reads, "depth", "depth");
					edge(EditKind::Writes, "opaque-depth", "linear");
					document.Record(
						{.Kind = EditKind::Set, .Key = core::Name("background"), .Value = "zero"}
					);
				}
				document.Record(edit);
			}
			document.Record(
				{.Kind = EditKind::AddNode,
				 .Name = core::Name("export"),
				 .NodeKind = core::Name("capture"),
				 .Scope = NodeScope::Frame}
			);
			edge(EditKind::Reads, "lit", "source");
			edge(EditKind::Reads, "opaque-depth", "depth");
		}

		RenderGraph pipeline;
		core::Name offender;
		REQUIRE(Build(document, pipeline, offender) == PipelineDocumentStatus::Ok);
		const core::Name name(compose ? (seam ? "body-seam-compose" : "body-compose") : "body-reference");
		REQUIRE(renderer.SetPipeline(name, pipeline));
		return name;
	};
	const auto referencePipeline = install(false, opaqueScope);
	const auto compositionPipeline = install(true, opaqueScope);
	REQUIRE(install(true, opaqueScope, true) == core::Name("body-seam-compose"));
	assets::TextureData white;
	white.Width = white.Height = 1;
	white.Format = assets::TextureFormat::RGBA8;
	white.Pixels.assign(4, std::byte{255});
	const core::Name emission("body-emission");
	REQUIRE(renderer.AddTexture(emission, white));
	render::SceneTarget target{65, 37};
	render::View view;
	view.Target = &target;
	view.WorldName = core::Name("body-source");
	view.OverrideLighting = true;
	view.Lighting.Ambient = {};
	view.Lighting.OutdoorAmbient = {};
	view.Lighting.Direct = {};
	const bool rotated = GENERATE(false, true);
	if (rotated)
		view.CameraFrame = core::CFrame(core::Vector3{7, 2, 3}) * core::CFrame::Angles(-.2f, .37f, .1f);
	const auto capture = [&](core::Name pipeline, std::span<const scene::DrawInstance> rows) {
		view.Pipeline = pipeline;
		view.Instances = rows;
		const auto token = renderer.QueueResourceImage(pipeline, core::Name("export"));
		REQUIRE(token != 0);
		render::OverlayImage overlay;
		const auto frame = renderer.Render(std::span(&view, 1), overlay, nullptr, false);
		REQUIRE(frame.Ran(core::Name("export")));
		return AwaitImage(renderer, token);
	};
	scene::DrawInstance wall;
	wall.Source = 1;
	wall.Frame = view.CameraFrame * core::CFrame(core::Vector3{0, 0, -4});
	wall.HalfExtent = {1, 1, .05f};
	wall.CastShadow = false;
	wall.EmissiveMap = emission;
	wall.EmissiveTint = {1, 0, 0};
	if (shaded) {
		view.Lighting.Direction = view.CameraFrame.VectorToWorldSpace({-.3f, -.4f, -1}).Unit();
		view.Lighting.Direct = {4, 4, 4};
		wall.EmissiveMap = {};
		wall.Tint = {1, 0, 0};
	}
	const bool resident = GENERATE(false, true);
	const auto residentToken =
		resident ? renderer.QueueResourceImage(
					   referencePipeline, core::Name("export"), 0, render::ResourceImageDelivery::Resident
				   )
				 : 0;
	if (resident) REQUIRE(residentToken != 0);
	const auto room = capture(referencePipeline, std::span(&wall, 1));
	render::PortalImageBinding binding;
	binding.WorldName = view.WorldName;
	binding.Portal = core::Name("room");
	binding.Expected = {1, "room", 1, 1};
	binding.ExpectedProjection = render::PortalImageProjection::Eye;
	binding.ExpectedScope =
		opaqueScope ? render::PortalImageScope::OpaqueLighting : render::PortalImageScope::CompleteWorld;
	binding.Sampling = scene::ResolveCamera(view.CameraFrame, view.Camera, 65.f / 37.f).ViewProjection;
	render::PortalImageReply reply;
	reply.Key = binding.Expected;
	reply.Scope = binding.ExpectedScope;
	reply.Status = render::PortalImageStatus::Ok;
	reply.Width = room.Width;
	reply.Height = room.Height;
	reply.RowStride = room.RowStride;
	reply.Pixels = room.Pixels;
	reply.Depth = room.Depth;
	reply.PixelHash = assets::Hasher::Of(reply.Pixels);
	reply.DepthHash = assets::Hasher::Of(reply.Depth);
	view.EyeImage = resident ? renderer.AdoptResourceImage(residentToken, binding)
							 : renderer.QueuePortalImage(binding, std::move(reply));
	view.EyeImageKey = binding.Portal;
	REQUIRE(view.EyeImage != 0);
	view.Pipeline = core::Name("body-seam-compose");
	render::OverlayImage projectionOverlay;
	CHECK_FALSE(renderer.Render(std::span(&view, 1), projectionOverlay, nullptr, false)
					.Ran(core::Name("body-compose")));
	view.Pipeline = install(true, !opaqueScope);
	render::OverlayImage scopeOverlay;
	CHECK_FALSE(
		renderer.Render(std::span(&view, 1), scopeOverlay, nullptr, false).Ran(core::Name("body-compose"))
	);
	REQUIRE(install(true, opaqueScope) == compositionPipeline);
	scene::DrawInstance body = wall;
	body.Source = 2;
	body.HalfExtent = {.6f, 1.4f, .1f};
	body.EmissiveTint = {0, 0, 1};
	if (shaded) body.Tint = {0, 0, 1};
	const auto unpack = [](const render::ResourceImage &image) {
		std::vector<float> samples;
		samples.reserve(image.Pixels.size() / 2);
		core::ByteReader reader(image.Pixels);
		while (!reader.AtEnd()) {
			const auto pair = glm::unpackHalf2x16(reader.ReadUInt32());
			samples.push_back(pair.x);
			samples.push_back(pair.y);
		}
		return samples;
	};
	std::unique_ptr<ecs::Store> characterWorld;
	ecs::Entity characterRoot, characterHumanoid;
	std::vector<std::pair<ecs::Entity, core::CFrame>> limbOffsets;
	if (humanoid) {
		scene::RegisterSceneClasses();
		characterWorld = std::make_unique<ecs::Store>("current-body");
		auto &store = *characterWorld;
		scene::InstallServices(store);
		render::RegisterPresentationComponents();
		store.SetResource(render::DrawList{});
		const auto player = scene::AddPlayer(store, "viewer", false, 91);
		scene::CharacterDesc description;
		description.Frame.Position = {0, -2.5f, 0};
		const auto model = scene::MakeCharacter(store, description);
		REQUIRE(scene::SetPlayerCharacter(store, player, model));
		const auto *character = store.Get<scene::Character>(model);
		REQUIRE(character != nullptr);
		characterRoot = character->Root;
		characterHumanoid = character->Humanoid;
		store.Each<const scene::CharacterLimb>([&](ecs::Entity entity, const scene::CharacterLimb &limb) {
			limbOffsets.emplace_back(entity, limb.Offset);
		});
	}
	std::optional<assets::ContentHash> firstPoseHash;
	std::vector<scene::DrawInstance> bodyRows;
	std::vector<scene::DrawInstance> apertureRows;
	std::optional<uint64_t> warmTextures;
	for (size_t pose = 0; pose < 4; ++pose) {
		const float distance = pose == 1 ? 5.f : 3.f;
		CAPTURE(rotated, resident, distance, humanoid, pose, shaded);
		body.Frame = view.CameraFrame * core::CFrame(core::Vector3{.7f, 0, -distance});
		bodyRows.assign(1, body);
		if (humanoid) {
			auto &store = *characterWorld;
			const float swing = pose == 0 || pose == 3 ? -.7f : .7f;
			for (const auto &[entity, rest] : limbOffsets) {
				auto *limb = store.GetMutable<scene::CharacterLimb>(entity);
				limb->Offset = rest;
				if (store.InstanceNameOf(entity) == core::Name("Right Arm"))
					limb->Offset = rest * core::CFrame::Angles(swing, 0, 0);
			}
			scene::PoseCharacters(store);
			scene::CapturePreviousTransforms(store);
			scene::SyncRendered(store);
			render::CollectInstances(store);
			bodyRows = store.Resource<render::DrawList>()->Instances;
			REQUIRE(bodyRows.size() == 6);
			const core::Vector3 normal = view.CameraFrame.VectorToWorldSpace({1, 0, 0});
			const auto cut = view.CameraFrame.PointToWorldSpace({.7f, 0, -distance});
			scene::SeamTransform through;
			through.Frame = view.CameraFrame * core::CFrame(core::Vector3{.7f, 0, -distance});
			through.Scale = .5f;
			for (auto &row : bodyRows) {
				REQUIRE(row.Rig == characterRoot.Id);
				row.Frame = through.Place(row.Frame);
				row.HalfExtent = row.HalfExtent * through.Scale;
				row.CastShadow = false;
				row.EmissiveMap = emission;
				row.EmissiveTint = {0, 0, 1};
				if (shaded) {
					row.EmissiveMap = {};
					row.Tint = {0, 0, 1};
				}
				row.SeamNormal = normal;
				row.SeamOffset = normal.Dot(cut);
			}
		}
		auto together = bodyRows;
		together.push_back(wall);
		const auto reference = capture(referencePipeline, together);
		const auto composed = capture(compositionPipeline, bodyRows);
		if (humanoid && !opaqueScope && !shaded && pose == 2) {
			apertureRows = together;
		}
		const auto poseHash = assets::Hasher::Of(composed.Pixels);
		if (pose == 0) firstPoseHash = poseHash;
		if (pose == 2 && humanoid) CHECK(poseHash != *firstPoseHash);
		if (pose == 3) CHECK(poseHash == *firstPoseHash);
		const auto expectedColour = unpack(reference), actualColour = unpack(composed);
		if (shaded && pose == 0) {
			const auto uploads = renderer.PortalImageUsage().Uploads;
			const auto sunlight = view.Lighting.Direct;
			view.Lighting.Direct = {};
			const auto unlit = capture(compositionPipeline, bodyRows);
			view.Lighting.Direct = sunlight;
			const auto darkColour = unpack(unlit);
			size_t darkenedBody = 0, retainedWall = 0;
			for (size_t pixel = 0; pixel < actualColour.size(); pixel += 4) {
				if (actualColour[pixel + 2] > .5f && actualColour[pixel] < .1f) {
					CHECK(darkColour[pixel + 2] < .001f);
					++darkenedBody;
				}
				if (actualColour[pixel] > .5f && actualColour[pixel + 2] < .1f) {
					CHECK(darkColour[pixel] == actualColour[pixel]);
					++retainedWall;
				}
			}
			CHECK(darkenedBody > 0);
			CHECK(retainedWall > 0);
			CHECK(unlit.Depth == composed.Depth);
			CHECK(renderer.PortalImageUsage().Uploads == uploads);
			CHECK(capture(compositionPipeline, bodyRows).Pixels == composed.Pixels);
		}
		const render::test::ImageView expectedView{
			65, 37, render::test::ImageFormat::Rgba32Float, std::as_bytes(std::span(expectedColour)), 0
		};
		const render::test::ImageView actualView{
			65, 37, render::test::ImageFormat::Rgba32Float, std::as_bytes(std::span(actualColour)), 0
		};
		render::test::CheckImage(
			renderer, "body-depth-compose", "radiance", "opaque body and wall", expectedView, actualView
		);
		size_t red = 0, blue = 0;
		for (size_t pixel = 0; pixel < actualColour.size(); pixel += 4) {
			red += actualColour[pixel] > .5f && actualColour[pixel + 2] < .1f;
			blue += actualColour[pixel + 2] > .5f && actualColour[pixel] < .1f;
		}
		CHECK(red > 0);
		CHECK(blue > 0);
		if (humanoid && pose == 0) {
			auto uncut = together;
			for (auto &row : uncut) {
				row.SeamNormal = {};
				row.SeamOffset = 0;
			}
			const auto fullBody = unpack(capture(referencePipeline, uncut));
			size_t fullBlue = 0;
			for (size_t pixel = 0; pixel < fullBody.size(); pixel += 4)
				fullBlue += fullBody[pixel + 2] > .5f && fullBody[pixel] < .1f;
			CHECK(fullBlue > blue);
		}
		if (const char *output = std::getenv("MONO_RENDER_PREVIEW_DIR")) {
			const auto directory = std::filesystem::path(output);
			std::filesystem::create_directories(directory);
			const std::string stem = std::string(humanoid ? "humanoid-" : "block-") +
									 (rotated ? "rotated-" : "identity-") +
									 (shaded ? "sunlit-" : "emissive-") + std::to_string(pose);
			render::test::WriteImagePreview(directory / (stem + "-direct.ppm"), expectedView);
			render::test::WriteImagePreview(directory / (stem + "-composed.ppm"), actualView);
		}
		core::ByteReader expected(reference.Depth), actual(composed.Depth);
		REQUIRE(composed.Depth.size() == reference.Depth.size());
		while (!expected.AtEnd())
			CHECK(std::abs(expected.ReadFloat() - actual.ReadFloat()) < .0001f);
		const auto textures = renderer.MemoryStatistics().TextureAllocations;
		if (warmTextures)
			CHECK(textures == *warmTextures);
		else
			warmTextures = textures;
	}
	if (humanoid) {
		auto &store = *characterWorld;
		const auto eye = store.CreateInstance(scene::CameraClass(), "FirstPerson");
		store.Set(eye, scene::CameraSubject{characterHumanoid});
		store.SetResource(scene::ActiveCamera{eye});
		scene::CameraController controller;
		controller.Mode = scene::CameraMode::LockFirstPerson;
		store.SetResource(controller);
		render::SelectFirstPersonBody(store, view);
		REQUIRE(view.EyeRig == characterRoot.Id);
		REQUIRE(view.EyePlayer == 91);
		const auto hidden = capture(compositionPipeline, bodyRows);
		CHECK(hidden.Pixels == room.Pixels);
		CHECK(hidden.Depth == room.Depth);
		view.EyeRig = 0;
		view.EyePlayer.reset();
	}
	CHECK(renderer.PortalImageUsage().Uploads == (resident ? 0 : 2));
	if (!apertureRows.empty()) {
		for (const float scale : {.25f, 1.f, 4.f}) {
			CheckBodyAperture(renderer, view, apertureRows, rotated, scale, resident);
			render::SelectFirstPersonBody(*characterWorld, view);
			REQUIRE(view.EyeRig == characterRoot.Id);
			CheckBodyAperture(renderer, view, apertureRows, rotated, scale, resident);
			view.EyeRig = 0;
			view.EyePlayer.reset();
		}
	}
	REQUIRE(renderer.DropPortalImage(view.EyeImage));
	CHECK(renderer.PortalImageUsage().TextureBytes == 0);
	for (const bool colourOnly : {false, true}) {
		view.EyeImage = 0;
		if (colourOnly) {
			render::PortalImageReply unpaired;
			unpaired.Key = binding.Expected;
			unpaired.Scope = binding.ExpectedScope;
			unpaired.Status = render::PortalImageStatus::Ok;
			unpaired.Width = room.Width;
			unpaired.Height = room.Height;
			unpaired.RowStride = room.RowStride;
			unpaired.Pixels = room.Pixels;
			unpaired.PixelHash = assets::Hasher::Of(unpaired.Pixels);
			view.EyeImage = renderer.QueuePortalImage(binding, std::move(unpaired));
			REQUIRE(view.EyeImage != 0);
		}
		view.Pipeline = compositionPipeline;
		view.Instances = std::span(&body, 1);
		render::OverlayImage overlay;
		CHECK_FALSE(
			renderer.Render(std::span(&view, 1), overlay, nullptr, false).Ran(core::Name("body-compose"))
		);
		if (colourOnly) REQUIRE(renderer.DropPortalImage(view.EyeImage));
	}
}

TEST_CASE(
	"resident body exports reuse replaced image pairs", "[render][gpu][resourceimage][resident-reuse][.]"
) {
	render::test::FixtureDevice fixture;
	fixture.Initialise();
	auto &renderer = fixture.Render;
	const int planeSet = GENERATE(0, 1, 2);
	const bool ambient = planeSet != 0;
	const bool directional = planeSet == 2;
	const size_t pixelBytes = directional ? 64 : ambient ? 48 : 12;
	CAPTURE(planeSet);
	InstallImageCapture(renderer, "lit", true, ambient, directional);
	assets::TextureData white;
	white.Width = white.Height = 1;
	white.Format = assets::TextureFormat::RGBA8;
	white.Pixels.assign(4, std::byte{255});
	const core::Name emission("resident-reuse-emission");
	REQUIRE(renderer.AddTexture(emission, white));
	render::SceneTarget target{65, 37};
	render::View view;
	view.Target = &target;
	view.WorldName = core::Name("resident-reuse");
	view.OverrideLighting = true;
	view.Lighting.Ambient = {};
	view.Lighting.OutdoorAmbient = {};
	view.Lighting.Direct = {};
	view.Pipeline = core::Name("image-export-pipeline");
	scene::DrawInstance body;
	body.Source = 1;
	body.HalfExtent = {.5f, .5f, .5f};
	body.CastShadow = false;
	body.Tint = {};
	body.EmissiveMap = emission;
	view.Instances = std::span(&body, 1);
	render::PortalImageBinding binding;
	binding.WorldName = view.WorldName;
	binding.Portal = core::Name("current-body");
	binding.Expected.PortalKey = "current-body";
	binding.ExpectedScope = render::PortalImageScope::OpaqueLighting;
	binding.ExpectedProjection = render::PortalImageProjection::Eye;
	uint64_t imported = 0, warmAllocations = 0;
	for (uint64_t frame = 0; frame < 12; ++frame) {
		body.Frame.Position = {0, 0, frame % 2 ? -5.f : -3.f};
		body.EmissiveTint = frame % 2 ? core::Color3{0, 1, 0} : core::Color3{1, 0, 0};
		const auto token = renderer.QueueResourceImage(
			view.Pipeline, core::Name("image-export"), 0, render::ResourceImageDelivery::Resident
		);
		REQUIRE(token != 0);
		// The copy shares the resident capture's fence. AwaitImage has a ten-second
		// deadline, so the next reuse sample starts after this submission completes.
		const auto completion = renderer.QueueResourceImage(view.Pipeline, core::Name("image-export"));
		REQUIRE(completion != 0);
		render::OverlayImage overlay;
		REQUIRE(
			renderer.Render(std::span(&view, 1), overlay, nullptr, false).Ran(core::Name("image-export"))
		);
		binding.Expected.RequestId = frame + 1;
		imported = renderer.AdoptResourceImage(token, binding);
		REQUIRE(imported != 0);
		REQUIRE(AwaitImage(renderer, completion).Status == render::ResourceImageStatus::Ok);
		CHECK(renderer.PortalImageUsage().Images == 1);
		CHECK(renderer.PortalImageUsage().Uploads == 0);
		CHECK(renderer.PortalImageUsage().PendingCpuBytes == 0);
		const auto allocations = renderer.MemoryStatistics().TextureAllocations;
		if (frame > 0) CHECK(renderer.PortalImageUsage().CachedTextureBytes == size_t(65) * 37 * pixelBytes);
		if (frame == 3) warmAllocations = allocations;
		if (frame > 3) CHECK(allocations == warmAllocations);
	}
	// More shapes than cache slots exercise bounded eviction and exact extent matching.
	body.Frame.Position = {0, 0, -7};
	body.EmissiveTint = {0, 0, 1};
	for (const uint32_t width : {33u, 17u, 9u, 5u, 65u}) {
		target.Width = width;
		const auto token = renderer.QueueResourceImage(
			view.Pipeline, core::Name("image-export"), 0, render::ResourceImageDelivery::Resident
		);
		REQUIRE(token != 0);
		// The copy shares the resident capture's fence. AwaitImage has a ten-second
		// deadline, so the next reuse sample starts after this submission completes.
		const auto completion = renderer.QueueResourceImage(view.Pipeline, core::Name("image-export"));
		REQUIRE(completion != 0);
		render::OverlayImage overlay;
		REQUIRE(
			renderer.Render(std::span(&view, 1), overlay, nullptr, false).Ran(core::Name("image-export"))
		);
		binding.Expected.RequestId++;
		imported = renderer.AdoptResourceImage(token, binding);
		REQUIRE(imported != 0);
		REQUIRE(AwaitImage(renderer, completion).Status == render::ResourceImageStatus::Ok);
		CHECK(renderer.PortalImageUsage().CachedTextureBytes <= size_t(4) * 65 * 37 * pixelBytes);
		CHECK(renderer.PortalImageUsage().Images == 1);
		CHECK(renderer.PortalImageUsage().Uploads == 0);
	}
	graph::RenderGraph graph;
	core::Name offender;
	REQUIRE(
		graph::Build(graph::DefaultPortalBodyDocument(), graph, offender) == graph::PipelineDocumentStatus::Ok
	);
	view.Pipeline = core::Name("resident-body-read");
	REQUIRE(renderer.SetPipeline(view.Pipeline, graph));
	view.Instances = {};
	view.EyeImage = imported;
	view.EyeImageKey = binding.Portal;
	const auto read = renderer.QueueResourceImage(view.Pipeline, core::Name("export"));
	REQUIRE(read != 0);
	render::OverlayImage overlay;
	REQUIRE(renderer.Render(std::span(&view, 1), overlay, nullptr, false).Ran(core::Name("export")));
	const auto image = AwaitImage(renderer, read);
	const size_t centre = 18 * image.Width + 32;
	core::ByteReader colour(std::span(image.Pixels).subspan(centre * 8, 8));
	const auto rg = glm::unpackHalf2x16(colour.ReadUInt32());
	CHECK(rg.x < .001f);
	CHECK(rg.y < .001f);
	const auto ba = glm::unpackHalf2x16(colour.ReadUInt32());
	CHECK(ba.x > .5f);
	core::ByteReader depth(std::span(image.Depth).subspan(centre * 4, 4));
	CHECK(std::abs(depth.ReadFloat() - 6.5f) < .001f);
	REQUIRE(renderer.DropPortalImage(imported));
	CHECK(renderer.PortalImageUsage().TextureBytes == 0);
	CHECK(renderer.PortalImageUsage().CachedTextureBytes == 0);
}

TEST_CASE(
	"incoming portal images evict optional resident cache", "[render][gpu][resourceimage][resident-budget][.]"
) {
	render::test::FixtureDevice fixture;
	fixture.Initialise();
	auto &renderer = fixture.Render;
	InstallImageCapture(renderer, "lit", true);
	render::SceneTarget target{512, 512};
	render::View view;
	view.Target = &target;
	view.WorldName = core::Name("resident-budget");
	view.Pipeline = core::Name("image-export-pipeline");
	render::OverlayImage overlay;
	constexpr size_t mib = 1024 * 1024;
	auto bindingFor = [&](std::string_view name) {
		render::PortalImageBinding binding;
		binding.WorldName = view.WorldName;
		binding.Portal = core::Name(name);
		binding.Expected.PortalKey = name;
		binding.Expected.RequestId = 1;
		return binding;
	};
	auto adopt = [&](std::string_view name) {
		const auto token = renderer.QueueResourceImage(
			view.Pipeline, core::Name("image-export"), 0, render::ResourceImageDelivery::Resident
		);
		REQUIRE(token != 0);
		REQUIRE(
			renderer.Render(std::span(&view, 1), overlay, nullptr, false).Ran(core::Name("image-export"))
		);
		const auto handle = renderer.AdoptResourceImage(token, bindingFor(name));
		REQUIRE(handle != 0);
		return handle;
	};
	auto queueCopied = [&](std::string_view name) {
		const auto binding = bindingFor(name);
		render::PortalImageReply reply;
		reply.Key = binding.Expected;
		reply.Scope = binding.ExpectedScope;
		reply.Status = render::PortalImageStatus::Ok;
		reply.Width = reply.Height = 512;
		reply.RowStride = 512 * 8;
		reply.Pixels.resize(2 * mib);
		reply.Depth.resize(mib);
		reply.PixelHash = assets::Hasher::Of(reply.Pixels);
		reply.DepthHash = assets::Hasher::Of(reply.Depth);
		const auto handle = renderer.QueuePortalImage(binding, std::move(reply));
		REQUIRE(handle != 0);
		return handle;
	};
	uint64_t resident = adopt("resident");
	resident = adopt("resident");
	REQUIRE(renderer.PortalImageUsage().CachedTextureBytes == 3 * mib);
	std::array<uint64_t, 9> copied{};
	for (size_t index = 0; index < copied.size(); ++index)
		copied[index] = queueCopied("copied-" + std::to_string(index));
	REQUIRE(renderer.PortalImageUsage().PendingCpuBytes == 27 * mib);
	renderer.Render(std::span(&view, 1), overlay, nullptr, false);
	CHECK(renderer.PortalImageUsage().Uploads == 18);
	CHECK(renderer.PortalImageUsage().TextureBytes == 30 * mib);
	CHECK(renderer.PortalImageUsage().CachedTextureBytes == 0);
	CHECK(renderer.PortalImageUsage().PendingCpuBytes == 0);

	// A paired cache entry cannot satisfy a colour-only export. Adoption must
	// evict that optional pair when the incoming live image needs its space.
	REQUIRE(renderer.DropPortalImage(copied.back()));
	InstallImageCapture(renderer, "lit", false);
	resident = adopt("resident");
	REQUIRE(renderer.PortalImageUsage().TextureBytes == 26 * mib);
	REQUIRE(renderer.PortalImageUsage().CachedTextureBytes == 3 * mib);
	copied.back() = queueCopied("copied-8");
	renderer.Render(std::span(&view, 1), overlay, nullptr, false);
	REQUIRE(renderer.PortalImageUsage().TextureBytes == 29 * mib);
	REQUIRE(renderer.PortalImageUsage().CachedTextureBytes == 3 * mib);
	const auto other = adopt("other-resident");
	CHECK(renderer.PortalImageUsage().Images == 11);
	CHECK(renderer.PortalImageUsage().TextureBytes == 31 * mib);
	CHECK(renderer.PortalImageUsage().CachedTextureBytes == 0);
	for (const auto handle : copied)
		REQUIRE(renderer.DropPortalImage(handle));
	REQUIRE(renderer.DropPortalImage(resident));
	REQUIRE(renderer.DropPortalImage(other));
	CHECK(renderer.PortalImageUsage().Images == 0);
	CHECK(renderer.PortalImageUsage().TextureBytes == 0);
	CHECK(renderer.PortalImageUsage().CachedTextureBytes == 0);
	CHECK(renderer.PortalImageUsage().PendingCpuBytes == 0);
}

TEST_CASE(
	"ordered transparent layers preserve opaque body depth",
	"[render][gpu][resourceimage][transparent-depth-compose][.]"
) {
	render::test::FixtureDevice fixture;
	fixture.Initialise();
	auto &renderer = fixture.Render;
	InstallImageCapture(renderer, "lit", true);
	graph::PipelineDocument document;
	const auto opaqueDocument = graph::DefaultPortalBodyDocument();
	for (const auto &edit : opaqueDocument.Edits()) {
		document.Record(edit);
		if (edit.Kind == graph::EditKind::AddNode && edit.Name == core::Name("body-compose"))
			document.Record(
				{.Kind = graph::EditKind::Set, .Key = core::Name("mode"), .Value = "transparent"}
			);
	}
	graph::RenderGraph graph;
	core::Name offender;
	REQUIRE(graph::Build(document, graph, offender) == graph::PipelineDocumentStatus::Ok);
	const core::Name composition("transparent-depth-compose");
	REQUIRE(renderer.SetPipeline(composition, graph));
	assets::TextureData texture;
	texture.Width = texture.Height = 1;
	texture.Format = assets::TextureFormat::RGBA8;
	texture.Pixels.assign(4, std::byte{255});
	const core::Name emission("layer-emission"), mask("layer-alpha");
	REQUIRE(renderer.AddTexture(emission, texture));
	const uint8_t opacity = GENERATE(uint8_t{0}, uint8_t{128}, uint8_t{255});
	texture.Pixels[3] = std::byte{opacity};
	REQUIRE(renderer.AddTexture(mask, texture));
	render::SceneTarget target{33, 33};
	render::View view;
	view.Target = &target;
	view.WorldName = core::Name("layer-world");
	view.OverrideLighting = true;
	view.Lighting.Ambient = {};
	view.Lighting.OutdoorAmbient = {};
	view.Lighting.Direct = {};
	scene::DrawInstance layer;
	layer.Source = 1;
	layer.HalfExtent = {.5f, .5f, .5f};
	layer.CastShadow = false;
	layer.Tint = {};
	layer.Texture = mask;
	layer.Alpha = scene::AlphaMode::Transparency;
	layer.AlphaCutoff = 0;
	layer.EmissiveMap = emission;
	view.Instances = std::span(&layer, 1);
	const auto capture = [&](core::Name pipeline, core::Name node) {
		view.Pipeline = pipeline;
		const auto token = renderer.QueueResourceImage(pipeline, node);
		REQUIRE(token != 0);
		render::OverlayImage overlay;
		REQUIRE(renderer.Render(std::span(&view, 1), overlay, nullptr, false).Ran(node));
		return AwaitImage(renderer, token);
	};
	const auto colourAt = [](const render::ResourceImage &image, size_t pixel) {
		core::ByteReader reader(std::span(image.Pixels).subspan(pixel * 8, 8));
		const auto rg = glm::unpackHalf2x16(reader.ReadUInt32());
		return glm::vec4(rg, glm::unpackHalf2x16(reader.ReadUInt32()));
	};
	const auto depthAt = [](const render::ResourceImage &image, size_t pixel) {
		core::ByteReader reader(std::span(image.Depth).subspan(pixel * 4, 4));
		return reader.ReadFloat();
	};
	const float opaqueDepth = GENERATE(0.f, 2.f, 2.5f, 5.f);
	const float backgroundOpacity = opaqueDepth == 0 ? GENERATE(0.f, 1.f) : 1.f;
	CAPTURE(opaqueDepth, opacity, backgroundOpacity);
	render::ResourceImage background;
	background.Width = background.Height = 33;
	background.RowStride = 33 * 8;
	core::ByteWriter pixels, depths;
	for (size_t pixel = 0; pixel < 33 * 33; ++pixel) {
		pixels.WriteUInt32(glm::packHalf2x16({0, 0}));
		pixels.WriteUInt32(glm::packHalf2x16({2 * backgroundOpacity, backgroundOpacity}));
		depths.WriteFloat(opaqueDepth);
	}
	background.Pixels.assign(pixels.Bytes().begin(), pixels.Bytes().end());
	background.Depth.assign(depths.Bytes().begin(), depths.Bytes().end());
	render::PortalImageBinding binding;
	binding.WorldName = view.WorldName;
	binding.Portal = core::Name("layer-background");
	binding.Expected.PortalKey = "layer-background";
	binding.ExpectedScope = render::PortalImageScope::OpaqueLighting;
	binding.ExpectedProjection = render::PortalImageProjection::Eye;
	view.EyeImageKey = binding.Portal;
	// Two layers are applied back to front over a body, a far wall or empty depth.
	for (int index = 0; index < 2; ++index) {
		layer.Frame.Position = {0, 0, index == 0 ? -4.f : -3.f};
		layer.EmissiveTint = index == 0 ? core::Color3{0, 4, 0} : core::Color3{4, 0, 0};
		const auto foreground = capture(core::Name("image-export-pipeline"), core::Name("image-export"));
		const auto centre = colourAt(foreground, 16 * 33 + 16);
		REQUIRE(std::abs(centre.a - float(opacity) / 255) < .001f);
		render::PortalImageReply reply;
		binding.Expected.RequestId++;
		reply.Key = binding.Expected;
		reply.Scope = binding.ExpectedScope;
		reply.Status = render::PortalImageStatus::Ok;
		reply.Width = background.Width;
		reply.Height = background.Height;
		reply.RowStride = background.RowStride;
		reply.Pixels = background.Pixels;
		reply.Depth = background.Depth;
		reply.PixelHash = assets::Hasher::Of(reply.Pixels);
		reply.DepthHash = assets::Hasher::Of(reply.Depth);
		view.EyeImage = renderer.QueuePortalImage(binding, std::move(reply));
		REQUIRE(view.EyeImage != 0);
		auto composed = capture(composition, core::Name("export"));
		std::vector<glm::vec4> expectedPixels(33 * 33), actualPixels(33 * 33);
		for (size_t pixel = 0; pixel < 33 * 33; ++pixel) {
			const auto front = colourAt(foreground, pixel), back = colourAt(background, pixel);
			const float distance = depthAt(foreground, pixel);
			// The independent PBR capture clears depth to the far plane;
			// the compositing graph exports that empty sample as zero.
			const bool visible = distance > 0 && distance < view.Camera.FarPlane &&
								 (opaqueDepth == 0 || distance < opaqueDepth);
			const float alpha = visible ? front.a : 0;
			const glm::vec4 expected(
				glm::vec3(front) * alpha + glm::vec3(back) * (1 - alpha), alpha + back.a * (1 - alpha)
			);
			const auto actual = colourAt(composed, pixel);
			expectedPixels[pixel] = expected;
			actualPixels[pixel] = actual;
			for (int channel = 0; channel < 4; ++channel)
				CHECK(std::abs(actual[channel] - expected[channel]) < .004f);
			CHECK(depthAt(composed, pixel) == opaqueDepth);
		}
		if (const char *output = std::getenv("MONO_RENDER_PREVIEW_DIR");
			output && opacity == 128 && index == 1 && backgroundOpacity == 1) {
			const auto directory = std::filesystem::path(output);
			std::filesystem::create_directories(directory);
			const auto stem = "transparent-depth-" + std::to_string(opaqueDepth);
			for (const bool expected : {true, false}) {
				const auto &samples = expected ? expectedPixels : actualPixels;
				const render::test::ImageView preview{
					33, 33, render::test::ImageFormat::Rgba32Float, std::as_bytes(std::span(samples)), 0
				};
				render::test::WriteImagePreview(
					directory / (stem + (expected ? "-expected.ppm" : "-actual.ppm")), preview
				);
			}
		}
		background = std::move(composed);
	}
	REQUIRE(renderer.DropPortalImage(view.EyeImage));
}

TEST_CASE(
	"peeled glass layers match direct transparent drawing and expose overflow",
	"[render][gpu][resourceimage][transparent-layer][.]"
) {
	using namespace graph;
	render::test::FixtureDevice fixture;
	fixture.Initialise();
	auto &renderer = fixture.Render;
	PipelineDocument document;
	const auto worldDocument = DefaultWorldHdrDocument();
	for (const auto &edit : worldDocument.Edits()) {
		if (edit.Kind == EditKind::AddNode && edit.Name == core::Name("present")) break;
		document.Record(edit);
	}
	const auto resource =
		[&](const std::string &name, ResourceFormat format, ResourceKind kind = ResourceKind::Colour) {
			document.Record(
				{.Kind = EditKind::AddResource, .Name = core::Name(name), .Resource = kind, .Format = format}
			);
		};
	const auto node = [&](const std::string &name, const char *kind, NodeScope scope = NodeScope::View) {
		document.Record(
			{.Kind = EditKind::AddNode,
			 .Name = core::Name(name),
			 .NodeKind = core::Name(kind),
			 .Scope = scope}
		);
	};
	const auto edge = [&](EditKind kind, const std::string &name, const char *port) {
		document.Record({.Kind = kind, .Target = core::Name(name), .Key = core::Name(port)});
	};
	for (int layer = 0; layer < 3; ++layer) {
		const auto name = "glass-" + std::to_string(layer);
		resource(name, ResourceFormat::RGBA16F);
		resource(name + "-depth", ResourceFormat::R32F);
		resource(name + "-z", ResourceFormat::D32F, ResourceKind::Depth);
		node(name, "transparent-layer");
		edge(EditKind::Reads, "depth", "opaque-z");
		if (layer > 0) edge(EditKind::Reads, "glass-" + std::to_string(layer - 1) + "-z", "previous-z");
		edge(EditKind::Reads, "ordered-entities", "entities");
		edge(EditKind::Reads, "view-instances", "instances");
		edge(EditKind::Reads, "shadow", "shadow");
		edge(EditKind::Writes, name, "colour");
		edge(EditKind::Writes, name + "-depth", "depth");
		edge(EditKind::Writes, name + "-z", "z");
	}
	for (int layer = 1; layer >= 0; --layer) {
		const auto name = "compose-" + std::to_string(layer);
		resource(name, ResourceFormat::RGBA16F);
		resource(name + "-depth", ResourceFormat::R32F);
		node(name, "depth-compose");
		document.Record({.Kind = EditKind::Set, .Key = core::Name("mode"), .Value = "premultiplied"});
		edge(EditKind::Reads, "glass-" + std::to_string(layer), "foreground");
		edge(EditKind::Reads, "glass-" + std::to_string(layer) + "-depth", "foreground-depth");
		edge(EditKind::Reads, layer == 1 ? "mirrored" : "compose-1", "background");
		edge(EditKind::Reads, layer == 1 ? "linear-depth" : "compose-1-depth", "background-depth");
		edge(EditKind::Writes, name, "colour");
		edge(EditKind::Writes, name + "-depth", "depth");
	}
	for (int layer = 0; layer < 3; ++layer) {
		const auto name = "glass-" + std::to_string(layer);
		node(name + "-export", "capture", NodeScope::Frame);
		edge(EditKind::Reads, name, "source");
		edge(EditKind::Reads, name + "-depth", "depth");
	}
	for (const bool direct : {true, false}) {
		node(direct ? "direct-export" : "composed-export", "capture", NodeScope::Frame);
		edge(EditKind::Reads, direct ? "display" : "compose-0", "source");
	}
	node("room-export", "capture", NodeScope::Frame);
	edge(EditKind::Reads, "lit", "source");
	edge(EditKind::Reads, "linear-depth", "depth");
	node("native-lens-export", "capture", NodeScope::Frame);
	edge(EditKind::Reads, "lens-b", "source");
	edge(EditKind::Reads, "linear-depth", "depth");
	RenderGraph graph;
	core::Name offender;
	REQUIRE(Build(document, graph, offender) == PipelineDocumentStatus::Ok);
	const core::Name pipeline("glass-peeling");
	REQUIRE(renderer.SetPipeline(pipeline, graph));
	assets::MeshData plane;
	plane.Vertices = {
		{{-1, -1, 0}, {0, 0, 1}, {0, 1}},
		{{1, -1, 0}, {0, 0, 1}, {1, 1}},
		{{1, 1, 0}, {0, 0, 1}, {1, 0}},
		{{-1, 1, 0}, {0, 0, 1}, {0, 0}}
	};
	plane.Indices = {0, 1, 2, 0, 2, 3};
	plane.ComputeBounds();
	const core::Name mesh("glass-plane"), emission("glass-white");
	REQUIRE(renderer.AddMesh(mesh, plane));
	assets::TextureData white;
	white.Width = white.Height = 1;
	white.Format = assets::TextureFormat::RGBA8;
	white.Pixels.assign(4, std::byte{255});
	REQUIRE(renderer.AddTexture(emission, white));
	const int scenario = GENERATE(0, 1, 2, 3, 4, 5);
	const bool rotated = GENERATE(false, true);
	const bool offsetCoplanar = scenario == 3 ? GENERATE(false, true) : false;
	CAPTURE(scenario, rotated, offsetCoplanar);
	INFO(
		(scenario == 5
			 ? "intersecting panes: centre=(0,0,-3), half extent=(1,1), red yaw=+.35, green yaw=-.35 radians"
			 : "parallel panes")
	);
	std::vector<scene::DrawInstance> rows;
	for (int index = 0; index < (scenario == 2 ? 3 : 2); ++index) {
		scene::DrawInstance row;
		row.Source = index + 1;
		row.Mesh = mesh;
		row.HalfExtent = {1, 1, 1};
		row.Frame.Position = {0, 0, -3.f - index};
		row.Transparency = .5f;
		row.CastShadow = false;
		row.Tint = {};
		row.EmissiveMap = emission;
		row.EmissiveStrength = 4;
		row.EmissiveTint = index == 0	? core::Color3{1, 0, 0}
						   : index == 1 ? core::Color3{0, 1, 0}
										: core::Color3{0, 0, 1};
		rows.push_back(row);
	}
	if (scenario == 3) {
		rows.back().Frame.Position.Z = -3;
		if (offsetCoplanar) {
			rows.back().Frame.Position.X = .25f;
			rows.back().Frame.Position.Y = .125f;
		}
	}
	if (scenario == 5) {
		INFO("intersecting panes: centre=(0,0,-3), half extent=(1,1), red yaw=+.35, green yaw=-.35 radians");
		rows[0].Frame = core::CFrame(core::Vector3{0, 0, -3}) * core::CFrame::Angles(0, .35f, 0);
		rows[1].Frame = core::CFrame(core::Vector3{0, 0, -3}) * core::CFrame::Angles(0, -.35f, 0);
	}
	if (scenario == 1 || scenario == 4) {
		auto body = rows.front();
		body.Source = 9;
		body.Frame.Position.Z = scenario == 4 ? -3.f : -3.5f;
		body.Transparency = 0;
		body.EmissiveTint = {0, 0, 1};
		rows.push_back(body);
	}
	render::SceneTarget target{33, 33};
	render::View view;
	view.Pipeline = pipeline;
	view.WorldName = core::Name("glass-world");
	view.Target = &target;
	if (rotated) {
		view.CameraFrame = core::CFrame(core::Vector3{7, 2, 3}) * core::CFrame::Angles(-.2f, .37f, .1f);
		for (auto &row : rows)
			row.Frame = view.CameraFrame * row.Frame;
	}
	view.Instances = rows;
	view.OverrideLighting = true;
	view.Lighting.Ambient = {};
	view.Lighting.OutdoorAmbient = {};
	view.Lighting.Direct = {};
	const std::array<uint64_t, 4> tokens{100, 101, 102, 103};
	std::array<render::ResourceImageRequest, 4> requests;
	for (size_t layer = 0; layer < 3; ++layer)
		requests[layer] = {tokens[layer], pipeline, core::Name("glass-" + std::to_string(layer) + "-export")};
	requests[3] = {tokens[3], pipeline, core::Name("direct-export")};
	REQUIRE(renderer.RequestResourceImages(requests));
	CHECK_FALSE(renderer.TakeResourceImages(tokens));
	render::OverlayImage overlay;
	REQUIRE(renderer.Render(std::span(&view, 1), overlay, nullptr, false).Ran(core::Name("glass-2")));
	const auto captured = AwaitImageGroup(renderer, tokens);
	REQUIRE(captured.front().CaptureFrame != 0);
	for (const auto &image : captured)
		CHECK(image.CaptureFrame == captured.front().CaptureFrame);
	std::array<float, 3> centreDepths;
	for (size_t layer = 0; layer < 3; ++layer) {
		core::ByteReader depth(std::span(captured[layer].Depth).subspan((16 * 33 + 16) * 4, 4));
		centreDepths[layer] = depth.ReadFloat();
	}
	const float alpha = 128.f / 255;
	float transmittance = 1;
	for (size_t layer = 0; layer < 3; ++layer) {
		const float expected = scenario == 4								  ? 0.f
							   : layer == 0									  ? 3.f
							   : layer == 1 && scenario != 1 && scenario != 3 ? 4.f
							   : layer == 2 && scenario == 2				  ? 5.f
																			  : 0.f;
		if (((scenario == 3 && offsetCoplanar) || scenario == 5) && layer == 1) {
			// Coplanar faces and the intersection centre can round to adjacent depths.
			// They may occupy two groups, but neither colour nor opacity may disappear.
			CHECK((centreDepths[layer] == 0 || std::abs(centreDepths[layer] - 3) < .001f));
		} else
			CHECK(std::abs(centreDepths[layer] - expected) < .001f);
		core::ByteReader colour(std::span(captured[layer].Pixels).subspan((16 * 33 + 16) * 8, 8));
		const auto rg = glm::unpackHalf2x16(colour.ReadUInt32());
		const auto ba = glm::unpackHalf2x16(colour.ReadUInt32());
		transmittance *= 1 - ba.y;
		if (centreDepths[layer] != 0) {
			const float expectedAlpha = (scenario == 3 || scenario == 5) && layer == 0 && centreDepths[1] == 0
											? 1 - (1 - alpha) * (1 - alpha)
											: alpha;
			CHECK(std::abs(ba.y - expectedAlpha) < .001f);
			CHECK(
				((scenario == 3 || scenario == 5) ? std::max(rg.x, rg.y)
				 : layer == 0					  ? rg.x
				 : layer == 1					  ? rg.y
												  : ba.x) > 1.9f
			);
		} else {
			CHECK(rg == glm::vec2(0));
			CHECK(ba == glm::vec2(0));
		}
	}
	if (scenario == 5) {
		// Samples straddle the intersection, with a physical depth separation
		// well above float rounding. The nearest retained colour must reverse.
		for (const uint32_t x : {14u, 18u}) {
			CAPTURE(x);
			const size_t pixel = 16 * 33 + x;
			core::ByteReader nearest(std::span(captured[0].Pixels).subspan(pixel * 8, 8));
			const auto redGreen = glm::unpackHalf2x16(nearest.ReadUInt32());
			CHECK((x < 16 ? redGreen.x : redGreen.y) > 1.9f);
			CHECK((x < 16 ? redGreen.y : redGreen.x) == 0);
			core::ByteReader front(std::span(captured[0].Depth).subspan(pixel * 4, 4));
			core::ByteReader back(std::span(captured[1].Depth).subspan(pixel * 4, 4));
			const float nearDepth = front.ReadFloat(), farDepth = back.ReadFloat();
			CHECK(nearDepth < 2.95f);
			CHECK(farDepth > 3.05f);
			core::ByteReader overflow(std::span(captured[2].Depth).subspan(pixel * 4, 4));
			CHECK(overflow.ReadFloat() == 0);
		}
	}
	const int visiblePanes = scenario == 4 ? 0 : scenario == 1 ? 1 : scenario == 2 ? 3 : 2;
	CHECK(std::abs((1 - transmittance) - (1 - std::pow(1 - alpha, visiblePanes))) < .001f);
	if (scenario != 2) {
		const auto token = renderer.QueueResourceImage(pipeline, core::Name("composed-export"));
		REQUIRE(token != 0);
		REQUIRE(renderer.Render(std::span(&view, 1), overlay, nullptr, false).Ran(core::Name("compose-0")));
		const auto composed = AwaitImage(renderer, token);
		core::ByteReader expected(captured[3].Pixels), actual(composed.Pixels);
		std::vector<float> directPixels, composedPixels;
		const bool perPixelOrder = scenario == 5 || (scenario == 3 && rotated && offsetCoplanar);
		size_t reorderedPixels = 0;
		while (!expected.AtEnd()) {
			const auto directRedGreen = glm::unpackHalf2x16(expected.ReadUInt32());
			const auto directBlueAlpha = glm::unpackHalf2x16(expected.ReadUInt32());
			const auto layeredRedGreen = glm::unpackHalf2x16(actual.ReadUInt32());
			const auto layeredBlueAlpha = glm::unpackHalf2x16(actual.ReadUInt32());
			directPixels.insert(
				directPixels.end(), {directRedGreen.x, directRedGreen.y, directBlueAlpha.x, directBlueAlpha.y}
			);
			composedPixels.insert(
				composedPixels.end(),
				{layeredRedGreen.x, layeredRedGreen.y, layeredBlueAlpha.x, layeredBlueAlpha.y}
			);
			if (perPixelOrder) {
				// Direct transparency has one object order. Intersecting and raster-drift coplanar
				// panes need per-pixel ordering, so compare the complete contributions independent
				// of which pane is in front at a pixel.
				CHECK(
					std::abs(
						std::min(directRedGreen.x, directRedGreen.y) -
						std::min(layeredRedGreen.x, layeredRedGreen.y)
					) < .004f
				);
				CHECK(
					std::abs(
						std::max(directRedGreen.x, directRedGreen.y) -
						std::max(layeredRedGreen.x, layeredRedGreen.y)
					) < .004f
				);
				if (std::abs(directRedGreen.x - layeredRedGreen.x) >= .004f ||
					std::abs(directRedGreen.y - layeredRedGreen.y) >= .004f)
					++reorderedPixels;
			} else {
				CHECK(std::abs(directRedGreen.x - layeredRedGreen.x) < .004f);
				CHECK(std::abs(directRedGreen.y - layeredRedGreen.y) < .004f);
			}
			CHECK(std::abs(directBlueAlpha.x - layeredBlueAlpha.x) < .004f);
			CHECK(std::abs(directBlueAlpha.y - layeredBlueAlpha.y) < .004f);
		}
		if (perPixelOrder) {
			CHECK(reorderedPixels > 0);
			core::ByteReader overflow(captured[2].Depth);
			while (!overflow.AtEnd())
				CHECK(overflow.ReadFloat() == 0);
		}
		if (const char *output = std::getenv("MONO_RENDER_PREVIEW_DIR")) {
			const auto directory = std::filesystem::path(output);
			std::filesystem::create_directories(directory);
			const auto stem = "peeled-glass-" + std::to_string(scenario) + (rotated ? "-rotated" : "-front") +
							  (offsetCoplanar ? "-offset" : "");
			for (const bool direct : {true, false}) {
				const auto &samples = direct ? directPixels : composedPixels;
				const render::test::ImageView preview{
					33, 33, render::test::ImageFormat::Rgba32Float, std::as_bytes(std::span(samples)), 0
				};
				render::test::WriteImagePreview(
					directory / (stem + (direct ? "-direct.ppm" : "-composed.ppm")), preview
				);
			}
		}
	}
	if (scenario == 0) {
		const auto savedView = view;
		render::PortalCaptureLighting captureLighting;
		std::array<render::SceneLight, render::MAX_PORTAL_CAPTURE_LIGHTS> captureLights{};
		REQUIRE(render::ResolvePortalCaptureLighting(captureLighting, view, captureLights));
		auto roomRows = rows;
		auto wall = rows.front();
		wall.Source = 20;
		wall.Frame = view.CameraFrame * core::CFrame(core::Vector3{0, 0, -6});
		wall.HalfExtent = {20, 20, 1};
		wall.Transparency = 0;
		wall.EmissiveTint = {0, 0, 1};
		wall.EmissiveStrength = 2;
		roomRows.push_back(wall);
		view.Instances = roomRows;
		view.Damage = {.Scene = true, .Objects = true, .Environment = true, .Portals = true};
		const std::array roomNodes{
			core::Name("room-export"), core::Name("glass-0-export"), core::Name("glass-1-export")
		};
		std::array<uint64_t, 3> roomTokens{};
		REQUIRE(renderer.QueueResourceImages(
			pipeline, roomNodes, view.Slot, render::ResourceImageDelivery::CopiedPixels, roomTokens
		));
		REQUIRE(renderer.Render(std::span(&view, 1), overlay, nullptr, false).Ran(core::Name("room-export")));
		auto roomImages = AwaitImageGroup(renderer, roomTokens);
		render::PortalImageLayerSet roomLayers;
		for (size_t layer = 0; layer < roomImages.size(); ++layer) {
			auto &reply = layer == 0 ? roomLayers.Opaque : roomLayers.Transparent.emplace_back();
			auto &image = roomImages[layer];
			REQUIRE(image.Status == render::ResourceImageStatus::Ok);
			CHECK(image.CaptureFrame == roomImages.front().CaptureFrame);
			reply.Key = {1, "native-glass-lens", 1, 1};
			reply.Scope = render::PortalImageScope::OpaqueLighting;
			reply.Status = render::PortalImageStatus::Ok;
			reply.CaptureTick = reply.ContentRevision = reply.LightingRevision = 1;
			reply.Width = image.Width;
			reply.Height = image.Height;
			reply.RowStride = image.RowStride;
			reply.Pixels = std::move(image.Pixels);
			reply.Depth = std::move(image.Depth);
			reply.PixelHash = assets::Hasher::Of(reply.Pixels);
			reply.DepthHash = assets::Hasher::Of(reply.Depth);
			reply.CaptureLighting = captureLighting;
		}
		render::PortalImageCapture accepted;
		accepted.Producer.World = "native-glass-lens-owner";
		accepted.Width = accepted.Height = 33;
		accepted.CaptureLighting = captureLighting;
		const auto rotation = view.CameraFrame.Rotation();
		accepted.Camera.Position = {
			view.CameraFrame.Position.X, view.CameraFrame.Position.Y, view.CameraFrame.Position.Z
		};
		accepted.Camera.Orientation = {rotation.x, rotation.y, rotation.z, rotation.w};
		const float near = view.Camera.NearPlane;
		const float half = near * std::tan(view.Camera.FieldOfViewRadians / 2);
		accepted.Camera.Frustum = {-half, half, -half, half, near, view.Camera.FarPlane};
		accepted.Binding.World = view.World;
		accepted.Binding.WorldName = view.WorldName;
		accepted.Binding.ViewSlot = view.Slot;
		accepted.Binding.Portal = core::Name("native-glass-lens");
		accepted.Binding.Expected = roomLayers.Opaque.Key;
		accepted.Binding.ExpectedScope = render::PortalImageScope::OpaqueLighting;
		accepted.Binding.ExpectedProjection = render::PortalImageProjection::Eye;
		accepted.Binding.Sampling = scene::ResolveCamera(view.CameraFrame, view.Camera, 1).ViewProjection;
		std::array<uint64_t, 3> roomHandles{};
		REQUIRE(renderer.QueuePortalImageLayerSet(accepted.Binding, std::move(roomLayers), roomHandles));
		accepted.Image = roomHandles[0];
		accepted.TransparentImages = {roomHandles[1], roomHandles[2]};
		const auto uploadFence = renderer.QueueResourceImage(pipeline, core::Name("room-export"));
		REQUIRE(uploadFence != 0);
		renderer.Render(std::span(&view, 1), overlay, nullptr, false);
		REQUIRE(AwaitImage(renderer, uploadFence).Status == render::ResourceImageStatus::Ok);
		REQUIRE(renderer.PortalImageLayerSetReady(accepted.Image));

		const core::Name owner(accepted.Producer.World), shift("native-lens-shift"), turn("native-lens-turn");
		render::ShaderCompiler compiler;
		for (const auto shader : {shift, turn}) {
			const std::string source = std::string(R"glsl(#version 450
layout(location=0) in vec2 uv;
layout(location=0) out vec4 colour;
layout(set=2,binding=0) uniform sampler2D sceneColour;
layout(set=2,binding=1) uniform sampler2D sceneDepth;
struct Lens {vec4 centre;vec4 x;vec4 y;vec4 z;vec4 spin;};
layout(set=3,binding=0) uniform LensPass {
 mat4 vp;mat4 inverseVp;vec4 target;vec4 eye;vec4 timeCount;Lens lenses[16];vec4 cameraDepth;
} pass;
void main(){ivec2 size=textureSize(sceneColour,0);ivec2 pixel=ivec2(gl_FragCoord.xy);
)glsl") +
									   (shader == shift ? "pixel.x=(pixel.x+int(pass.timeCount.x))%size.x;"
														: "pixel=ivec2(pixel.y,size.x-1-pixel.x);") +
									   "colour=texelFetch(sceneColour,pixel,0);}";
			const auto compiled = compiler.Compile(source, render::ShaderStage::Fragment);
			REQUIRE_FALSE(compiled.Failed);
			REQUIRE(renderer.AddLensShader(shader, compiled.SpirV, owner));
			accepted.Lenses.Programs.push_back(
				{.Hash = renderer.LensShaderHash(shader, owner), .SpirV = compiled.SpirV}
			);
		}
		accepted.Lenses.TimeSeconds = 3;
		for (const auto shader : {shift, turn, shift}) {
			render::PortalCaptureLens lens;
			lens.Position = accepted.Camera.Position;
			lens.Shader = shader.Text();
			lens.ProgramHash = renderer.LensShaderHash(shader, owner);
			lens.Radius = 100;
			lens.Strength = 1;
			accepted.Lenses.Entries.push_back(lens);
			view.Lighting.ShaderLenses[view.Lighting.ShaderLensCount++] = {
				.Frame = core::CFrame(view.CameraFrame.Position),
				.Shader = shader,
				.Radius = 100,
				.Strength = 1
			};
		}
		view.LensTimeSeconds = accepted.Lenses.TimeSeconds;
		view.LensContentOwner = owner;
		accepted.LensPrograms = renderer.RetainPortalLensPrograms(accepted.Lenses);
		REQUIRE(accepted.LensPrograms != 0);
		const auto retainedAgain = renderer.RetainPortalLensPrograms(accepted.Lenses);
		REQUIRE(retainedAgain == accepted.LensPrograms);
		renderer.ReleasePortalLensPrograms(retainedAgain);
		if (!rotated) {
			for (int invalidKind = 0; invalidKind < 4; ++invalidKind) {
				CAPTURE(invalidKind);
				auto invalid = accepted.Lenses;
				auto &program = invalid.Programs.front();
				const auto replacedHash = program.Hash;
				if (invalidKind == 0) {
					REQUIRE(program.SpirV.size() > 5);
					program.SpirV[5] = 0;
				} else {
					const auto source =
						invalidKind == 1
							? std::string("#version 450\nvoid main(){gl_Position=vec4(0,0,0,1);}")
							: std::string("#version 450\nlayout(location=0) out vec4 colour;\nlayout(set=") +
								  (invalidKind == 2 ? "0,binding=0" : "2,binding=2") +
								  ") uniform sampler2D image;\nvoid main(){colour=texture(image,vec2(.5));}";
					const auto module = compiler.Compile(
						source, invalidKind == 1 ? render::ShaderStage::Vertex : render::ShaderStage::Fragment
					);
					REQUIRE_FALSE(module.Failed);
					program.SpirV = module.SpirV;
				}
				program.Hash = assets::Hasher::Of(std::as_bytes(std::span(program.SpirV)));
				for (auto &lens : invalid.Entries)
					if (lens.ProgramHash == replacedHash) lens.ProgramHash = program.Hash;
				REQUIRE(render::ValidPortalCaptureLenses(invalid));
				CHECK(renderer.RetainPortalLensPrograms(invalid) == 0);
				const auto retainedValid = renderer.RetainPortalLensPrograms(accepted.Lenses);
				REQUIRE(retainedValid == accepted.LensPrograms);
				renderer.ReleasePortalLensPrograms(retainedValid);
			}
		}
		const core::Name collidingOwner("portal.lens.programs/0");
		const auto collisionProgram = compiler.Compile(
			"#version 450\nlayout(location=0) out vec4 colour;\nvoid main(){colour=vec4(16,0,16,1);}",
			render::ShaderStage::Fragment
		);
		REQUIRE_FALSE(collisionProgram.Failed);
		const auto collisionHash = assets::Hasher::Of(std::as_bytes(std::span(collisionProgram.SpirV)));
		for (const auto shader : {shift, turn})
			REQUIRE(renderer.AddLensShader(shader, collisionProgram.SpirV, collidingOwner));
		auto body = wall;
		body.Source = 21;
		body.HalfExtent = {.55f, .7f, 1};
		body.EmissiveTint = {1, 1, 0};
		body.EmissiveStrength = 4;
		for (const float bodyDistance : {2.f, 3.5f, 5.f}) {
			CAPTURE(bodyDistance);
			body.Frame = view.CameraFrame * core::CFrame(core::Vector3{.2f, -.1f, -bodyDistance});
			auto nativeRows = roomRows;
			nativeRows.push_back(body);
			view.Instances = nativeRows;
			view.Damage = {.Scene = true, .Objects = true, .Environment = true, .Portals = true};
			const std::array nativeNodes{core::Name("native-lens-export"), core::Name("direct-export")};
			std::array<uint64_t, 2> nativeTokens{};
			REQUIRE(renderer.QueueResourceImages(
				pipeline, nativeNodes, view.Slot, render::ResourceImageDelivery::CopiedPixels, nativeTokens
			));
			REQUIRE(renderer.Render(std::span(&view, 1), overlay, nullptr, false)
						.Ran(core::Name("native-lens-export")));
			const auto native = AwaitImageGroup(renderer, nativeTokens);
			REQUIRE(native[0].Status == render::ResourceImageStatus::Ok);
			REQUIRE(native[1].Status == render::ResourceImageStatus::Ok);
			CHECK(native[0].Pixels != native[1].Pixels);
			for (size_t pixel = 0; pixel < 33 * 33; ++pixel) {
				const size_t sampledX = (pixel / 33 + 3) % 33;
				const size_t sampledY = 32 - (pixel % 33 + 3) % 33;
				const auto expected = std::span(native[1].Pixels).subspan((sampledY * 33 + sampledX) * 8, 8);
				const auto actual = std::span(native[0].Pixels).subspan(pixel * 8, 8);
				CHECK(std::equal(actual.begin(), actual.end(), expected.begin()));
			}
			view.Instances = std::span(&body, 1);
			if (bodyDistance == 2 && !rotated) {
				auto mismatched = accepted;
				for (auto &lens : mismatched.Lenses.Entries)
					lens.ProgramHash = lens.Shader == shift.Text() ? accepted.Lenses.Programs[1].Hash
																   : accepted.Lenses.Programs[0].Hash;
				REQUIRE(render::ValidPortalCaptureLenses(mismatched.Lenses));
				CHECK(renderer.ComposePortalBodyImage(mismatched, view) == 0);
			}
			const auto prepared = renderer.ComposePortalBodyImage(accepted, view);
			REQUIRE(prepared != 0);
			REQUIRE(renderer.DropPortalImage(prepared));
			const auto copiedToken = renderer.QueueResourceImage(
				core::Name("portal-body-eye-image/lenses/layers-2/0"), core::Name("export")
			);
			REQUIRE(copiedToken != 0);
			const auto copiedHandle = renderer.ComposePortalBodyImage(accepted, view);
			REQUIRE(copiedHandle != 0);
			const auto copied = AwaitImage(renderer, copiedToken);
			REQUIRE(copied.Status == render::ResourceImageStatus::Ok);
			REQUIRE(copied.Pixels.size() == native[0].Pixels.size());
			core::ByteReader nativePixels(native[0].Pixels), copiedPixels(copied.Pixels);
			while (!nativePixels.AtEnd()) {
				const auto direct = glm::unpackHalf2x16(nativePixels.ReadUInt32());
				const auto imported = glm::unpackHalf2x16(copiedPixels.ReadUInt32());
				// The two glass stores have the same bound as the direct peel comparison above.
				CHECK(std::abs(direct.x - imported.x) < .004f);
				CHECK(std::abs(direct.y - imported.y) < .004f);
			}
			REQUIRE(copied.Depth.size() == native[0].Depth.size());
			core::ByteReader nativeDepth(native[0].Depth), copiedDepth(copied.Depth);
			while (!nativeDepth.AtEnd())
				CHECK(std::abs(nativeDepth.ReadFloat() - copiedDepth.ReadFloat()) < .003f);
			REQUIRE(renderer.DropPortalImage(copiedHandle));
			if (bodyDistance == 2) {
				renderer.DropContentOwner(collidingOwner);
				for (const auto shader : {shift, turn})
					CHECK_FALSE(renderer.HasLensShader(shader, collidingOwner));
				const auto afterDropToken = renderer.QueueResourceImage(
					core::Name("portal-body-eye-image/lenses/layers-2/0"), core::Name("export")
				);
				REQUIRE(afterDropToken != 0);
				const auto afterDropHandle = renderer.ComposePortalBodyImage(accepted, view);
				REQUIRE(afterDropHandle != 0);
				const auto afterDrop = AwaitImage(renderer, afterDropToken);
				REQUIRE(afterDrop.Status == render::ResourceImageStatus::Ok);
				CHECK(afterDrop.Pixels == copied.Pixels);
				CHECK(afterDrop.Depth == copied.Depth);
				REQUIRE(renderer.DropPortalImage(afterDropHandle));
				for (const auto shader : {shift, turn})
					REQUIRE(renderer.AddLensShader(shader, collisionProgram.SpirV, collidingOwner));
			}
		}
		renderer.ReleasePortalLensPrograms(accepted.LensPrograms);
		for (const auto shader : {shift, turn}) {
			CHECK(renderer.HasLensShader(shader, collidingOwner));
			CHECK(renderer.LensShaderHash(shader, collidingOwner) == collisionHash);
		}
		renderer.DropContentOwner(collidingOwner);
		CHECK(renderer.ComposePortalBodyImage(accepted, view) == 0);
		REQUIRE(renderer.DropPortalImage(accepted.Image));
		view = savedView;
	}
	if (scenario == 0 && !rotated) {
		rows.front().Shader = core::Name("uncaptured-material");
		const core::Name unrelated("unrelated-glass-pipeline");
		REQUIRE(renderer.SetPipeline(unrelated, graph));
		const auto waiting = renderer.QueueResourceImage(unrelated, core::Name("glass-0-export"));
		REQUIRE(waiting != 0);
		const auto token = renderer.QueueResourceImage(pipeline, core::Name("glass-0-export"));
		REQUIRE(token != 0);
		CHECK_FALSE(renderer.Render(std::span(&view, 1), overlay, nullptr, false).Ran(core::Name("glass-0")));
		CHECK(AwaitImage(renderer, token).Status == render::ResourceImageStatus::Failed);
		CHECK_FALSE(renderer.TakeResourceImage(waiting).has_value());
		rows.front().Shader = {};
		view.Pipeline = unrelated;
		REQUIRE(renderer.Render(std::span(&view, 1), overlay, nullptr, false).Ran(core::Name("glass-0")));
		CHECK(AwaitImage(renderer, waiting).Status == render::ResourceImageStatus::Ok);
	}
}

TEST_CASE(
	"resident capture groups transfer all ownership or none",
	"[render][gpu][resourceimage][resident-group][.]"
) {
	using namespace engine;
	render::test::FixtureDevice fixture;
	fixture.Initialise();
	auto &renderer = fixture.Render;
	InstallImageCapture(renderer, "lit", true);
	const auto pipeline = core::Name("image-export-pipeline");
	render::SceneTarget target{512, 512};
	render::View view;
	view.Pipeline = pipeline;
	view.Target = &target;
	render::OverlayImage overlay;
	uint64_t nextToken = 1000;
	auto capture = [&](size_t count) {
		std::vector<uint64_t> tokens;
		std::vector<render::ResourceImageRequest> requests;
		for (size_t index = 0; index < count; ++index) {
			tokens.push_back(nextToken++);
			requests.push_back(
				{tokens.back(),
				 pipeline,
				 core::Name("image-export"),
				 0,
				 render::ResourceImageDelivery::Resident}
			);
		}
		REQUIRE(renderer.RequestResourceImages(requests));
		const auto fenceProbe =
			count < 5 ? renderer.QueueResourceImage(pipeline, core::Name("image-export")) : 0;
		if (count < 5) REQUIRE(fenceProbe != 0);
		REQUIRE(
			renderer.Render(std::span(&view, 1), overlay, nullptr, false).Ran(core::Name("image-export"))
		);
		if (fenceProbe)
			REQUIRE(AwaitImage(renderer, fenceProbe).Status == render::ResourceImageStatus::Ok);
		else
			REQUIRE(SDL_WaitForGPUIdle(static_cast<SDL_GPUDevice *>(renderer.Backend().Device)));
		return tokens;
	};
	auto binding = [](size_t index) {
		render::PortalImageBinding value;
		value.WorldName = core::Name("group-owner");
		value.Portal = core::Name("layer-" + std::to_string(index / 3));
		value.Layer = static_cast<uint8_t>(index % 3);
		value.ExpectedScope = render::PortalImageScope::OpaqueLighting;
		value.Expected = {1, std::string(value.Portal.Text()), 1, 1};
		return value;
	};
	std::array<uint64_t, 9> previous{};
	for (size_t batch = 0; batch < 3; ++batch) {
		const auto tokens = capture(3);
		const std::array bindings{binding(batch * 3), binding(batch * 3 + 1), binding(batch * 3 + 2)};
		REQUIRE(renderer.AdoptResourceImages(tokens, bindings, std::span(previous).subspan(batch * 3, 3)));
	}
	const auto tokens = capture(2);
	std::array bindings{binding(0), binding(1)};
	CHECK_FALSE(renderer.AdoptResourceImages({}, {}, {}));
	std::array<uint64_t, 2> handles{777, 888};
	const auto retained = renderer.PortalImageUsage();
	CHECK(retained.TextureBytes == 27 * 1024 * 1024);
	CHECK_FALSE(renderer.AdoptResourceImages(tokens, bindings, handles));
	CHECK(handles == std::array<uint64_t, 2>{777, 888});
	CHECK(renderer.PortalImageUsage().TextureBytes == retained.TextureBytes);
	CHECK(renderer.PortalImageUsage().CachedTextureBytes == retained.CachedTextureBytes);
	for (const auto token : tokens)
		CHECK(renderer.CanPublishResourceImage(token, 512, 512));
	REQUIRE(renderer.DropPortalImage(previous.back()));
	for (const int invalid : {0, 1, 2, 3}) {
		auto changed = bindings;
		auto changedTokens = tokens;
		if (invalid == 0) changed[1].Expected.PortalKey = "wrong";
		if (invalid == 1) changed[1] = changed[0];
		if (invalid == 2) changedTokens[1] = changedTokens[0];
		if (invalid == 3) changed[1].Sampling = glm::mat4(0);
		CHECK_FALSE(renderer.AdoptResourceImages(changedTokens, changed, handles));
		CHECK(handles == std::array<uint64_t, 2>{777, 888});
		for (const auto token : tokens)
			CHECK(renderer.CanPublishResourceImage(token, 512, 512));
	}
	REQUIRE(renderer.AdoptResourceImages(tokens, bindings, handles));
	CHECK_FALSE(renderer.DropPortalImage(previous[0]));
	CHECK_FALSE(renderer.DropPortalImage(previous[1]));
	CHECK(renderer.PortalImageUsage().TextureBytes == 24 * 1024 * 1024);
	CHECK(renderer.PortalImageUsage().CachedTextureBytes == 6 * 1024 * 1024);
	for (const auto token : tokens)
		CHECK_FALSE(renderer.CanPublishResourceImage(token, 512, 512));
	for (const auto handle : handles)
		CHECK(renderer.DropPortalImage(handle));
	for (size_t index = 2; index < 8; ++index)
		CHECK(renderer.DropPortalImage(previous[index]));
	CHECK(renderer.PortalImageUsage().TextureBytes == 0);
	CHECK(renderer.PortalImageUsage().CachedTextureBytes == 0);

	const auto firstFrame = capture(1);
	const auto secondFrame = capture(1);
	const std::array mixedFrames{firstFrame.front(), secondFrame.front()};
	CHECK_FALSE(renderer.AdoptResourceImages(mixedFrames, bindings, handles));
	CHECK(renderer.PortalImageUsage().Images == 0);
	for (const auto token : mixedFrames) {
		CHECK(renderer.CanPublishResourceImage(token, 512, 512));
		CHECK(renderer.CancelResourceImage(token));
	}

	// Slot pressure is independent of the byte budget. A group cannot reserve
	// the last free import and then fail after consuming its first capture.
	target.Width = target.Height = 16;
	const auto fullCapture = capture(5);
	const std::array fullOwners{binding(0), binding(1), binding(2), binding(3), binding(4)};
	std::array<uint64_t, 5> fullHandles{};
	REQUIRE(renderer.AdoptResourceImages(fullCapture, fullOwners, fullHandles));
	CHECK(renderer.PortalImageUsage().Images == 5);
	for (const auto handle : fullHandles)
		CHECK(renderer.DropPortalImage(handle));
	CHECK(renderer.PortalImageUsage().Images == 0);
	std::array<uint64_t, render::MAX_IMPORTED_PORTAL_IMAGES - 1> occupied{};
	for (size_t first = 0; first < occupied.size(); first += 5) {
		const size_t count = std::min(size_t{5}, occupied.size() - first);
		const auto small = capture(count);
		std::vector<render::PortalImageBinding> owners;
		for (size_t index = 0; index < count; ++index)
			owners.push_back(binding(first + index));
		REQUIRE(renderer.AdoptResourceImages(small, owners, std::span(occupied).subspan(first, count)));
	}
	const auto small = capture(2);
	bindings = {binding(occupied.size()), binding(occupied.size() + 1)};
	const auto beforeSlotRefusal = handles;
	CHECK_FALSE(renderer.AdoptResourceImages(small, bindings, handles));
	CHECK(handles == beforeSlotRefusal);
	CHECK(renderer.PortalImageUsage().Images == occupied.size());
	for (const auto token : small)
		CHECK(renderer.CanPublishResourceImage(token, 16, 16));
	CHECK_FALSE(renderer.AdoptResourceImages(small, std::span(bindings).first(1), handles));
	CHECK_FALSE(renderer.AdoptResourceImages(small, bindings, std::span(handles).first(1)));
	REQUIRE(renderer.DropPortalImage(occupied.back()));
	REQUIRE(renderer.AdoptResourceImages(small, bindings, handles));
	CHECK(handles[0] != handles[1]);
	CHECK(renderer.PortalImageUsage().Images == render::MAX_IMPORTED_PORTAL_IMAGES);
	for (size_t index = 0; index + 1 < occupied.size(); ++index)
		CHECK(renderer.DropPortalImage(occupied[index]));
	for (const auto handle : handles)
		CHECK(renderer.DropPortalImage(handle));
	CHECK(renderer.PortalImageUsage().Images == 0);
	CHECK(renderer.PortalImageUsage().TextureBytes == 0);
	CHECK(renderer.PortalImageUsage().CachedTextureBytes == 0);
}

TEST_CASE(
	"copied portal layers place the current body between glass and room",
	"[render][gpu][resourceimage][layer-body][.]"
) {
	using namespace engine;
	render::test::FixtureDevice fixture;
	fixture.Initialise();
	auto &renderer = fixture.Render;
	const core::Name lensOwner("copied-lens-owner");
	const core::Name lensA("copied-lens-a"), lensB("copied-lens-b");
	render::ShaderCompiler lensCompiler;
	const auto lensSource = [](bool second) {
		return std::string(R"glsl(#version 450
layout(location=0) in vec2 inUv;
layout(location=0) out vec4 outColour;
layout(set=2,binding=0) uniform sampler2D sceneColour;
layout(set=2,binding=1) uniform sampler2D linearDepth;
struct Lens { vec4 CentreRadius; vec4 AxisXInner; vec4 AxisYFalloff; vec4 AxisZStrength; vec4 SpinPriority; };
layout(set=3,binding=0) uniform LensPass {
 mat4 ViewProjection; mat4 InverseViewProjection; vec4 Target; vec4 Eye; vec4 TimeCount; Lens Lenses[16]; vec4 CameraDepth;
} pass;
void main() {
 vec4 colour=texture(sceneColour,inUv);
 vec3 addition=vec3(pass.TimeCount.x, texture(linearDepth,inUv).r, pass.Lenses[0].AxisZStrength.w)/16.0;
)glsl") + (second ? "outColour=vec4(colour.rgb*2.0+addition.bgr,colour.a);}"
				  : "outColour=vec4(colour.rgb*0.5+addition,colour.a);}");
	};
	const auto firstProgram = lensCompiler.Compile(lensSource(false), render::ShaderStage::Fragment);
	const auto secondProgram = lensCompiler.Compile(lensSource(true), render::ShaderStage::Fragment);
	REQUIRE_FALSE(firstProgram.Failed);
	REQUIRE_FALSE(secondProgram.Failed);
	REQUIRE(renderer.AddLensShader(lensA, firstProgram.SpirV, lensOwner));
	REQUIRE(renderer.AddLensShader(lensB, secondProgram.SpirV, lensOwner));
	CHECK(
		renderer.LensShaderHash(lensA, lensOwner) ==
		assets::Hasher::Of(std::as_bytes(std::span(firstProgram.SpirV)))
	);
	CHECK(renderer.LensShaderHash(lensA).IsZero());

	const bool seam = GENERATE(false, true);
	graph::RenderGraph graph;
	core::Name offender;
	REQUIRE(
		graph::Build(graph::DefaultPortalBodyDocument(seam, true), graph, offender) ==
		graph::PipelineDocumentStatus::Ok
	);
	const core::Name pipeline("layer-body");
	REQUIRE(renderer.SetPipeline(pipeline, graph));
	const float bodyDepth = GENERATE(1.f, 3.f, 5.f, 7.f);
	const bool rotated = GENERATE(false, true);
	CAPTURE(bodyDepth, rotated, seam);
	const auto image = [](glm::vec4 colour, float distance, uint32_t extent = 17) {
		render::PortalImageReply reply;
		reply.Key = {1, "layer-door", 1, 1};
		reply.Scope = render::PortalImageScope::OpaqueLighting;
		reply.Status = render::PortalImageStatus::Ok;
		reply.CaptureTick = reply.ContentRevision = reply.LightingRevision = 1;
		reply.Width = reply.Height = extent;
		reply.RowStride = extent * 8;
		core::ByteWriter pixels, depth;
		for (size_t pixel = 0; pixel < size_t(extent) * extent; ++pixel) {
			pixels.WriteUInt32(glm::packHalf2x16({colour.r, colour.g}));
			pixels.WriteUInt32(glm::packHalf2x16({colour.b, colour.a}));
			depth.WriteFloat(distance);
		}
		reply.Pixels.assign(pixels.Bytes().begin(), pixels.Bytes().end());
		reply.Depth.assign(depth.Bytes().begin(), depth.Bytes().end());
		reply.PixelHash = assets::Hasher::Of(reply.Pixels);
		reply.DepthHash = assets::Hasher::Of(reply.Depth);
		return reply;
	};
	render::PortalImageLayerSet layers{
		image({0, 0, 2, 1}, 6), {image({0, 1, 0, .5f}, 2), image({1, 0, 1, .25f}, 4)}
	};
	std::vector<std::byte> wire;
	std::string error;
	REQUIRE(render::EncodePortalImageLayerSet(layers, wire, error));
	render::PortalImageLayerSet decoded;
	REQUIRE(render::DecodePortalImageLayerSet(wire, decoded, error));
	render::SceneTarget target{17, 17};
	render::View view;
	view.Pipeline = pipeline;
	view.Target = &target;
	view.WorldName = core::Name("layer-receiver");
	view.EyeImageKey = core::Name("layer-door");
	if (rotated)
		view.CameraFrame = core::CFrame(core::Vector3{7, 2, 3}) * core::CFrame::Angles(-.2f, .37f, .1f);
	render::PortalCaptureCamera captureCamera;
	const auto rotation = view.CameraFrame.Rotation();
	captureCamera.Position = {
		view.CameraFrame.Position.X, view.CameraFrame.Position.Y, view.CameraFrame.Position.Z
	};
	captureCamera.Orientation = {rotation.x, rotation.y, rotation.z, rotation.w};
	const float near = view.Camera.NearPlane;
	const float half = near * std::tan(view.Camera.FieldOfViewRadians / 2);
	captureCamera.Frustum = {-half, half, -half, half, near, view.Camera.FarPlane};
	const auto projection = seam ? render::PortalImageProjection::Seam : render::PortalImageProjection::Eye;
	if (seam) {
		const auto normal = view.CameraFrame.VectorToWorldSpace({0, 0, -1});
		const auto mouth = view.CameraFrame.PointToWorldSpace({0, 0, -.5f});
		captureCamera.ClipPlane = {normal.X, normal.Y, normal.Z, -normal.Dot(mouth)};
	}
	REQUIRE(render::ResolvePortalCaptureCamera(captureCamera, projection, view));
	render::PortalImageBinding binding;
	binding.WorldName = view.WorldName;
	binding.Portal = view.EyeImageKey;
	binding.Expected = decoded.Opaque.Key;
	binding.ExpectedScope = render::PortalImageScope::OpaqueLighting;
	binding.ExpectedProjection = projection;
	binding.Sampling = seam ? scene::ResolveSurfaceCamera(view.CameraFrame, *view.Projection).ViewProjection
							: scene::ResolveCamera(view.CameraFrame, view.Camera, 1).ViewProjection;
	std::array<uint64_t, 3> imported{};
	REQUIRE(renderer.QueuePortalImageLayerSet(binding, std::move(decoded), imported));
	view.EyeImage = imported[0];
	view.EyeTransparentImages = {imported[1], imported[2]};
	CHECK_FALSE(renderer.PortalImageLayerSetReady(view.EyeImage));
	CHECK(renderer.PortalImageUsage().Images == 3);
	assets::TextureData white;
	white.Width = white.Height = 1;
	white.Format = assets::TextureFormat::RGBA8;
	white.Pixels.assign(4, std::byte{255});
	REQUIRE(renderer.AddTexture(core::Name("body-emission"), white));
	assets::MeshData plane;
	plane.Vertices = {
		{{-1, -1, 0}, {0, 0, 1}, {0, 1}},
		{{1, -1, 0}, {0, 0, 1}, {1, 1}},
		{{1, 1, 0}, {0, 0, 1}, {1, 0}},
		{{-1, 1, 0}, {0, 0, 1}, {0, 0}}
	};
	plane.Indices = {0, 1, 2, 0, 2, 3};
	plane.ComputeBounds();
	REQUIRE(renderer.AddMesh(core::Name("current-body-plane"), plane));
	scene::DrawInstance body;
	body.Mesh = core::Name("current-body-plane");
	body.Source = 1;
	body.HalfExtent = {.7f, .7f, 1};
	body.Frame = view.CameraFrame * core::CFrame(core::Vector3{0, 0, -bodyDepth});
	body.Tint = {};
	body.EmissiveMap = core::Name("body-emission");
	body.EmissiveTint = {1, 0, 0};
	body.EmissiveStrength = 16;
	body.CastShadow = false;
	view.Instances = std::span(&body, 1);
	view.OverrideLighting = true;
	view.Lighting.Ambient = {};
	view.Lighting.OutdoorAmbient = {};
	view.Lighting.Direct = {};
	render::OverlayImage overlay;
	auto capture = [&]() {
		const auto token = renderer.QueueResourceImage(pipeline, core::Name("export"));
		REQUIRE(token != 0);
		renderer.Render(std::span(&view, 1), overlay, nullptr, false);
		return AwaitImage(renderer, token);
	};
	const auto result = capture();
	REQUIRE(result.Status == render::ResourceImageStatus::Ok);
	CHECK(renderer.PortalImageLayerSetReady(view.EyeImage));
	const auto checkImage = [&](const render::ResourceImage &captured, float distance) {
		const auto projection = scene::ResolveCamera(core::CFrame{}, view.Camera, 1).ViewProjection;
		const float halfWidth = std::abs(projection[0][0]) * .7f / distance;
		const float halfHeight = std::abs(projection[1][1]) * .7f / distance;
		core::ByteReader pixels(captured.Pixels), depth(captured.Depth);
		std::vector<float> actualSamples, expectedSamples;
		for (size_t pixel = 0; pixel < 17 * 17; ++pixel) {
			const float x = 2 * (float(pixel % 17) + .5f) / 17 - 1;
			const float y = 2 * (float(pixel / 17) + .5f) / 17 - 1;
			const bool bodyVisible = distance < 6 && std::abs(x) < halfWidth && std::abs(y) < halfHeight;
			const float visibleDepth = bodyVisible ? distance : 6;
			glm::vec4 expected = bodyVisible ? glm::vec4{16, 0, 0, 1} : glm::vec4{0, 0, 2, 1};
			if (visibleDepth > 4) expected = glm::vec4{1, 0, 1, .25f} + expected * .75f;
			if (visibleDepth > 2) expected = glm::vec4{0, 1, 0, .5f} + expected * .5f;
			const auto rg = glm::unpackHalf2x16(pixels.ReadUInt32());
			const auto ba = glm::unpackHalf2x16(pixels.ReadUInt32());
			const glm::vec4 actual{rg, ba};
			for (size_t channel = 0; channel < 4; ++channel) {
				CHECK(std::abs(actual[channel] - expected[channel]) < .004f);
				actualSamples.push_back(actual[channel]);
				expectedSamples.push_back(expected[channel]);
			}
			CHECK(std::abs(depth.ReadFloat() - visibleDepth) < .003f);
		}
		if (const char *output = std::getenv("MONO_RENDER_PREVIEW_DIR")) {
			const auto directory = std::filesystem::path(output);
			std::filesystem::create_directories(directory);
			const auto stem =
				"layer-body-" + std::to_string(int(distance)) + (rotated ? "-rotated" : "-front");
			for (const bool reference : {true, false}) {
				const auto &samples = reference ? expectedSamples : actualSamples;
				const render::test::ImageView preview{
					17, 17, render::test::ImageFormat::Rgba32Float, std::as_bytes(std::span(samples)), 0
				};
				render::test::WriteImagePreview(
					directory / (stem + (reference ? "-expected.ppm" : "-actual.ppm")), preview
				);
			}
		}
	};
	checkImage(result, bodyDepth);
	const auto uploads = renderer.PortalImageUsage().Uploads;
	std::swap(view.EyeTransparentImages[0], view.EyeTransparentImages[1]);
	CHECK(capture().Status == render::ResourceImageStatus::Failed);
	std::swap(view.EyeTransparentImages[0], view.EyeTransparentImages[1]);
	CHECK(capture().Pixels == result.Pixels);
	const float movedDepth = bodyDepth == 1 ? 7.f : 1.f;
	body.Frame = view.CameraFrame * core::CFrame(core::Vector3{0, 0, -movedDepth});
	const auto moved = capture();
	REQUIRE(moved.Status == render::ResourceImageStatus::Ok);
	checkImage(moved, movedDepth);
	CHECK(moved.Pixels != result.Pixels);
	CHECK(renderer.PortalImageUsage().Uploads == uploads);
	if (bodyDepth == 3 && !rotated) {
		render::PortalImageCapture accepted;
		accepted.Image = imported[0];
		accepted.TransparentImages = {imported[1], imported[2]};
		accepted.Binding = binding;
		accepted.Width = accepted.Height = 17;
		accepted.CaptureLighting.emplace();
		accepted.Camera = captureCamera;
		auto currentView = view;
		currentView.Damage = {};
		currentView.CameraFrame.Position = {100, 100, 100};
		const auto composed = renderer.ComposePortalBodyImage(accepted, currentView);
		REQUIRE(composed != 0);
		CHECK(renderer.PortalImageUsage().Images == 4);
		CHECK(renderer.PortalImageLayerSetReady(accepted.Image));
		const auto token = renderer.QueueResourceImage(
			core::Name(seam ? "portal-body-seam-image/layers-2/0" : "portal-body-eye-image/layers-2/0"),
			core::Name("export")
		);
		REQUIRE(token != 0);
		body.Frame = view.CameraFrame * core::CFrame(core::Vector3{0, 0, -bodyDepth});
		const auto replaced = renderer.ComposePortalBodyImage(accepted, currentView);
		REQUIRE(replaced != 0);
		CHECK(AwaitImage(renderer, token).Pixels == result.Pixels);
		body.Frame = view.CameraFrame * core::CFrame(core::Vector3{0, 0, -movedDepth});
		CHECK(renderer.PortalImageUsage().Images == 4);
		CHECK(renderer.PortalImageUsage().Uploads == uploads);
		view.Damage.Objects = false;
		CHECK(capture().Pixels == moved.Pixels);
		view.Damage.Objects = true;
		std::swap(accepted.TransparentImages[0], accepted.TransparentImages[1]);
		CHECK(renderer.ComposePortalBodyImage(accepted, currentView) == 0);
		std::swap(accepted.TransparentImages[0], accepted.TransparentImages[1]);
		const auto acceptedCamera = accepted.Camera;
		accepted.Camera.Frustum[4] = 0;
		CHECK(renderer.ComposePortalBodyImage(accepted, currentView) == 0);
		accepted.Camera = acceptedCamera;
		body.Transparency = .5f;
		CHECK(renderer.ComposePortalBodyImage(accepted, currentView) == 0);
		body.Transparency = 0;
		currentView.WorldName = core::Name("foreign-body-owner");
		CHECK(renderer.ComposePortalBodyImage(accepted, currentView) == 0);
		CHECK(renderer.PortalImageUsage().Images == 4);
		REQUIRE(renderer.DropPortalImage(replaced));
		CHECK(renderer.PortalImageUsage().Images == 3);
	}
	if (bodyDepth == 3 && !rotated) {
		view.WorldName = core::Name("foreign-layer-receiver");
		CHECK(capture().Status == render::ResourceImageStatus::Failed);
		view.WorldName = binding.WorldName;
		CHECK(capture().Pixels == moved.Pixels);
		auto foreign = layers.Transparent[0];
		binding.Layer = 3;
		CHECK(renderer.QueuePortalImage(binding, std::move(foreign)) == 0);
		foreign = layers.Transparent[0];
		foreign.Key.CameraRevision++;
		binding.Layer = 1;
		binding.Expected = foreign.Key;
		view.EyeTransparentImages[0] = renderer.QueuePortalImage(binding, std::move(foreign));
		REQUIRE(view.EyeTransparentImages[0] != 0);
		CHECK(capture().Status == render::ResourceImageStatus::Failed);
		CHECK(renderer.DropPortalImage(view.EyeTransparentImages[0]));
		view.EyeTransparentImages[0] = imported[1];
		CHECK(capture().Pixels == moved.Pixels);
	}

	if (bodyDepth == 3 && !rotated) {
		body.Frame = view.CameraFrame * core::CFrame(core::Vector3{0, 0, -bodyDepth});
		CHECK(capture().Pixels == result.Pixels);
		auto replacement = layers;
		replacement.Transparent[0] = image({1, 0, 0, .5f}, 2);
		for (auto *member : {&replacement.Opaque, &replacement.Transparent[0], &replacement.Transparent[1]})
			member->Key.RequestId = 2;
		binding.Layer = 0;
		binding.Expected = replacement.Opaque.Key;
		std::array<uint64_t, 3> next{777, 888, 999};
		auto invalid = replacement;
		invalid.Transparent[1].CaptureTick++;
		const auto preserved = invalid;
		CHECK_FALSE(renderer.QueuePortalImageLayerSet(binding, std::move(invalid), next));
		CHECK(invalid == preserved);
		CHECK(next == std::array<uint64_t, 3>{777, 888, 999});
		CHECK(renderer.PortalImageUsage().Images == 3);
		REQUIRE(renderer.QueuePortalImageLayerSet(binding, std::move(replacement), next));
		CHECK_FALSE(renderer.PortalImageLayerSetReady(next[0]));
		CHECK(renderer.PortalImageLayerSetReady(imported[0]));
		CHECK(renderer.PortalImageUsage().Images == 6);
		// Upload the replacement while still drawing the old complete set.
		CHECK(capture().Pixels == result.Pixels);
		REQUIRE(renderer.PortalImageLayerSetReady(next[0]));
		view.EyeImage = next[0];
		view.EyeTransparentImages = {next[1], next[2]};
		const auto replaced = capture();
		REQUIRE(replaced.Status == render::ResourceImageStatus::Ok);
		core::ByteReader before(result.Pixels), after(replaced.Pixels);
		for (size_t pixel = 0; pixel < 17 * 17; ++pixel) {
			const auto oldRg = glm::unpackHalf2x16(before.ReadUInt32());
			const auto newRg = glm::unpackHalf2x16(after.ReadUInt32());
			CHECK(std::abs(newRg.x - oldRg.x - 1) < .004f);
			CHECK(std::abs(newRg.y - oldRg.y + 1) < .004f);
			CHECK(before.ReadUInt32() == after.ReadUInt32());
		}
		CHECK(renderer.DropPortalImage(next[2]));
		CHECK_FALSE(renderer.PortalImageLayerSetReady(next[0]));
		CHECK_FALSE(renderer.DropPortalImage(next[0]));
		view.EyeImage = imported[0];
		view.EyeTransparentImages = {imported[1], imported[2]};
		CHECK(capture().Pixels == result.Pixels);

		for (uint64_t frame = 0; frame < 6; ++frame) {
			auto cycle = layers;
			for (auto *member : {&cycle.Opaque, &cycle.Transparent[0], &cycle.Transparent[1]})
				member->Key.RequestId = 10 + frame;
			binding.Expected = cycle.Opaque.Key;
			const auto allocations = renderer.MemoryStatistics().TextureAllocations;
			REQUIRE(renderer.QueuePortalImageLayerSet(binding, std::move(cycle), next));
			CHECK(renderer.MemoryStatistics().TextureAllocations == allocations);
			CHECK(capture().Pixels == result.Pixels);
			REQUIRE(renderer.PortalImageLayerSetReady(next[0]));
			view.EyeImage = next[0];
			view.EyeTransparentImages = {next[1], next[2]};
			CHECK(capture().Pixels == result.Pixels);
			CHECK(renderer.DropPortalImage(next[0]));
			view.EyeImage = imported[0];
			view.EyeTransparentImages = {imported[1], imported[2]};
			CHECK(renderer.PortalImageUsage().CachedTextureBytes == size_t(17) * 17 * 12 * 3);
		}

		// Budget the old and new sets together, both before and after upload.
		std::array<uint64_t, 10> pressure{};
		binding.Expected = layers.Opaque.Key;
		for (size_t index = 0; index < pressure.size(); ++index) {
			auto filler = image({0, 0, 2, 1}, 6, 512);
			filler.Key.PortalKey = "pressure-" + std::to_string(index);
			auto owner = binding;
			owner.Portal = core::Name(filler.Key.PortalKey);
			owner.Expected = filler.Key;
			pressure[index] = renderer.QueuePortalImage(owner, std::move(filler));
			REQUIRE(pressure[index] != 0);
		}
		render::PortalImageLayerSet large{
			image({0, 0, 2, 1}, 6, 256), {image({0, 1, 0, .5f}, 2, 256), image({1, 0, 1, .25f}, 4, 256)}
		};
		const auto retainedLarge = large;
		const auto beforeRefusal = next;
		CHECK(renderer.PortalImageUsage().PendingCpuBytes == 30 * 1024 * 1024);
		CHECK_FALSE(renderer.QueuePortalImageLayerSet(binding, std::move(large), next));
		CHECK(large == retainedLarge);
		CHECK(next == beforeRefusal);
		CHECK(capture().Pixels == result.Pixels);
		CHECK(renderer.PortalImageUsage().PendingCpuBytes == 0);
		CHECK(renderer.PortalImageUsage().TextureBytes > 30 * 1024 * 1024);
		CHECK_FALSE(renderer.QueuePortalImageLayerSet(binding, std::move(large), next));
		CHECK(large == retainedLarge);
		CHECK(next == beforeRefusal);
		for (const auto handle : pressure)
			CHECK(renderer.DropPortalImage(handle));
		REQUIRE(renderer.QueuePortalImageLayerSet(binding, std::move(large), next));
		CHECK_FALSE(renderer.PortalImageLayerSetReady(next[0]));
		CHECK(renderer.DropPortalImage(next[1]));
		CHECK(renderer.PortalImageUsage().Images == 3);
		CHECK(renderer.PortalImageUsage().PendingCpuBytes == 0);
		CHECK(capture().Pixels == result.Pixels);
	}

	{
		binding.Layer = 0;
		binding.Expected = layers.Opaque.Key;
		const auto baseline = renderer.PortalImageUsage();
		for (int cycle = 0; cycle < 2; ++cycle) {
			auto withOverlay = layers;
			withOverlay.SpatialOverlay = image({.25f, .5f, 0, .5f}, 0);
			withOverlay.SpatialOverlay->Depth.clear();
			withOverlay.SpatialOverlay->DepthHash = {};
			const auto saved = withOverlay;
			std::array<uint64_t, 4> overlayHandles{};
			const auto allocations = renderer.MemoryStatistics().TextureAllocations;
			REQUIRE(renderer.QueuePortalImageLayerSet(binding, std::move(withOverlay), overlayHandles));
			if (cycle != 0) CHECK(renderer.MemoryStatistics().TextureAllocations == allocations);
			CHECK(renderer.PortalImageUsage().Images == baseline.Images + 4);
			CHECK(renderer.PortalImageUsage().TextureBytes == baseline.TextureBytes + 17 * 17 * 44);
			CHECK_FALSE(renderer.PortalImageLayerSetReady(overlayHandles[0]));
			CHECK(capture().Status == render::ResourceImageStatus::Ok);
			CHECK(renderer.PortalImageLayerSetReady(overlayHandles[0]));
			CHECK(renderer.PortalImageUsage().PendingCpuBytes == 0);
			render::PortalImageCapture accepted;
			accepted.Image = overlayHandles[0];
			accepted.TransparentImages = {overlayHandles[1], overlayHandles[2]};
			accepted.SpatialOverlayImage = overlayHandles[3];
			accepted.Binding = binding;
			accepted.Width = accepted.Height = 17;
			accepted.CaptureLighting.emplace();
			accepted.Camera = captureCamera;
			body.Frame = view.CameraFrame * core::CFrame(core::Vector3{0, 0, -bodyDepth});
			REQUIRE(renderer.ComposePortalBodyImage(accepted, view) != 0);
			const auto token = renderer.QueueResourceImage(
				core::Name(
					seam ? "portal-body-seam-image/overlay/layers-2/0"
						 : "portal-body-eye-image/overlay/layers-2/0"
				),
				core::Name("export")
			);
			REQUIRE(token != 0);
			const auto composed = renderer.ComposePortalBodyImage(accepted, view);
			REQUIRE(composed != 0);
			const auto blended = AwaitImage(renderer, token);
			REQUIRE(blended.Status == render::ResourceImageStatus::Ok);
			CHECK(blended.Depth == result.Depth);
			core::ByteReader sourcePixels(result.Pixels), blendedPixels(blended.Pixels);
			for (size_t pixel = 0; pixel < 17 * 17; ++pixel) {
				const auto oldRg = glm::unpackHalf2x16(sourcePixels.ReadUInt32());
				const auto oldBa = glm::unpackHalf2x16(sourcePixels.ReadUInt32());
				const auto newRg = glm::unpackHalf2x16(blendedPixels.ReadUInt32());
				const auto newBa = glm::unpackHalf2x16(blendedPixels.ReadUInt32());
				CHECK(glm::length(newRg - (glm::vec2{.25f, .5f} + oldRg * .5f)) < .005f);
				CHECK(glm::length(newBa - (glm::vec2{0, .5f} + oldBa * .5f)) < .005f);
			}
			REQUIRE(renderer.DropPortalImage(composed));
			accepted.Producer.World = "copied-lens-world";
			const std::array lensOwners{
				render::WorldContentOwner{core::Name(accepted.Producer.World), lensOwner}
			};
			accepted.Lenses.TimeSeconds = 7;
			for (const auto shader : {lensA, lensB, lensA}) {
				render::PortalCaptureLens lens;
				lens.Position = accepted.Camera.Position;
				lens.Orientation = {0, 0, 0, 1};
				lens.Shader = shader.Text();
				lens.ProgramHash = renderer.LensShaderHash(shader, lensOwner);
				lens.Radius = 100;
				lens.Strength = 3;
				accepted.Lenses.Entries.push_back(std::move(lens));
			}
			CHECK(renderer.ComposePortalBodyImage(accepted, view) == 0);
			view.ForeignContentOwners = lensOwners;
			auto wrongHash = accepted;
			wrongHash.Lenses.Entries[0].ProgramHash =
				assets::Hasher::Of(std::as_bytes(std::span("wrong", 5)));
			CHECK(renderer.ComposePortalBodyImage(wrongHash, view) == 0);
			REQUIRE(renderer.ComposePortalBodyImage(accepted, view) != 0);
			const auto lensToken = renderer.QueueResourceImage(
				core::Name(
					seam ? "portal-body-seam-image/overlay/lenses/layers-2/0"
						 : "portal-body-eye-image/overlay/lenses/layers-2/0"
				),
				core::Name("export")
			);
			REQUIRE(lensToken != 0);
			const auto lensImage = renderer.ComposePortalBodyImage(accepted, view);
			REQUIRE(lensImage != 0);
			const auto lensed = AwaitImage(renderer, lensToken);
			REQUIRE(lensed.Status == render::ResourceImageStatus::Ok);
			CHECK(lensed.Depth == blended.Depth);
			core::ByteReader beforeLens(blended.Pixels), afterLens(lensed.Pixels), lensDepth(blended.Depth);
			for (size_t pixel = 0; pixel < 17 * 17; ++pixel) {
				const auto beforeRg = glm::unpackHalf2x16(beforeLens.ReadUInt32());
				const auto beforeBa = glm::unpackHalf2x16(beforeLens.ReadUInt32());
				const auto afterRg = glm::unpackHalf2x16(afterLens.ReadUInt32());
				const auto afterBa = glm::unpackHalf2x16(afterLens.ReadUInt32());
				// Binary fractions keep HDR16 stores independent of nearest/zero rounding.
				const glm::vec3 addition{7.f / 16, lensDepth.ReadFloat() / 16, 3.f / 16};
				const auto stored = [](glm::vec3 value) {
					const auto redGreen = glm::unpackHalf2x16(glm::packHalf2x16({value.r, value.g}));
					const auto blue = glm::unpackHalf2x16(glm::packHalf2x16({value.b, 0}));
					return glm::vec3{redGreen.x, redGreen.y, blue.x};
				};
				glm::vec3 expected{beforeRg.x, beforeRg.y, beforeBa.x};
				expected = stored(expected * .5f + addition);
				expected = stored(expected * 2.f + glm::vec3(addition.z, addition.y, addition.x));
				expected = stored(expected * .5f + addition);
				CHECK(glm::length(glm::vec3(afterRg.x, afterRg.y, afterBa.x) - expected) < .01f);
				CHECK(afterBa.y == beforeBa.y);
			}
			REQUIRE(renderer.DropPortalImage(lensImage));
			accepted.Lenses = {};
			view.ForeignContentOwners = {};

			accepted.SpatialOverlayImage = imported[1];
			CHECK(renderer.ComposePortalBodyImage(accepted, view) == 0);
			accepted.SpatialOverlayImage = 0;
			CHECK(renderer.ComposePortalBodyImage(accepted, view) == 0);
			CHECK(renderer.DropPortalImage(overlayHandles[3]));
			CHECK_FALSE(renderer.PortalImageLayerSetReady(overlayHandles[0]));
			CHECK_FALSE(renderer.DropPortalImage(overlayHandles[0]));
			CHECK(renderer.PortalImageUsage().Images == baseline.Images);
			CHECK(renderer.PortalImageUsage().TextureBytes == baseline.TextureBytes);
			auto invalid = saved;
			invalid.SpatialOverlay->ContentRevision++;
			const auto before = overlayHandles;
			CHECK_FALSE(renderer.QueuePortalImageLayerSet(binding, std::move(invalid), overlayHandles));
			CHECK(overlayHandles == before);
			CHECK(renderer.PortalImageUsage().Images == baseline.Images);
		}
	}

	CHECK(renderer.DropPortalImage(view.EyeImage));
	for (const auto handle : view.EyeTransparentImages)
		CHECK_FALSE(renderer.DropPortalImage(handle));
	CHECK(renderer.PortalImageUsage().Images == 0);
}

TEST_CASE("data capture binds copied planes to the rendered snapshot", "[render][gpu][data-capture][.]") {
	render::test::FixtureDevice fixture;
	fixture.Initialise();
	auto &renderer = fixture.Render;
	InstallImageCapture(renderer, "lit", true, true);

	render::SceneTarget target{32, 24};
	render::View view;
	view.Pipeline = core::Name("image-export-pipeline");
	view.Target = &target;
	view.SnapshotId = "snapshot-render-1";
	render::DataCaptureRequest request{
		.SnapshotId = view.SnapshotId,
		.Pipeline = view.Pipeline,
		.CaptureNode = core::Name("image-export"),
		.Channels = {
			render::DataCaptureChannel::RgbLinearHdr,
			render::DataCaptureChannel::LinearDepth,
			render::DataCaptureChannel::ShadingNormal,
		},
	};
	render::DataCaptureTicket ticket;
	REQUIRE(renderer.QueueDataCapture(request, ticket));
	render::OverlayImage overlay;
	REQUIRE(renderer.Render(std::span(&view, 1), overlay, nullptr, false).Ran(core::Name("image-export")));

	render::DataCapturePoll captured;
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	do {
		captured = renderer.PollDataCapture(ticket);
		if (captured.Status == render::DataCaptureStatus::Pending) SDL_Delay(1);
	} while (captured.Status == render::DataCaptureStatus::Pending &&
			 std::chrono::steady_clock::now() < deadline);
	REQUIRE(captured.Status == render::DataCaptureStatus::Ready);
	REQUIRE(captured.SnapshotId == view.SnapshotId);
	REQUIRE(captured.Planes.size() == 3);
	for (const auto &plane : captured.Planes) {
		REQUIRE(plane.Status == render::DataCaptureStatus::Ready);
		CHECK(plane.Width == target.Width);
		CHECK(plane.Height == target.Height);
		CHECK(plane.Hash == assets::Hasher::Of(plane.Bytes));
	}
	CHECK(captured.CameraPose.NearPlaneMetres == view.Camera.NearPlane);
	CHECK(captured.CameraPose.FarPlaneMetres == view.Camera.FarPlane);

	request.Channels = {render::DataCaptureChannel::RgbLinearHdr, render::DataCaptureChannel::ObjectIds};
	render::DataCaptureTicket partial;
	REQUIRE(renderer.QueueDataCapture(request, partial));
	REQUIRE(renderer.Render(std::span(&view, 1), overlay, nullptr, false).Ran(core::Name("image-export")));
	do {
		captured = renderer.PollDataCapture(partial);
		if (captured.Status == render::DataCaptureStatus::Pending) SDL_Delay(1);
	} while (captured.Status == render::DataCaptureStatus::Pending &&
			 std::chrono::steady_clock::now() < deadline);
	REQUIRE(captured.Status == render::DataCaptureStatus::Partial);
	CHECK(captured.Planes[0].Status == render::DataCaptureStatus::Ready);
	CHECK(captured.Planes[1].Status == render::DataCaptureStatus::Unsupported);

	render::DataCaptureTicket cancelled;
	REQUIRE(renderer.QueueDataCapture(request, cancelled));
	renderer.CancelDataCapture(cancelled);
	CHECK(renderer.PollDataCapture(cancelled).Status == render::DataCaptureStatus::Cancelled);

	request.SnapshotId = "snapshot-render-2";
	request.Channels = {render::DataCaptureChannel::RgbLinearHdr};
	REQUIRE(renderer.QueueDataCapture(request, ticket));
	REQUIRE(renderer.Render(std::span(&view, 1), overlay, nullptr, false).Ran(core::Name("image-export")));
	do {
		captured = renderer.PollDataCapture(ticket);
		if (captured.Status == render::DataCaptureStatus::Pending) SDL_Delay(1);
	} while (captured.Status == render::DataCaptureStatus::Pending &&
			 std::chrono::steady_clock::now() < deadline);
	CHECK(captured.Status == render::DataCaptureStatus::Invalid);
}

TEST_CASE("script capture retains copied bytes until explicit release", "[render][gpu][data-capture][.]") {
	render::test::FixtureDevice fixture;
	fixture.Initialise();
	auto &renderer = fixture.Render;
	InstallImageCapture(renderer, "lit", true);

	world::Universe worlds;
	world::WorldSettings settings;
	settings.Name = core::Name("script-capture-world");
	const world::WorldId world = worlds.Create(settings);
	REQUIRE(world.IsValid());
	world::DataFactorySession session(worlds);
	session.SetPauseParticipant(
		[world](world::WorldId paused, world::DataFactoryPauseScope scope, bool, std::string &) {
			return paused == world && scope == world::DataFactoryPauseScope::AllSystems;
		}
	);
	REQUIRE(
		session.Pause("script-capture-world", world::DataFactoryPauseScope::AllSystems, 0).Status ==
		world::DataFactoryStatus::Ok
	);
	std::string snapshot;
	REQUIRE(session.Snapshot("script-capture-world", snapshot).Status == world::DataFactoryStatus::Ok);

	render::ScriptDataCaptureBridge bridge(session, renderer);
	script::DataCaptureBridgeRequest request{
		.InstanceId = "script-capture-world",
		.SnapshotId = snapshot,
		.Pipeline = "image-export-pipeline",
		.CaptureNode = "image-export",
		.Channels = {"rgb_linear_hdr"},
		.TemporalHistory = "preserve",
	};
	uint64_t ticket = 0;
	std::string detail;
	REQUIRE(bridge.Queue("script-capture-world", request, ticket, detail));

	render::SceneTarget target{32, 24};
	render::View view;
	view.WorldName = core::Name("script-capture-world");
	view.Pipeline = core::Name("image-export-pipeline");
	view.Target = &target;
	bridge.PrepareView(view);
	bridge.PrepareView(view);
	REQUIRE(view.SnapshotId == snapshot);
	render::OverlayImage overlay;
	REQUIRE(renderer.Render(std::span(&view, 1), overlay, nullptr, false).Ran(core::Name("image-export")));

	script::DataCaptureBridgePoll poll;
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	do {
		bridge.Pump();
		REQUIRE(bridge.Poll("script-capture-world", ticket, poll, detail));
		if (poll.Status == "pending") SDL_Delay(1);
	} while (poll.Status == "pending" && std::chrono::steady_clock::now() < deadline);
	REQUIRE(poll.Status == "ready");
	REQUIRE(poll.Planes.size() == 1);
	CHECK(poll.Planes.front().Resource != poll.Planes.front().SourceResource);
	std::vector<std::byte> bytes;
	const size_t byteCount = static_cast<size_t>(poll.Planes.front().RowStride) * poll.Planes.front().Height;
	REQUIRE(bridge.ReadPlane(
		"script-capture-world", ticket, poll.Planes.front().Resource, 0, byteCount, bytes, detail
	));
	CHECK(bytes.size() == byteCount);
	CHECK(assets::Hasher::Of(bytes).ToHex() == poll.Planes.front().Hash);
	script::DataCaptureBridgePoll repeated;
	REQUIRE(bridge.Poll("script-capture-world", ticket, repeated, detail));
	REQUIRE(repeated.Planes.size() == 1);
	CHECK(repeated.Planes.front().Resource == poll.Planes.front().Resource);
	CHECK(repeated.Planes.front().Hash == poll.Planes.front().Hash);
	std::vector<std::byte> repeatedBytes;
	REQUIRE(bridge.ReadPlane(
		"script-capture-world", ticket, repeated.Planes.front().Resource, 0, byteCount, repeatedBytes, detail
	));
	CHECK(repeatedBytes == bytes);
	REQUIRE(bridge.Release("script-capture-world", ticket, detail));
	CHECK_FALSE(bridge.Poll("script-capture-world", ticket, poll, detail));
}
