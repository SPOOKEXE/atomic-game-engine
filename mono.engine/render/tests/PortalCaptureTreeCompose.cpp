#include "RenderFixture.hpp"

#include <engine/assets/Mesh.hpp>
#include <engine/assets/Texture.hpp>
#include <engine/core/Bytes.hpp>
#include <engine/graph/PipelineDocument.hpp>
#include <engine/render/PortalCaptureTreeImport.hpp>
#include <engine/render/PortalGeometry.hpp>
#include <engine/render/PortalImageRuntime.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/generators/catch_generators.hpp>
#include <glm/packing.hpp>

TEST_SUITE_ID("engine.render.portalcapturetreecompose")
TEST_DEPENDS("engine.render.portalcapturetreeimport")

namespace {
	using namespace engine;
	using namespace engine::render;
	constexpr uint32_t EXTENT = 65;
	const core::Name MESH("nested-oracle-plane"), EMISSION("nested-oracle-emission");
	const core::Name NATIVE_PIPELINE("nested-native-reference");

	scene::DrawInstance Panel(
		uint64_t source, float x, float depth, float width, float height, core::Color3 colour, float y = 0
	) {
		scene::DrawInstance row;
		row.Source = source;
		row.Frame.Position = {x, y, -depth};
		row.HalfExtent = {width, height, .001f};
		row.Mesh = MESH;
		row.CastShadow = false;
		row.EmissiveMap = EMISSION;
		row.EmissiveTint = colour;
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

	assets::MeshData WeightedBody() {
		assets::MeshData mesh;
		mesh.JointCount = 2;
		mesh.Vertices = {
			{{-1, -1, 0}, {0, 0, 1}, {0, 1}},
			{{1, -1, 0}, {0, 0, 1}, {1, 1}},
			{{1, 1, 0}, {0, 0, 1}, {1, 0}},
			{{-1, 1, 0}, {0, 0, 1}, {0, 0}}
		};
		for (size_t index = 0; index < mesh.Vertices.size(); ++index) {
			auto &vertex = mesh.Vertices[index];
			vertex.Joints[0] = 0;
			vertex.Joints[1] = 1;
			vertex.Weights[0] = index == 0 || index == 3 ? 49151 : 16384;
			vertex.Weights[1] = 65535 - vertex.Weights[0];
		}
		mesh.Indices = {0, 1, 2, 0, 2, 3};
		mesh.ComputeBounds();
		return mesh;
	}

	assets::MeshData
	DeformedBody(const assets::MeshData &weighted, const std::array<core::CFrame, 2> &palette) {
		auto mesh = weighted;
		mesh.JointCount = 0;
		for (auto &vertex : mesh.Vertices) {
			const glm::dvec3 original(vertex.Position[0], vertex.Position[1], vertex.Position[2]);
			const glm::dvec3 originalNormal(vertex.Normal[0], vertex.Normal[1], vertex.Normal[2]);
			glm::dvec3 position{}, normal{};
			for (size_t influence = 0; influence < 2; ++influence) {
				const auto &joint = palette[vertex.Joints[influence]];
				const auto rotation = glm::mat3_cast(glm::normalize(glm::dquat(joint.Rotation())));
				const glm::dvec3 translation(joint.Position.X, joint.Position.Y, joint.Position.Z);
				const double weight = double(vertex.Weights[influence]) / 65535.;
				position += (rotation * original + translation) * weight;
				normal += rotation * originalNormal * weight;
			}
			for (size_t axis = 0; axis < 3; ++axis) {
				vertex.Position[axis] = float(position[axis]);
				vertex.Normal[axis] = float(normal[axis]);
			}
			std::fill(std::begin(vertex.Joints), std::end(vertex.Joints), 0);
			std::fill(std::begin(vertex.Weights), std::end(vertex.Weights), 0);
		}
		mesh.ComputeBounds();
		return mesh;
	}

	scene::DrawInstance Placed(scene::DrawInstance row, const core::CFrame &frame) {
		row.Frame = frame * row.Frame;
		return row;
	}

	void PlaceEdge(PortalCaptureTreeEdge &edge, const core::CFrame &parent, const core::CFrame &child) {
		const auto vector = [](const auto &v) { return core::Vector3{v[0], v[1], v[2]}; };
		const auto array = [](const core::Vector3 &v) { return std::array{v.X, v.Y, v.Z}; };
		edge.Centre = array(parent.PointToWorldSpace(vector(edge.Centre)));
		edge.First = array(parent.VectorToWorldSpace(vector(edge.First)));
		edge.Second = array(parent.VectorToWorldSpace(vector(edge.Second)));
		const auto rotation = child.Rotation() * glm::inverse(parent.Rotation());
		const core::CFrame turn({}, rotation);
		edge.Position = array(child.Position - turn.VectorToWorldSpace(parent.Position) * edge.Scale);
		edge.Orientation = {rotation.x, rotation.y, rotation.z, rotation.w};
		PortalGeometry geometry;
		std::string error;
		REQUIRE(DecodePortalGeometry(edge.Geometry, geometry, error));
		for (auto &row : geometry.Rows) {
			const auto &p = row.Pose;
			const auto frame = parent * core::CFrame({p[0], p[1], p[2]}, glm::quat(p[6], p[3], p[4], p[5]));
			const auto q = frame.Rotation();
			row.Pose = {frame.Position.X, frame.Position.Y, frame.Position.Z, q.x, q.y, q.z, q.w};
		}
		REQUIRE(EncodePortalGeometry(geometry, edge.Geometry, error));
	}

	void InstallNative(Renderer &renderer) {
		using namespace graph;
		PipelineDocument document;
		const auto native = DefaultPbrDocument();
		for (const auto &edit : native.Edits()) {
			if (edit.Kind == EditKind::AddNode && edit.Name == core::Name("present")) {
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
			}
			document.Record(edit);
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
		}
		RenderGraph pipeline;
		core::Name offender;
		REQUIRE(Build(document, pipeline, offender) == PipelineDocumentStatus::Ok);
		REQUIRE(renderer.SetPipeline(NATIVE_PIPELINE, pipeline));
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
		assets::TextureData white;
		white.Width = white.Height = 1;
		white.Format = assets::TextureFormat::RGBA8;
		white.Pixels.assign(4, std::byte{255});
		REQUIRE(renderer.AddTexture(EMISSION, white));
	}
	std::vector<std::byte> ReadComposed(Renderer &renderer, const char *resource, uint32_t pixelBytes);
	struct NativeImage {
		ResourceImage Export;
		std::vector<std::byte> HardwareDepth;
	};
	NativeImage NativeCapture(
		Renderer &renderer, View view, std::span<const scene::DrawInstance> rows, bool room = false
	) {
		view.Pipeline = NATIVE_PIPELINE;
		view.Instances = rows;
		const core::Name node(room ? "room-export" : "native-export");
		const auto token = renderer.QueueResourceImage(NATIVE_PIPELINE, node);
		REQUIRE(token != 0);
		OverlayImage overlay;
		REQUIRE(renderer.Render(std::span(&view, 1), overlay, nullptr, false).Ran(node));
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while (std::chrono::steady_clock::now() < deadline) {
			if (auto result = renderer.TakeResourceImage(token)) {
				REQUIRE(result->Status == ResourceImageStatus::Ok);
				return {std::move(*result), ReadComposed(renderer, "depth", 4)};
			}
			SDL_Delay(1);
		}
		FAIL("native room capture did not complete");
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
	std::vector<float> Depth(std::span<const std::byte> bytes) {
		std::vector<float> depth;
		core::ByteReader reader(bytes);
		while (!reader.AtEnd())
			depth.push_back(reader.ReadFloat());
		return depth;
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
	"nested scaled apertures compose current body in each room depth domain",
	"[render][gpu][portal-tree-compose][.]"
) {
	const bool transformed = GENERATE(false, true);
	const bool skinned = GENERATE(false, true);
	CAPTURE(transformed, skinned);
	test::FixtureDevice fixture;
	fixture.Initialise();
	auto &renderer = fixture.Render;
	InstallNative(renderer);
	const core::Name skinMesh("nested-weighted-body");
	const std::array<core::Name, 2> referenceNames{
		core::Name("nested-body-pose-0"), core::Name("nested-body-pose-1")
	};
	const std::array<std::array<core::CFrame, 2>, 2> palettes{
		{{core::CFrame({-.12f, .06f, 0}, glm::quat(glm::vec3{0, 0, .16f})),
		  core::CFrame({.1f, -.08f, 0}, glm::quat(glm::vec3{0, 0, -.2f}))},
		 {core::CFrame({.14f, -.09f, 0}, glm::quat(glm::vec3{0, 0, -.25f})),
		  core::CFrame({-.11f, .13f, 0}, glm::quat(glm::vec3{0, 0, .23f}))}}
	};
	std::array<assets::MeshData, 2> deformed;
	if (skinned) {
		const auto weighted = WeightedBody();
		REQUIRE(weighted.IsValid());
		REQUIRE(renderer.AddMesh(skinMesh, weighted));
		for (size_t pose = 0; pose < palettes.size(); ++pose) {
			deformed[pose] = DeformedBody(weighted, palettes[pose]);
			REQUIRE(deformed[pose].IsValid());
			REQUIRE(renderer.AddMesh(referenceNames[pose], deformed[pose]));
		}
	}
	SceneTarget target{EXTENT, EXTENT};
	View baseView;
	baseView.World = 7;
	baseView.WorldName = core::Name("nested-consumer");
	baseView.Target = &target;
	PortalCaptureLighting lighting;
	std::array<SceneLight, MAX_PORTAL_CAPTURE_LIGHTS> lights{};
	REQUIRE(ResolvePortalCaptureLighting(lighting, baseView, lights));
	std::array<std::vector<scene::DrawInstance>, 3> rooms;
	rooms[0] = Frame(100, 2, 1.6f, {.25f, 0, 0});
	rooms[1] = Frame(200, 8, 2.4f, {0, .25f, 0});
	rooms[1].push_back(Panel(210, -.8f, 6, .2f, 3, {.5f, 0, .5f}));
	rooms[2].push_back(Panel(300, 0, 6, 30, 30, {0, 0, 1}));
	const std::array<float, 3> rootToNode{1, 2, .5f};
	std::array<core::CFrame, 3> roomFrames{};
	core::CFrame aim;
	if (transformed) {
		roomFrames = {
			core::CFrame({3, -2, 1}, glm::quat(glm::vec3{.07f, .23f, -.11f})),
			core::CFrame({-4, 1, 6}, glm::quat(glm::vec3{-.19f, -.41f, .17f})),
			core::CFrame({2, 5, -3}, glm::quat(glm::vec3{.13f, .62f, -.27f}))
		};
		aim = core::CFrame({}, glm::quat(glm::vec3{0, -.05f, .035f}));
	}
	const auto planeDepth = [&](float distance, size_t pixelX) {
		const float x = 2.f * (float(pixelX) + .5f) / EXTENT - 1.f;
		const auto ray = aim.VectorToWorldSpace({x, 0, -1});
		return distance / -ray.Z;
	};
	std::array<View, 3> views;
	PortalCaptureTree original;
	for (size_t index = 0; index < rooms.size(); ++index) {
		PortalCaptureTreeNode node;
		node.Producer = {"native-room-" + std::to_string(index), "portal-image-requests", 1, index + 1};
		const float near = .1f * rootToNode[index];
		node.Camera.Frustum = {-near, near, -near, near, near, 100 * rootToNode[index]};
		node.Camera.Projection = index == 0 ? PortalImageProjection::Eye : PortalImageProjection::Seam;
		const auto cameraFrame = roomFrames[index] * aim;
		const auto rotation = cameraFrame.Rotation();
		node.Camera.Position = {cameraFrame.Position.X, cameraFrame.Position.Y, cameraFrame.Position.Z};
		node.Camera.Orientation = {rotation.x, rotation.y, rotation.z, rotation.w};
		if (index != 0) {
			const float entrance = index == 1 ? 4.f : 2.f;
			const auto normal = roomFrames[index].VectorToWorldSpace({0, 0, -1});
			node.Camera.ClipPlane = {
				normal.X,
				normal.Y,
				normal.Z,
				-entrance + scene::PortalClipBias(entrance) - normal.Dot(roomFrames[index].Position)
			};
		}
		views[index] = baseView;
		const PortalCaptureCamera camera{
			node.Camera.Position, node.Camera.Orientation, node.Camera.Frustum, node.Camera.ClipPlane
		};
		REQUIRE(ResolvePortalCaptureCamera(camera, node.Camera.Projection, views[index]));
		std::vector<scene::DrawInstance> placedRoom;
		for (const auto &row : rooms[index])
			placedRoom.push_back(Placed(row, roomFrames[index]));
		auto capture = NativeCapture(renderer, views[index], placedRoom, true);
		auto &captured = capture.Export;
		auto &opaque = node.Layers.Opaque;
		opaque.Key = {index + 1, "room-" + std::to_string(index), 1, 1};
		opaque.Status = PortalImageStatus::Ok;
		opaque.Scope = PortalImageScope::OpaqueLighting;
		opaque.CaptureLighting = lighting;
		opaque.Width = captured.Width;
		opaque.Height = captured.Height;
		opaque.RowStride = captured.RowStride;
		opaque.Pixels = std::move(captured.Pixels);
		opaque.Depth = std::move(captured.Depth);
		opaque.PixelHash = assets::Hasher::Of(opaque.Pixels);
		opaque.DepthHash = assets::Hasher::Of(opaque.Depth);
		original.Nodes.push_back(std::move(node));
	}
	original.Edges = {Edge(0, 2, 1.6f, 2), Edge(1, 8, 2.4f, .25f)};
	for (auto &edge : original.Edges)
		PlaceEdge(edge, roomFrames[edge.Parent], roomFrames[edge.Child]);
	if (transformed) {
		for (const auto &edge : original.Edges) {
			CHECK((edge.Position != std::array<float, 3>{}));
			CHECK((edge.Orientation != std::array<float, 4>{0, 0, 0, 1}));
		}
	}
	REQUIRE(ValidPortalCaptureTree(original));
	for (int root = 2; root >= 0; --root) {
		CAPTURE(root);
		PortalCaptureTree tree;
		tree.Nodes.assign(original.Nodes.begin() + root, original.Nodes.end());
		for (const auto &edge : original.Edges) {
			if (edge.Parent < root) continue;
			auto mapped = edge;
			mapped.Parent -= root;
			mapped.Child -= root;
			tree.Edges.push_back(std::move(mapped));
		}
		REQUIRE(ValidPortalCaptureTree(tree));
		PortalImageBinding binding;
		binding.World = baseView.World;
		binding.WorldName = baseView.WorldName;
		binding.Portal = core::Name(tree.Nodes[0].Layers.Opaque.Key.PortalKey);
		binding.Expected = tree.Nodes[0].Layers.Opaque.Key;
		binding.ExpectedScope = PortalImageScope::OpaqueLighting;
		binding.ExpectedProjection = tree.Nodes[0].Camera.Projection;
		binding.Sampling =
			scene::ResolveSurfaceCamera(views[root].CameraFrame, *views[root].Projection).ViewProjection;
		const auto token = renderer.QueuePortalCaptureTree(binding, std::move(tree));
		REQUIRE(token != 0);
		OverlayImage overlay;
		renderer.Render(std::span(&views[root], 1), overlay, nullptr, false);
		REQUIRE(renderer.PortalCaptureTreeReady(token));
		const auto uploadBytes = renderer.PortalImageUsage().UploadedBytes;
		std::vector<float> previous;
		for (int pose = 0; pose < 2; ++pose) {
			CAPTURE(pose);
			// The two seam scales map C distance4 to B16 and A8.
			auto childBody = Panel(400, !skinned && pose ? .4f : 0, 4, .7f, .7f, {1, 1, 0});
			auto nativeChild = childBody;
			if (skinned) {
				childBody.Mesh = skinMesh;
				childBody.SkinCount = 2;
				nativeChild.Mesh = referenceNames[pose];
				const auto &mesh = deformed[pose];
				// Preserve the original mesh scale despite the deformed bounds.
				nativeChild.Frame.Position =
					nativeChild.Frame.Position + (mesh.Minimum + mesh.Maximum) * (.5f * .7f);
				const auto extent = (mesh.Maximum - mesh.Minimum) * .5f;
				nativeChild.HalfExtent = {extent.X * .7f, extent.Y * .7f, .001f};
			}
			const float bodyScale = rootToNode[root] / rootToNode[2];
			auto body = Placed(Scaled(childBody, bodyScale), roomFrames[root]);
			auto nativeBody = Placed(Scaled(nativeChild, bodyScale), roomFrames[root]);
			if (skinned) {
				const auto normal = roomFrames[root].VectorToWorldSpace({1, 0, 0});
				body.SeamNormal = nativeBody.SeamNormal = normal;
				body.SeamOffset = nativeBody.SeamOffset =
					normal.Dot(roomFrames[root].Position) - .15f * bodyScale;
			}
			std::vector<scene::DrawInstance> unfolded;
			for (size_t room = root; room < rooms.size(); ++room)
				for (const auto &row : rooms[room])
					unfolded.push_back(
						Placed(Scaled(row, rootToNode[root] / rootToNode[room]), roomFrames[root])
					);
			unfolded.push_back(nativeBody);
			const auto nativeCapture = NativeCapture(renderer, views[root], unfolded);
			const auto &native = nativeCapture.Export;
			const size_t centre = size_t(EXTENT / 2) * EXTENT + EXTENT / 2;
			const auto rawDepth = Depth(nativeCapture.HardwareDepth)[centre];
			REQUIRE(std::isfinite(rawDepth));
			REQUIRE(rawDepth > 0);
			REQUIRE(rawDepth < 1);
			const auto vp =
				scene::ResolveSurfaceCamera(views[root].CameraFrame, *views[root].Projection).ViewProjection;
			// Start from actual raster depth: subpixel triangle snapping changes the
			// sampled plane. Independently check the export's world-to-camera units.
			const auto homogeneous = glm::inverse(glm::dmat4(vp)) * glm::dvec4(0, 0, rawDepth, 1);
			const auto point = glm::dvec3(homogeneous) / homogeneous.w;
			const auto &camera = views[root].CameraFrame;
			const auto forward = camera.LookVector();
			const auto relative = point - glm::dvec3(camera.Position.X, camera.Position.Y, camera.Position.Z);
			const double rasterDistance = glm::dot(relative, glm::dvec3(forward.X, forward.Y, forward.Z));
			const float nativeDistance = Depth(native.Depth)[centre];
			const float idealDistance = planeDepth(4 * rootToNode[root] / rootToNode[2], EXTENT / 2);
			CAPTURE(rawDepth, rasterDistance, nativeDistance, idealDistance);
			CHECK(std::abs(nativeDistance - rasterDistance) <= .0001);
			const auto expected = Colour(native.Pixels);
			auto bodyView = views[root];
			bodyView.Instances = std::span(&body, 1);
			if (skinned) bodyView.JointFrames = palettes[pose];
			const auto composed = renderer.ComposePortalCaptureTree(token, bodyView);
			REQUIRE(composed != 0);
			const auto actual = Colour(ReadComposed(renderer, "composed-colour", 8));
			const auto depth = Depth(ReadComposed(renderer, "composed-depth", 4));
			const auto image = [](const auto &samples) {
				return test::ImageView{
					EXTENT,
					EXTENT,
					test::ImageFormat::Rgba32Float,
					std::as_bytes(std::span(samples)),
					EXTENT * 16
				};
			};
			test::CheckImage(
				renderer,
				"nested-body-depth",
				std::string(skinned ? "skin-" : "") + (transformed ? "transformed-" : "aligned-") + "room-" +
					std::to_string(root) + "-pose-" + std::to_string(pose),
				skinned
					? "CPU-deformed native body versus retained nested rooms with GPU palette and ancestor "
					  "clip"
					: "native unfolded rooms versus independent retained room tree with current yellow body",
				image(expected),
				image(actual),
				{.Absolute = .002, .Region = {}}
			);
			REQUIRE(actual[centre * 4] > .9f);
			REQUIRE(actual[centre * 4 + 1] > .9f);
			if (root == 2) CHECK(std::abs(depth[centre] - nativeDistance) <= .0001f);
			const float expectedDepth = root == 2 ? 4 : root == 1 ? 8 : 2;
			CHECK(std::abs(depth[centre] - planeDepth(expectedDepth, EXTENT / 2)) <= .0001f);
			// Each selected ray crosses the narrow B blocker before the nested mouth.
			if (root <= 1) {
				const size_t blockerX = transformed ? 26 : 28;
				const size_t blocked = size_t(EXTENT / 2) * EXTENT + blockerX;
				CHECK(actual[blocked * 4] > .49f);
				CHECK(actual[blocked * 4 + 1] < .01f);
				CHECK(actual[blocked * 4 + 2] > .49f);
				CHECK(std::abs(depth[blocked] - planeDepth(root == 1 ? 6.f : 2.f, blockerX)) <= .0001f);
			}
			if (skinned) {
				const size_t clipped = size_t(EXTENT / 2) * EXTENT + 29;
				CHECK(actual[clipped * 4] < .002f);
				CHECK(actual[clipped * 4 + 1] < .002f);
				CHECK(actual[clipped * 4 + 2] > .99f);
			}
			if (pose != 0) CHECK(actual != previous);
			previous = actual;
			CHECK(renderer.PortalImageUsage().UploadedBytes == uploadBytes);
			if (!skinned && !transformed && root == 0 && pose == 1) {
				const auto retainedUsage = renderer.PortalImageUsage();
				const auto replacement = renderer.ComposePortalCaptureTree(token, bodyView);
				REQUIRE(replacement != 0);
				CHECK(replacement != composed);
				CHECK(renderer.PortalImageUsage().Images == retainedUsage.Images + 1);
				CHECK(renderer.DropPortalImage(replacement));

				// One free slot admits the leaf, then the next parent must refuse.
				// The previous root and all preexisting imports must survive rollback.
				std::vector<uint64_t> fillers;
				while (renderer.PortalImageUsage().Images + 1 < MAX_IMPORTED_PORTAL_IMAGES) {
					auto reply = original.Nodes.back().Layers.Opaque;
					auto fillerBinding = binding;
					fillerBinding.Portal = core::Name("tree-capacity-" + std::to_string(fillers.size()));
					reply.Key.PortalKey = fillerBinding.Portal.Text();
					fillerBinding.Expected = reply.Key;
					const auto filler = renderer.QueuePortalImage(fillerBinding, std::move(reply));
					REQUIRE(filler != 0);
					fillers.push_back(filler);
				}
				// Ordinary imports allocate their textures when uploads are submitted.
				renderer.Render(std::span(&views[root], 1), overlay, nullptr, false);
				REQUIRE(renderer.PortalImageUsage().PendingCpuBytes == 0);
				const auto fullUsage = renderer.PortalImageUsage();
				REQUIRE(fullUsage.Images == MAX_IMPORTED_PORTAL_IMAGES - 1);
				CHECK(renderer.ComposePortalCaptureTree(token, bodyView) == 0);
				CHECK(renderer.PortalImageUsage().Images == fullUsage.Images);
				CHECK(renderer.PortalImageUsage().TextureBytes == fullUsage.TextureBytes);
				CHECK(renderer.PortalCaptureTreeReady(token));
				for (const auto filler : fillers)
					CHECK(renderer.DropPortalImage(filler));
			}
			CHECK(renderer.DropPortalImage(composed));
		}
		renderer.DropPortalCaptureTree(token);
	}
}
