// Analytic planes exercise actual graph images. The oracle uses pinhole rays,
// independent of ResolveCamera, renderer matrices and shader projection code.

#include "RenderFixture.hpp"

#include <engine/assets/Mesh.hpp>
#include <engine/assets/Texture.hpp>
#include <engine/graph/PipelineCatalogue.hpp>
#include <engine/graph/PipelineDocument.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/testing/Suite.hpp>

#include <glm/ext/matrix_clip_space.hpp>

#include <array>
#include <cmath>
#include <limits>
#include <sstream>

TEST_SUITE_ID("engine.render.fixtures")
TEST_DEPENDS("engine.render.imagecomparison")
TEST_DEPENDS("engine.graph.pipelinedocument")

namespace {

	using namespace engine;
	using namespace engine::render::test;

	struct Plane {
		float X = 0;
		float Y = 0;
		float Distance = 4;
		float HalfWidth = 1;
		float HalfHeight = 1;
		uint32_t Colour = 0xFF0000FFu;
	};

	struct ExpectedImages {
		std::vector<uint32_t> Albedo;
		std::vector<float> Depth;
	};

	struct FittedLens {
		double Left = -1;
		double Right = 1;
		double Bottom = -1;
		double Top = 1;
		bool Orthographic = false;
		core::Vector3 ClipNormal{};
		double ClipDistance = 0;
	};

	// Scalar ray intersections stay independent of the fitted GPU matrix.
	ExpectedImages ProjectFittedPlanes(
		std::span<const Plane> planes,
		uint32_t width,
		uint32_t height,
		const scene::Camera &camera,
		const core::CFrame &eye,
		const FittedLens &lens
	) {
		ExpectedImages images;
		images.Albedo.resize(static_cast<size_t>(width) * height);
		images.Depth.resize(images.Albedo.size(), camera.FarPlane);
		for (uint32_t y = 0; y < height; y++) {
			for (uint32_t x = 0; x < width; x++) {
				const size_t pixel = static_cast<size_t>(y) * width + x;
				const double nearX = lens.Left + (lens.Right - lens.Left) * (x + 0.5) / width;
				const double nearY = lens.Top - (lens.Top - lens.Bottom) * (y + 0.5) / height;
				for (const Plane &plane : planes) {
					const double depth = plane.Distance + eye.Position.Z;
					if (depth < camera.NearPlane || depth >= images.Depth[pixel]) {
						continue;
					}
					const double scale = lens.Orthographic ? 1 : depth / camera.NearPlane;
					const double worldX = eye.Position.X + nearX * scale;
					const double worldY = eye.Position.Y + nearY * scale;
					const double clip = lens.ClipNormal.X * worldX + lens.ClipNormal.Y * worldY -
										lens.ClipNormal.Z * plane.Distance;
					if (clip < lens.ClipDistance || std::abs(worldX - plane.X) >= plane.HalfWidth ||
						std::abs(worldY - plane.Y) >= plane.HalfHeight) {
						continue;
					}
					images.Albedo[pixel] = plane.Colour;
					images.Depth[pixel] = static_cast<float>(depth);
				}
			}
		}
		return images;
	}

	// Intersects pixel-centre rays with finite front-facing planes and chooses
	// the nearest in the lens interval. Row zero is the top of the image.
	ExpectedImages ProjectPlanes(
		std::span<const Plane> planes,
		uint32_t width,
		uint32_t height,
		const scene::Camera &camera,
		const core::CFrame &eye
	) {
		ExpectedImages images;
		images.Albedo.resize(static_cast<size_t>(width) * height);
		images.Depth.resize(images.Albedo.size(), camera.FarPlane);
		const double tangent = std::tan(static_cast<double>(camera.FieldOfViewRadians) / 2);
		const double aspect = static_cast<double>(width) / height;
		for (uint32_t y = 0; y < height; y++) {
			for (uint32_t x = 0; x < width; x++) {
				const size_t pixel = static_cast<size_t>(y) * width + x;
				for (const Plane &plane : planes) {
					const double depth = plane.Distance + eye.Position.Z;
					if (depth < camera.NearPlane || depth >= images.Depth[pixel]) {
						continue;
					}
					const double worldX =
						eye.Position.X + (2 * (x + 0.5) / width - 1) * tangent * aspect * depth;
					const double worldY = eye.Position.Y + (1 - 2 * (y + 0.5) / height) * tangent * depth;
					if (std::abs(worldX - plane.X) < plane.HalfWidth &&
						std::abs(worldY - plane.Y) < plane.HalfHeight) {
						images.Albedo[pixel] = plane.Colour;
						images.Depth[pixel] = static_cast<float>(depth);
					}
				}
			}
		}
		return images;
	}

	assets::MeshData PlaneMesh() {
		assets::MeshData mesh;
		mesh.Vertices = {
			{{-0.5f, -0.5f, 0}, {0, 0, 1}, {0, 1}},
			{{0.5f, -0.5f, 0}, {0, 0, 1}, {1, 1}},
			{{0.5f, 0.5f, 0}, {0, 0, 1}, {1, 0}},
			{{-0.5f, 0.5f, 0}, {0, 0, 1}, {0, 0}},
		};
		mesh.Indices = {0, 1, 2, 0, 2, 3};
		mesh.ComputeBounds();
		return mesh;
	}

	std::vector<scene::DrawInstance> DrawPlanes(std::span<const Plane> planes) {
		std::vector<scene::DrawInstance> instances;
		for (const Plane &plane : planes) {
			scene::DrawInstance instance;
			instance.Source = static_cast<uint32_t>(instances.size() + 1);
			instance.Frame.Position = {plane.X, plane.Y, -plane.Distance};
			instance.HalfExtent = {plane.HalfWidth, plane.HalfHeight, 0.01f};
			instance.Tint = {
				static_cast<float>(plane.Colour & 255u) / 255,
				static_cast<float>((plane.Colour >> 8) & 255u) / 255,
				static_cast<float>((plane.Colour >> 16) & 255u) / 255
			};
			instance.Mesh = core::Name("fixture.plane");
			instance.Texture = core::Name("fixture.white");
			instance.CastShadow = false;
			instances.push_back(instance);
		}
		return instances;
	}

	graph::PipelineDocument InstallFixture(render::Renderer &renderer) {
		graph::PipelineDocument document = graph::DefaultPbrDocument();
		// This final consumer keeps captured intermediates alive through every graph write.
		const core::Name captureKind("fixture-capture-boundary");
		graph::NodeKindSpec capture;
		capture.Kind = captureKind;
		capture.Scope = graph::NodeScope::Frame;
		capture.Queue = graph::ExecutionQueue::Cpu;
		capture.Category = graph::NodeCategory::Output;
		for (const auto resource : {"albedo", "linear-depth", "composed-image"}) {
			capture.Inputs.push_back({.Name = core::Name(resource), .Kind = graph::ResourceKind::Texture});
		}
		REQUIRE(graph::RegisterNodeKind(std::move(capture)));
		REQUIRE(renderer.InstallNodeHandler(captureKind, [](const graph::RunContext &) { return true; }));
		document.Record({
			.Kind = graph::EditKind::AddNode,
			.Name = captureKind,
			.NodeKind = captureKind,
			.Scope = graph::NodeScope::Frame,
		});
		for (const auto resource : {"albedo", "linear-depth", "composed-image"}) {
			document.Record({
				.Kind = graph::EditKind::Reads,
				.Target = core::Name(resource),
				.Key = core::Name(resource),
			});
		}
		graph::RenderGraph graph;
		core::Name offender;
		REQUIRE(graph::Build(document, graph, offender) == graph::PipelineDocumentStatus::Ok);
		REQUIRE(renderer.SetPipeline(core::Name("fixture.pbr"), graph));
		REQUIRE(renderer.AddMesh(core::Name("fixture.plane"), PlaneMesh()));
		assets::TextureData white;
		white.Width = 1;
		white.Height = 1;
		white.Format = assets::TextureFormat::RGBA8;
		white.Pixels.assign(4, std::byte{255});
		REQUIRE(renderer.AddTexture(core::Name("fixture.white"), white));
		return document;
	}

	void CheckPlanes(
		render::Renderer &renderer,
		const graph::PipelineDocument &document,
		std::string_view name,
		std::span<const Plane> planes,
		const render::View &view
	) {
		const uint32_t width = view.Target->Width;
		const uint32_t height = view.Target->Height;
		const ExpectedImages expected = ProjectPlanes(planes, width, height, view.Camera, view.CameraFrame);
		std::ostringstream inputs;
		inputs << "slot=" << view.Slot << " world=" << view.World << " fov=" << view.Camera.FieldOfViewRadians
			   << " near=" << view.Camera.NearPlane << " far=" << view.Camera.FarPlane
			   << " eye=" << view.CameraFrame.Position.X << ',' << view.CameraFrame.Position.Y << ','
			   << view.CameraFrame.Position.Z << "\nseed=0 samples=1 projection=perspective\n";
		for (const Plane &plane : planes) {
			inputs << "plane=" << plane.X << ',' << plane.Y << ',' << plane.Distance << ',' << plane.HalfWidth
				   << ',' << plane.HalfHeight << ',' << plane.Colour << '\n';
		}
		inputs << graph::Write(document);
		const CapturedImage albedo = CaptureResource(
			renderer, core::Name("albedo"), view.Slot, width, height, ImageFormat::Rgba8Unorm
		);
		CheckImage(
			renderer,
			name,
			"albedo",
			inputs.str(),
			{width, height, ImageFormat::Rgba8Unorm, std::as_bytes(std::span(expected.Albedo))},
			albedo.View()
		);
		const CapturedImage depth = CaptureResource(
			renderer, core::Name("linear-depth"), view.Slot, width, height, ImageFormat::R32Float
		);
		// D24 quantization propagates through the perspective inverse. At far=32
		// and near=.25, 0.001 covers its worst-case depth error with margin.
		ImageTolerance depthTolerance;
		depthTolerance.Absolute = 0.001;
		CheckImage(
			renderer,
			name,
			"linear-depth",
			inputs.str(),
			{width, height, ImageFormat::R32Float, std::as_bytes(std::span(expected.Depth))},
			depth.View(),
			depthTolerance
		);
	}
}

TEST_CASE(
	"pinhole oracle fixes image orientation, depth order and odd target pitch", "[render][gpu][fixture][.]"
) {
	FixtureDevice fixture;
	fixture.Initialise();
	const auto document = InstallFixture(fixture.Render);
	const std::array planes{
		Plane{-1.1f, 0.7f, 4, 0.72f, 0.46f, 0xFF0000FFu},
		Plane{0.65f, -0.5f, 3, 0.52f, 0.65f, 0xFF00FF00u},
		Plane{0.65f, -0.5f, 6, 1.5f, 1.5f, 0xFFFF0000u},
	};
	const auto instances = DrawPlanes(planes);
	render::View view;
	view.World = 901;
	view.WorldName = core::Name("fixture.projection");
	view.Pipeline = core::Name("fixture.pbr");
	view.Instances = instances;
	view.Camera.FieldOfViewRadians = 1.5707963267948966f;
	view.Camera.NearPlane = 0.25f;
	view.Camera.FarPlane = 32;
	render::OverlayImage overlay;
	for (const render::SceneTarget target : {render::SceneTarget{65, 37}, {113, 71}, {1, 1}}) {
		view.Target = &target;
		const auto frame = fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false);
		REQUIRE(frame.Ran(core::Name("gbuffer")));
		REQUIRE(frame.Ran(core::Name("depth-linearise")));
		CheckPlanes(fixture.Render, document, "pinhole-" + std::to_string(target.Width), planes, view);
	}
}

TEST_CASE("graph images replace stale geometry after edits and camera motion", "[render][gpu][fixture][.]") {
	FixtureDevice fixture;
	fixture.Initialise();
	const auto document = InstallFixture(fixture.Render);
	std::array planes{Plane{0.7f, 0.35f, 3, 0.62f, 0.39f, 0xFF0000FFu}};
	render::SceneTarget target{79, 53};
	render::View view;
	view.World = 902;
	view.WorldName = core::Name("fixture.edits");
	view.Pipeline = core::Name("fixture.pbr");
	view.Target = &target;
	view.Camera.FieldOfViewRadians = 1.5707963267948966f;
	view.Camera.NearPlane = 0.25f;
	view.Camera.FarPlane = 32;
	render::OverlayImage overlay;
	for (uint32_t step = 0; step < 4; step++) {
		if (step == 1) {
			planes[0].X = -0.65f;
			planes[0].Colour = 0xFF00FF00u;
		}
		if (step == 2) {
			view.CameraFrame.Position = {0.35f, -0.2f, 0.5f};
		}
		const std::span<const Plane> visible = step == 3 ? std::span<const Plane>{} : std::span(planes);
		const auto instances = DrawPlanes(visible);
		view.Instances = instances;
		fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false);
		CheckPlanes(fixture.Render, document, "edit-" + std::to_string(step), visible, view);
	}
}

TEST_CASE("pinhole reference has known coverage and rejects clipped planes", "[render][fixture-math]") {
	scene::Camera camera;
	camera.FieldOfViewRadians = 1.5707963267948966f;
	camera.NearPlane = 0.25f;
	camera.FarPlane = 32;
	const std::array planes{Plane{0, 0, 2, 1, 1, 0xFF0000FFu}};
	const ExpectedImages expected = ProjectPlanes(planes, 4, 4, camera, {});
	const std::vector<uint32_t> colours{
		0,
		0,
		0,
		0,
		0,
		0xFF0000FFu,
		0xFF0000FFu,
		0,
		0,
		0xFF0000FFu,
		0xFF0000FFu,
		0,
		0,
		0,
		0,
		0,
	};
	CHECK(expected.Albedo == colours);
	CHECK(expected.Depth[0] == 32);
	CHECK(expected.Depth[5] == 2);
	camera.NearPlane = 2.5f;
	CHECK(ProjectPlanes(planes, 4, 4, camera, {}).Albedo == std::vector<uint32_t>(16));
	camera.NearPlane = 0.25f;
	camera.FarPlane = 1.5f;
	CHECK(ProjectPlanes(planes, 4, 4, camera, {}).Albedo == std::vector<uint32_t>(16));
}

TEST_CASE(
	"graph projection honours changed field of view and clipping distances", "[render][gpu][fixture][.]"
) {
	FixtureDevice fixture;
	fixture.Initialise();
	const auto document = InstallFixture(fixture.Render);
	const std::array planes{
		Plane{0, 0, 0.2f, 2, 2, 0xFFFFFFFFu},
		Plane{-0.6f, 0.4f, 4, 0.47f, 0.32f, 0xFF0000FFu},
		Plane{0.5f, -0.3f, 5, 0.54f, 0.62f, 0xFF00FF00u},
		Plane{0, 0, 20, 20, 20, 0xFFFF0000u},
	};
	const auto instances = DrawPlanes(planes);
	const render::SceneTarget target{97, 61};
	render::View view;
	view.World = 903;
	view.WorldName = core::Name("fixture.lens");
	view.Pipeline = core::Name("fixture.pbr");
	view.Instances = instances;
	view.Target = &target;
	view.Camera.FieldOfViewRadians = 1.0471975511965977f;
	view.Camera.NearPlane = 0.5f;
	view.Camera.FarPlane = 12;
	render::OverlayImage overlay;
	fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false);
	CheckPlanes(fixture.Render, document, "changed-lens", planes, view);
}

TEST_CASE(
	"fitted camera projections preserve clipping and linear depth", "[render][gpu][fixture][projection][.]"
) {
	FixtureDevice fixture;
	fixture.Initialise();
	const auto document = InstallFixture(fixture.Render);
	const std::array planes{
		Plane{-1, 0.5f, 4, 0.8f, 0.6f, 0xFF0000FFu},
		Plane{3.7f, -0.3f, 3, 0.6f, 0.7f, 0xFF00FF00u},
		Plane{0.2f, -0.4f, 6, 4, 3, 0xFFFF0000u},
		Plane{0, 0, 0.1f, 20, 20, 0xFFFFFFFFu},
	};
	const auto instances = DrawPlanes(planes);
	const render::SceneTarget target{107, 73};
	render::View view;
	view.World = 906;
	view.WorldName = core::Name("fixture.fitted-camera");
	view.Pipeline = core::Name("fixture.pbr");
	view.Target = &target;
	view.Instances = instances;
	view.Camera.NearPlane = 0.5f;
	view.Camera.FarPlane = 32;
	view.Camera.FieldOfViewRadians = 1.0471975511965977f;
	render::OverlayImage overlay;
	for (const bool placed : {false, true}) {
		// Apply the same rigid placement to camera and geometry. The independent
		// local-space pinhole oracle is unchanged, while graph culling must use
		// both the fitted projection and the non-identity camera transform.
		view.CameraFrame = placed ? core::CFrame::LookAt({30, 4, 10}, {29, 3, 6}) : core::CFrame{};
		auto placedInstances = instances;
		for (auto &instance : placedInstances) {
			instance.Frame = view.CameraFrame * instance.Frame;
		}
		view.Instances = placedInstances;
		for (const bool orthographic : {false, true}) {
			for (const bool clipped : {false, true}) {
				// The oblique replacement tilts the far plane as well. All fixture
				// geometry lies well inside that plane; the independent near half-space
				// is the boundary tested here.
				if (orthographic && clipped) {
					continue;
				}
				FittedLens lens;
				lens.Orthographic = orthographic;
				lens.Left = orthographic ? -2.5 : -0.3;
				lens.Right = orthographic ? 4.5 : 0.9;
				lens.Bottom = orthographic ? -2 : -0.4;
				lens.Top = orthographic ? 2 : 0.6;
				if (clipped) {
					lens.ClipNormal = {0.25f, 0.1f, -1};
					lens.ClipDistance = 4.2;
				}
				view.Projection = orthographic ? glm::ortho(
													 static_cast<float>(lens.Left),
													 static_cast<float>(lens.Right),
													 static_cast<float>(lens.Bottom),
													 static_cast<float>(lens.Top),
													 view.Camera.NearPlane,
													 view.Camera.FarPlane
												 )
											   : glm::frustum(
													 static_cast<float>(lens.Left),
													 static_cast<float>(lens.Right),
													 static_cast<float>(lens.Bottom),
													 static_cast<float>(lens.Top),
													 view.Camera.NearPlane,
													 view.Camera.FarPlane
												 );
				if (clipped) {
					view.Projection = scene::ObliqueProjection(
						*view.Projection,
						view.CameraFrame,
						view.CameraFrame.VectorToWorldSpace(lens.ClipNormal),
						static_cast<float>(lens.ClipDistance) +
							view.CameraFrame.VectorToWorldSpace(lens.ClipNormal)
								.Dot(view.CameraFrame.Position)
					);
				}
				const ExpectedImages expected =
					ProjectFittedPlanes(planes, target.Width, target.Height, view.Camera, {}, lens);
				REQUIRE(std::count_if(expected.Albedo.begin(), expected.Albedo.end(), [](uint32_t pixel) {
							return pixel != 0;
						}) > 100);
				const auto frame = fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false);
				REQUIRE(frame.Ran(core::Name("gbuffer")));
				REQUIRE(frame.Ran(core::Name("depth-linearise")));
				const std::string name = std::string(
											 orthographic ? "orthographic"
											 : clipped	  ? "oblique"
														  : "off-axis"
										 ) +
										 (placed ? "-placed" : "");
				std::ostringstream inputs;
				inputs << "projection=" << name << " extents=" << lens.Left << ',' << lens.Right << ','
					   << lens.Bottom << ',' << lens.Top << " near=" << view.Camera.NearPlane
					   << " far=" << view.Camera.FarPlane << " clip=" << lens.ClipNormal.X << ','
					   << lens.ClipNormal.Y << ',' << lens.ClipNormal.Z << ',' << lens.ClipDistance
					   << "\ncamera=" << view.CameraFrame.Position.X << ',' << view.CameraFrame.Position.Y
					   << ',' << view.CameraFrame.Position.Z << " placed=" << placed << "\nseed=0 samples=1\n"
					   << graph::Write(document);
				const CapturedImage albedo = CaptureResource(
					fixture.Render,
					core::Name("albedo"),
					0,
					target.Width,
					target.Height,
					ImageFormat::Rgba8Unorm
				);
				CheckImage(
					fixture.Render,
					name,
					"albedo",
					inputs.str(),
					{target.Width,
					 target.Height,
					 ImageFormat::Rgba8Unorm,
					 std::as_bytes(std::span(expected.Albedo))},
					albedo.View()
				);
				const CapturedImage depth = CaptureResource(
					fixture.Render,
					core::Name("linear-depth"),
					0,
					target.Width,
					target.Height,
					ImageFormat::R32Float
				);
				ImageTolerance tolerance;
				tolerance.Absolute = 0.001;
				CheckImage(
					fixture.Render,
					name,
					"linear-depth",
					inputs.str(),
					{target.Width,
					 target.Height,
					 ImageFormat::R32Float,
					 std::as_bytes(std::span(expected.Depth))},
					depth.View(),
					tolerance
				);
			}
		}
	}
	view.CameraFrame = {};
	view.Instances = instances;
	for (const float invalid : {0.0f, std::numeric_limits<float>::infinity()}) {
		view.Projection = glm::mat4{invalid};
		const auto refused = fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false);
		CHECK_FALSE(refused.Ran(core::Name("gbuffer")));
	}
	view.Projection.reset();
	fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false);
	CheckPlanes(fixture.Render, document, "fitted-to-perspective", planes, view);
}

TEST_CASE("batched cameras keep world residency and view images separate", "[render][gpu][fixture][.]") {
	FixtureDevice fixture;
	fixture.Initialise();
	const auto document = InstallFixture(fixture.Render);
	const std::array firstPlanes{Plane{-0.5f, 0.4f, 3, 0.43f, 0.37f, 0xFF0000FFu}};
	std::array secondPlanes{Plane{0.6f, -0.35f, 4, 0.57f, 0.46f, 0xFFFF0000u}};
	const auto firstInstances = DrawPlanes(firstPlanes);
	auto secondInstances = DrawPlanes(secondPlanes);
	const render::SceneTarget target{83, 59};
	std::array<render::View, 3> views;
	for (size_t index = 0; index < views.size(); index++) {
		auto &view = views[index];
		view.Slot = index;
		view.World = index == 1 ? 905 : 904;
		view.WorldName = core::Name(index == 1 ? "fixture.world-b" : "fixture.world-a");
		view.Pipeline = core::Name("fixture.pbr");
		view.Instances = index == 1 ? std::span(secondInstances) : std::span(firstInstances);
		view.Damage.Objects = true;
		view.Target = &target;
		view.Camera.FieldOfViewRadians = 1.5707963267948966f;
		view.Camera.NearPlane = 0.25f;
		view.Camera.FarPlane = 32;
	}
	views[2].CameraFrame.Position.X = 0.4f;
	render::OverlayImage overlay;
	fixture.Render.Render(views, overlay, nullptr, false);
	CheckPlanes(fixture.Render, document, "world-a-first", firstPlanes, views[0]);
	CheckPlanes(fixture.Render, document, "world-b-first", secondPlanes, views[1]);
	CheckPlanes(fixture.Render, document, "world-a-second-camera", firstPlanes, views[2]);
	views[0].Damage = {};
	views[2].Damage = {};
	secondPlanes[0].Y = 0.6f;
	secondInstances = DrawPlanes(secondPlanes);
	views[1].Instances = secondInstances;
	views[1].Damage.Objects = true;
	fixture.Render.Render(views, overlay, nullptr, false);
	CheckPlanes(fixture.Render, document, "world-a-retained", firstPlanes, views[0]);
	CheckPlanes(fixture.Render, document, "world-b-edited", secondPlanes, views[1]);
}
