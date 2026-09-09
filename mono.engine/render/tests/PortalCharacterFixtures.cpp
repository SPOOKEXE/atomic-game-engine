// A real player walks through the authored pair. A separate, unfolded draw
// list has neither portals nor seam clipping and supplies the image reference.
#include "RenderFixture.hpp"
#include "RenderTypes.hpp"

#include <engine/assets/Builtin.hpp>
#include <engine/core/Paths.hpp>
#include <engine/ecs/Scheduler.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/graph/PipelineCatalogue.hpp>
#include <engine/graph/PipelineDocument.hpp>
#include <engine/physics/Characters.hpp>
#include <engine/physics/PhysicsWorld.hpp>
#include <engine/physics/Pipeline.hpp>
#include <engine/render/ResourceImage.hpp>
#include <engine/render/WorldPresentation.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/scene/Characters.hpp>
#include <engine/scene/Controls.hpp>
#include <engine/scene/Gravity.hpp>
#include <engine/scene/Input.hpp>
#include <engine/scene/Interpolation.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scene/SurfaceCameras.hpp>
#include <engine/scene/Visibility.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/generators/catch_generators.hpp>

#include <array>
#include <cmath>
#include <iostream>
#include <sstream>

TEST_SUITE_ID("engine.render.portalcharacterfixtures")
TEST_DEPENDS("engine.scene.cameracontinuation")
TEST_DEPENDS("engine.physics.characters")
TEST_DEPENDS("engine.render.worldpresentation")

namespace {
	using namespace engine;
	using namespace engine::render::test;
	using core::CFrame;
	using core::Vector3;
	constexpr uint32_t WIDTH = 129, HEIGHT = 97;
	constexpr float STEP = 1.0f / 60;
	constexpr float PI = 3.14159265359f;
	const core::Name PIPELINE("character.fixture.pbr");
	const core::Name CUBE("character.fixture.cube");
	const core::Name WHITE("character.fixture.white");

	struct Walk {
		ecs::Store World{"character.fixture"};
		ecs::Scheduler Scheduler;
		ecs::Entity Eye, Root, Humanoid, Entrance, Exit;
		Vector3 NearCentre{0, 4, 0}, FarCentre{100, 4, 0};
		CFrame Rotation;
		float Scale;
		bool FirstPerson;
		bool FrontFace;

		Vector3 ToFar(Vector3 point) const {
			return FarCentre + Rotation.VectorToWorldSpace((point - NearCentre) * Scale);
		}
		CFrame ToNear(CFrame frame) const {
			return CFrame(
				NearCentre + Rotation.VectorToObjectSpace(frame.Position - FarCentre) / Scale,
				(Rotation.Inverse() * CFrame(Vector3{}, frame.Rotation())).Rotation()
			);
		}
		ecs::Entity Box(CFrame frame, Vector3 size, core::Color3 tint) {
			scene::PartDesc part;
			part.Frame = frame;
			part.Size = size;
			const auto entity = scene::MakePart(World, part);
			World.SetParent(entity, scene::WorkspaceOf(World));
			World.GetMutable<scene::Visual>(entity)->Tint = tint;
			return entity;
		}
		Walk(bool firstPerson, bool rolled, bool frontFace = false)
			: Rotation(CFrame::Angles(0, 0, rolled ? .63f : 0)), Scale(rolled ? 1.5f : 1),
			  FirstPerson(firstPerson), FrontFace(frontFace) {
			scene::RegisterSceneClasses();
			scene::InstallServices(World);
			render::RegisterPresentationComponents();
			World.SetResource(render::DrawList{});
			physics::PreparePhysicsWorld(World);
			scene::PrepareGravity(World);
			physics::RegisterPhysicsSystems(Scheduler);
			physics::RegisterCharacterSystems(Scheduler);
			scene::RegisterGravitySystem(Scheduler);
			Scheduler.Add("capture-previous", ecs::Phase::PreSimulation, scene::CapturePreviousTransforms);
			Box(CFrame(Vector3{0, -.5f, 8}), {32, 1, 16}, {.3f, .3f, .3f});
			Box(CFrame(ToFar({0, -.5f, -8}), Rotation.Rotation()),
				Vector3{32, 1, 16} * Scale,
				{.3f, .3f, .3f});
			for (float side : {-1.0f, 1.0f})
				Box(CFrame(ToFar({side * 8, 5, -10}), Rotation.Rotation()),
					Vector3{16, 20, .1f} * Scale,
					side < 0 ? core::Color3{.1f, .3f, .8f} : core::Color3{.1f, .8f, .3f});
			Entrance = Box(
				CFrame(NearCentre + Vector3{0, 0, frontFace ? -.05f : .05f}), {10, 8, .1f}, {.2f, .2f, .2f}
			);
			const CFrame exitRotation = Rotation * CFrame::Angles(0, PI, 0);
			Exit = Box(
				CFrame(
					FarCentre + exitRotation.VectorToWorldSpace({0, 0, (frontFace ? -.05f : .05f) * Scale}),
					exitRotation.Rotation()
				),
				Vector3{10, 8, .1f} * Scale,
				{.2f, .2f, .2f}
			);
			for (const auto &pair : {std::pair{Entrance, Exit}, std::pair{Exit, Entrance}}) {
				const auto camera =
					World.CreateInstance(ecs::Classes::Find(core::Name("SurfaceCamera")), "Mouth");
				World.SetParent(camera, pair.first);
				scene::SurfaceCamera surface;
				surface.Face = frontFace ? scene::NormalId::Back : scene::NormalId::Front;
				World.Set(camera, surface);
				World.Set(camera, scene::Portal{pair.second});
			}
			const auto player = scene::AddPlayer(World, "Walker", true);
			const auto model = scene::LoadCharacter(World, player, CFrame(Vector3{0, 0, 2.1f}));
			REQUIRE(model != ecs::NULL_ENTITY);
			const auto rig = *World.Get<scene::Character>(model);
			Root = rig.Root;
			Humanoid = rig.Humanoid;
			World.Each<scene::CharacterLimb, scene::Visual>(
				[](ecs::Entity, const scene::CharacterLimb &, scene::Visual &visual) {
					visual.Tint = {.8f, .1f, .1f};
				}
			);
			Eye = World.CreateInstance(scene::CameraClass(), "PlayerEye");
			auto &lens = *World.GetMutable<scene::Camera>(Eye);
			lens.FieldOfViewRadians = 1.0471975512f;
			lens.NearPlane = .05f;
			lens.FarPlane = 64;
			World.SetResource(scene::ActiveCamera{Eye, static_cast<float>(WIDTH) / HEIGHT});
			scene::CameraController control;
			control.Mode = firstPerson ? scene::CameraMode::LockFirstPerson : scene::CameraMode::Classic;
			control.Distance = firstPerson ? 0 : 6;
			control.HeadHeight = 1.5f;
			control.Basis = CFrame::Angles(0, 0, .17f);
			World.SetResource(control);
			World.SetResource(scene::InputState{});
			REQUIRE(scene::FollowOwnCharacter(World));
			REQUIRE(scene::PlaceCamera(World));
			ecs::Entity selected;
			REQUIRE(World.GetProperty(Eye, core::Name("CameraSubject"), &selected, sizeof(selected)));
			REQUIRE(selected == Humanoid);
			REQUIRE_FALSE(World.Has<scene::Transform>(Humanoid));
			REQUIRE(scene::CameraSubjectRoot(World, Eye) == Root);
		}
		void Present() {
			World.SetFrame(STEP, 1);
			(void)scene::UpdateCameraControl(World);
			(void)physics::UpdatePoppercam(World);
			REQUIRE(scene::PlaceCamera(World));
			(void)scene::PoseCharacters(World);
			(void)scene::AimSurfaceCameras(World);
			(void)scene::SyncRendered(World);
			render::CollectInstances(World);
		}
		void Tick(bool moving) {
			World.ResourceMutable<scene::InputState>()->Down.Set(scene::KeyCode::W, moving);
			(void)scene::UpdateCharacterControl(World);
			Scheduler.Tick(World, STEP);
			Present();
		}
	};

	graph::PipelineDocument Install(render::Renderer &renderer) {
		auto document = graph::DefaultPbrDocument();
		const core::Name observer("character-fixture-capture");
		graph::NodeKindSpec spec;
		spec.Kind = observer;
		spec.Scope = graph::NodeScope::Frame;
		spec.Queue = graph::ExecutionQueue::Cpu;
		spec.Category = graph::NodeCategory::Output;
		spec.Inputs.push_back({.Name = core::Name("tonemapped"), .Kind = graph::ResourceKind::Texture});
		REQUIRE(graph::RegisterNodeKind(std::move(spec)));
		REQUIRE(renderer.InstallNodeHandler(observer, [](const graph::RunContext &) { return true; }));
		document.Record(
			{.Kind = graph::EditKind::AddNode,
			 .Name = observer,
			 .NodeKind = observer,
			 .Scope = graph::NodeScope::Frame}
		);
		document.Record(
			{.Kind = graph::EditKind::Reads,
			 .Target = core::Name("tonemapped"),
			 .Key = core::Name("tonemapped")}
		);
		graph::RenderGraph graph;
		core::Name offender;
		REQUIRE(graph::Build(document, graph, offender) == graph::PipelineDocumentStatus::Ok);
		REQUIRE(renderer.SetPipeline(PIPELINE, graph));
		REQUIRE(renderer.AddMesh(CUBE, assets::MakeBuiltin(assets::BuiltinMesh::Cube)));
		assets::TextureData white;
		white.Width = white.Height = 1;
		white.Format = assets::TextureFormat::RGBA8;
		white.Pixels.assign(4, std::byte{255});
		REQUIRE(renderer.AddTexture(WHITE, white));
		renderer.SetPortalDepth(1);
		return document;
	}

	void
	CompareWalk(FixtureDevice &fixture, Walk &walk, const graph::PipelineDocument &document, unsigned tick) {
		const auto &draw = *walk.World.Resource<render::DrawList>();
		auto actual = draw.Instances;
		std::vector<scene::DrawInstance> unfolded;
		for (size_t index = 0; index < draw.BaseInstanceCount; index++) {
			auto row = actual[index];
			if (row.Source == walk.Entrance.Id || row.Source == walk.Exit.Id) continue;
			if (row.Frame.Position.X > 50) {
				row.Frame = walk.ToNear(row.Frame);
				row.HalfExtent = row.HalfExtent / walk.Scale;
			}
			row.SeamNormal = {};
			row.SeamOffset = 0;
			row.SeamLight = {};
			unfolded.push_back(row);
		}
		for (auto *rows : {&actual, &unfolded})
			for (auto &row : *rows) {
				row.Mesh = CUBE;
				row.Texture = WHITE;
				row.CastShadow = false;
			}
		std::vector<scene::SurfaceSlot> slots;
		scene::GatherSurfaceSlots(walk.World, slots);
		render::ApplySurfaceSlots(actual, slots, core::Name("character.fixture"));
		std::vector<render::PortalView> portals;
		REQUIRE(render::CollectPortalViews(walk.World, portals, slots) == 2);
		const render::SceneTarget target{WIDTH, HEIGHT};
		std::array<render::View, 2> views;
		for (size_t index = 0; index < views.size(); index++) {
			auto &view = views[index];
			view.Slot = index;
			view.World = 1400 + index;
			view.WorldName = core::Name(index ? "character.unfolded" : "character.fixture");
			view.Pipeline = PIPELINE;
			view.Target = &target;
			view.CameraFrame = walk.World.Get<scene::Transform>(walk.Eye)->Frame;
			view.Camera = *walk.World.Get<scene::Camera>(walk.Eye);
			view.OverrideLighting = true;
			view.Lighting.Ambient = {.5f, .5f, .5f};
			view.Lighting.Direct = {};
		}
		if (views[1].CameraFrame.Position.X > 50) views[1].CameraFrame = walk.ToNear(views[1].CameraFrame);
		// The renderer brings its near plane inside the nearest portal.
		// The unfolded chart has no panes, so derive the same pinhole clipping
		// distance directly from this fixture's authored z=0 aperture.
		views[1].Camera.NearPlane = std::min(
			views[1].Camera.NearPlane,
			std::max(scene::PORTAL_NEAR_MIN, std::abs(views[1].CameraFrame.Position.Z) * .5f)
		);
		views[0].Instances = actual;
		views[0].Portals = portals;
		views[1].Instances = unfolded;
		render::OverlayImage overlay;
		fixture.Render.Render(views, overlay, nullptr, false);
		const auto observed = CaptureResource(
			fixture.Render, core::Name("tonemapped"), 0, WIDTH, HEIGHT, ImageFormat::Rgba8Unorm
		);
		const auto reference = CaptureResource(
			fixture.Render, core::Name("tonemapped"), 1, WIDTH, HEIGHT, ImageFormat::Rgba8Unorm
		);
		std::vector<uint32_t> expected(WIDTH * HEIGHT), captured(WIDTH * HEIGHT);
		size_t samples = 0, body = 0;
		const auto eye = views[1].CameraFrame;
		const double tangent = std::tan(views[1].Camera.FieldOfViewRadians * .5);
		for (uint32_t y = 2; y + 2 < HEIGHT; y++)
			for (uint32_t x = 2; x + 2 < WIDTH; x++) {
				const auto ray = eye.VectorToWorldSpace(
					{static_cast<float>((2 * (x + .5) / WIDTH - 1) * tangent * WIDTH / HEIGHT),
					 static_cast<float>((1 - 2 * (y + .5) / HEIGHT) * tangent),
					 -1}
				);
				const float distance = -eye.Position.Z / ray.Z;
				const auto point = eye.Position + ray * distance;
				// Before the camera enters, compare only the geometric aperture.
				// Afterward the forward room is entirely in the destination chart.
				if (eye.Position.Z > 0 &&
					(distance <= 0 || std::abs(point.X) >= 4.7f || std::abs(point.Y - 4) >= 3.7f))
					continue;
				const auto *pixel = reference.Bytes.data() + y * reference.RowStrideBytes + x * 4;
				bool flat = true;
				// A fixed two-pixel filter footprint in the independent image
				// removes only raster boundaries, never disagreements in actual pixels.
				for (int dy = -2; dy <= 2; dy++)
					for (int dx = -2; dx <= 2; dx++)
						flat &=
							std::memcmp(
								pixel,
								reference.Bytes.data() + (y + dy) * reference.RowStrideBytes + (x + dx) * 4,
								3
							) == 0;
				if (!flat) continue;
				const size_t at = y * WIDTH + x;
				std::memcpy(&expected[at], pixel, 4);
				std::memcpy(&captured[at], observed.Bytes.data() + y * observed.RowStrideBytes + x * 4, 4);
				samples++;
				body += std::to_integer<int>(pixel[0]) > std::to_integer<int>(pixel[2]) + 30;
			}
		INFO("tick=" << tick << " samples=" << samples << " body=" << body);
		REQUIRE(samples > 200);
		if (!walk.FirstPerson) REQUIRE(body > 10);
		ImageTolerance tolerance;
		tolerance.Absolute = 2.0 / 255;
		const ImageView expectedView{
			WIDTH, HEIGHT, ImageFormat::Rgba8Unorm, std::as_bytes(std::span(expected))
		};
		const ImageView actualView{
			WIDTH, HEIGHT, ImageFormat::Rgba8Unorm, std::as_bytes(std::span(captured))
		};
		const auto result = CompareImages(expectedView, actualView, tolerance);
		std::cout << "character portal tick=" << tick << " first=" << walk.FirstPerson
				  << " scale=" << walk.Scale << " front=" << walk.FrontFace << " samples=" << samples
				  << " body=" << body << " mismatched=" << result.MismatchedPixels
				  << " max=" << result.MaximumAbsoluteError << " rmse=" << result.RootMeanSquareError << '\n';
		std::ostringstream inputs;
		inputs << "tick=" << tick << " fixed-delta=" << STEP << " first-person=" << walk.FirstPerson
			   << " scale=" << walk.Scale << " front=" << walk.FrontFace
			   << " expected-chart-eye=" << eye.Position.X << ',' << eye.Position.Y << ',' << eye.Position.Z
			   << " samples=" << samples << " body=" << body << "\n"
			   << graph::Write(document);
		inputs << "\nroot=" << walk.World.Get<scene::Transform>(walk.Root)->Frame.Position.X << ','
			   << walk.World.Get<scene::Transform>(walk.Root)->Frame.Position.Y << ','
			   << walk.World.Get<scene::Transform>(walk.Root)->Frame.Position.Z
			   << " actual-eye=" << views[0].CameraFrame.Position.X << ',' << views[0].CameraFrame.Position.Y
			   << ',' << views[0].CameraFrame.Position.Z;
		inputs << "\nreference-near=" << views[1].Camera.NearPlane << " far=" << views[1].Camera.FarPlane
			   << " fov=" << views[1].Camera.FieldOfViewRadians
			   << " aperture-centre=0,4,0 half-extent=5,4 edge-guard=.3 flat-reference-footprint=5x5"
			   << " look=" << eye.LookVector().X << ',' << eye.LookVector().Y << ',' << eye.LookVector().Z
			   << " up=" << eye.UpVector().X << ',' << eye.UpVector().Y << ',' << eye.UpVector().Z;
		for (const auto &row : actual)
			inputs << "\nrow source=" << row.Source << " rig=" << row.Rig
				   << " position=" << row.Frame.Position.X << ',' << row.Frame.Position.Y << ','
				   << row.Frame.Position.Z << " extent=" << row.HalfExtent.X << ',' << row.HalfExtent.Y << ','
				   << row.HalfExtent.Z << " clip=" << row.SeamNormal.X << ',' << row.SeamNormal.Y << ','
				   << row.SeamNormal.Z << ',' << row.SeamOffset << " surface=" << row.Surface;
		CheckImage(
			fixture.Render,
			"character-" + std::to_string(walk.FrontFace) + "-" + std::to_string(walk.FirstPerson) + "-" +
				std::to_string(walk.Scale) + "-" + std::to_string(tick),
			"tonemapped",
			inputs.str(),
			expectedView,
			actualView,
			tolerance
		);
	}
}

TEST_CASE(
	"a Humanoid player camera walks through rolled scaled portal pixels", "[render][gpu][portal-character][.]"
) {
	const bool firstPerson = GENERATE(false, true);
	const bool rolled = GENERATE(false, true);
	const bool frontFace = GENERATE(false, true);
	FixtureDevice fixture;
	fixture.Initialise();
	const auto document = Install(fixture.Render);
	Walk walk(firstPerson, rolled, frontFace);
	walk.Present();
	CompareWalk(fixture, walk, document, 0);
	if (!firstPerson && !rolled && frontFace) {
		const auto obstruction = walk.Box(CFrame(Vector3{0, 4, 5}), {4, 4, .2f}, {.3f, .3f, .3f});
		walk.Tick(false);
		const auto &control = *walk.World.Resource<scene::CameraController>();
		REQUIRE(control.OccludedDistance > 0);
		REQUIRE(control.OccludedDistance < control.Distance - 1);
		CompareWalk(fixture, walk, document, 1000);
		walk.World.Destroy(obstruction);
		walk.Tick(false);
	}

	bool crossed = false;
	bool straddled = false;
	for (unsigned tick = 1; tick <= 32; tick++) {
		const auto before = *walk.World.Resource<scene::CameraController>();
		walk.Tick(true);
		const auto *transit = walk.World.Get<scene::PortalTransit>(walk.Root);
		if (!straddled && transit == nullptr &&
			std::abs(walk.World.Get<scene::Transform>(walk.Root)->Frame.Position.Z) < .5f) {
			const auto &draw = *walk.World.Resource<render::DrawList>();
			REQUIRE(draw.Instances.size() > draw.BaseInstanceCount);
			CompareWalk(fixture, walk, document, tick);
			straddled = true;
		}
		if (transit == nullptr) continue;
		REQUIRE(transit->Serial == 1);
		const auto after = *walk.World.Resource<scene::CameraController>();
		CHECK(
			(after.Basis.UpVector() - walk.Rotation.VectorToWorldSpace(before.Basis.UpVector())).Magnitude() <
			.0002f
		);
		CHECK(
			(after.Basis.LookVector() - walk.Rotation.VectorToWorldSpace(before.Basis.LookVector()))
				.Magnitude() < .0002f
		);
		CHECK(
			(after.Basis.RightVector() - walk.Rotation.VectorToWorldSpace(before.Basis.RightVector()))
				.Magnitude() < .0002f
		);
		CHECK(std::abs(after.Distance - before.Distance * walk.Scale) < .0002f);
		ecs::Entity selected;
		REQUIRE(walk.World.GetProperty(walk.Eye, core::Name("CameraSubject"), &selected, sizeof(selected)));
		CHECK(selected == walk.Humanoid);
		CompareWalk(fixture, walk, document, tick);
		walk.World.ResourceMutable<scene::InputState>()->Down.Set(scene::KeyCode::W, false);
		const auto arrived = walk.World.Get<scene::Transform>(walk.Eye)->Frame;
		walk.Present();
		const auto repeated = walk.World.Get<scene::Transform>(walk.Eye)->Frame;
		CHECK((arrived.Position - repeated.Position).Magnitude() < .0002f);
		CHECK((arrived.UpVector() - repeated.UpVector()).Magnitude() < .0002f);
		CHECK((arrived.LookVector() - repeated.LookVector()).Magnitude() < .0002f);
		CHECK((arrived.RightVector() - repeated.RightVector()).Magnitude() < .0002f);
		CompareWalk(fixture, walk, document, tick + 100);
		crossed = true;
		break;
	}
	REQUIRE(straddled);
	REQUIRE(crossed);
}

TEST_CASE(
	"player collection leaves surface face markers to explicit inspection",
	"[render][presentation][portal-markers]"
) {
	Walk walk(true, false);
	for (unsigned frame = 0; frame < 3; frame++) {
		if (frame == 2)
			walk.Tick(true);
		else
			walk.Present();
		const auto &draw = *walk.World.Resource<render::DrawList>();
		CHECK(std::count_if(draw.Instances.begin(), draw.Instances.end(), [&](const auto &row) {
				  return row.Source == walk.Entrance.Id || row.Source == walk.Exit.Id;
			  }) == 2);
		CHECK(std::none_of(draw.Instances.begin(), draw.Instances.end(), [&](const auto &row) {
			return walk.World.Has<scene::SurfaceCamera>(ecs::Entity{row.Source});
		}));
	}
	std::vector<scene::DrawInstance> inspection;
	REQUIRE(scene::AppendSurfaceFaceMarkers(walk.World, inspection) == 2);
	CHECK(inspection.size() == 2);
}

TEST_CASE(
	"portal probes carry destination emission and local light without duplicating ambient",
	"[render][gpu][portal-radiance][.]"
) {
	const int destinationSource = GENERATE(0, 1, 2, 3);
	// Emission uses an authored 0..16 UNORM8 intensity. Choose an exact step.
	constexpr float EMISSION = 128.0f / 255.0f;
	CAPTURE(destinationSource);
	FixtureDevice fixture;
	fixture.Initialise();
	auto &renderer = fixture.Render;
	const auto installed = Install(renderer);
	(void)installed;
	auto document = graph::DefaultWorldHdrDocument();
	const core::Name pipelineName("portal.constant-radiance"), captureName("radiance-export");
	document.Record(
		{.Kind = graph::EditKind::AddNode,
		 .Name = captureName,
		 .NodeKind = core::Name("capture"),
		 .Scope = graph::NodeScope::Frame}
	);
	document.Record(
		{.Kind = graph::EditKind::Reads, .Target = core::Name("lens-b"), .Key = core::Name("source")}
	);
	graph::RenderGraph graph;
	core::Name offender;
	REQUIRE(graph::Build(document, graph, offender) == graph::PipelineDocumentStatus::Ok);
	REQUIRE(renderer.SetPipeline(pipelineName, graph));
	std::array<scene::DrawInstance, 3> rows;
	for (size_t index = 0; index < rows.size(); index++) {
		rows[index].Source = index + 1;
		rows[index].Mesh = CUBE;
		rows[index].Texture = WHITE;
		rows[index].Tint = {1, 1, 1};
		rows[index].CastShadow = false;
	}
	rows[0].Frame.Position = {0, 0, 3};
	rows[0].HalfExtent = {10, 10, .01f};
	rows[1].HalfExtent = {4, 4, .01f};
	rows[1].Surface = 0;
	rows[2].Frame.Position = {100, 0, destinationSource == 3 ? 3.0f : -3.0f};
	rows[2].HalfExtent = {10, 10, .01f};
	if (destinationSource == 1) {
		rows[2].Tint = {};
		rows[2].EmissiveMap = WHITE;
		rows[2].EmissiveTint = {1, 0, 0};
		rows[2].EmissiveStrength = EMISSION;
	}
	const render::SceneLight destinationLight{.Position = {100, 0, -1}, .Range = 8, .Colour = {1, 0, 0}};
	render::PortalView portal;
	portal.Normal = {0, 0, 1};
	portal.First = {4, 0, 0};
	portal.Second = {0, 4, 0};
	portal.Warp.Frame.Position = {100, 0, 0};
	if (destinationSource == 3) portal.Warp.Frame = CFrame({100, 0, 0}, CFrame::Angles(0, PI, 0).Rotation());
	const render::SceneTarget target{65, 37};
	std::array<render::View, 2> views;
	for (size_t index = 0; index < views.size(); index++) {
		auto &view = views[index];
		view.Slot = 0;
		view.World = 1600 + index;
		view.WorldName = core::Name(index ? "radiance.with-portal" : "radiance.control");
		view.Pipeline = pipelineName;
		view.Target = &target;
		view.Instances = std::span(rows).first(destinationSource ? 3 : 2);
		if (destinationSource == 2) view.Lights = std::span(&destinationLight, 1);
		view.CameraFrame = CFrame::LookAt({0, 0, 1}, {0, 0, 3});
		view.Camera.NearPlane = .1f;
		view.Camera.FarPlane = 32;
		view.OverrideLighting = true;
		view.Lighting.Ambient = {.25f, .25f, .25f};
		view.Lighting.Direct = destinationSource == 3 ? core::Color3{.3f, 0, 0} : core::Color3{};
		if (destinationSource == 3) view.Lighting.Direction = {0, 0, 1};
	}
	views[1].Portals = std::span(&portal, 1);
	std::vector<float> directReference;
	render::OverlayImage overlay;
	for (size_t index = 0; index < views.size(); index++) {
		REQUIRE(renderer.RequestResourceImage({index + 1, pipelineName, captureName, 0}));
		renderer.Render(std::span(&views[index], 1), overlay, nullptr, false);
		std::optional<render::ResourceImage> captured;
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while (!captured && std::chrono::steady_clock::now() < deadline) {
			captured = renderer.TakeResourceImage(index + 1);
			if (!captured) SDL_Delay(1);
		}
		REQUIRE(captured.has_value());
		REQUIRE(captured->Status == render::ResourceImageStatus::Ok);
		REQUIRE(captured->Width == target.Width);
		REQUIRE(captured->Height == target.Height);
		std::vector<float> expected(target.Width * target.Height * 4);
		std::vector<float> observed(expected.size());
		// Quarter radiance is exactly representable in binary16. Inspect a
		// central 5x5 patch, away from screen-space sampling boundaries.
		for (uint32_t y = 16; y <= 20; y++)
			for (uint32_t x = 30; x <= 34; x++)
				for (size_t channel = 0; channel < 4; channel++) {
					const size_t at = (y * target.Width + x) * 4 + channel;
					const size_t byte = y * captured->RowStride + x * 8 + channel * 2;
					const uint16_t half = std::to_integer<uint16_t>(captured->Pixels[byte]) |
										  (std::to_integer<uint16_t>(captured->Pixels[byte + 1]) << 8);
					REQUIRE((half & 0x8000) == 0);
					const int exponent = (half >> 10) & 31;
					REQUIRE(exponent < 31);
					observed[at] = exponent == 0 ? std::ldexp(static_cast<float>(half & 1023), -24)
												 : std::ldexp(1.0f + (half & 1023) / 1024.0f, exponent - 15);
					expected[at] = channel == 3 ? 1.0f : .25f;
				}
		std::cout << "portal ambient control=" << (index == 0) << " source=" << destinationSource
				  << " ambient=.25 actual=" << observed[(18 * target.Width + 32) * 4] << '\n';
		if (destinationSource == 3) {
			// Both rooms already receive this same global sun. Its ordinary
			// BRDF varies across the patch, so the independent portal-free
			// rendering supplies the exact geometric reference for that term.
			CHECK(observed[(18 * target.Width + 32) * 4] > .5f);
			if (index == 0) directReference = observed;
			expected = directReference;
		}
		if (destinationSource == 1 && index) {
			// At the optical centre the receiver normal points at the aperture.
			// Its front face is z=2.99 and the authored aperture gives reach=16.
			const double falloff = 1 - std::pow(2.99 / 16, 2);
			const double radiance = .25 + EMISSION * falloff * falloff;
			CHECK(std::abs(observed[(18 * target.Width + 32) * 4] - radiance) < 2.0 / 4096);
		}
		if ((destinationSource == 1 || destinationSource == 2) && index) {
			// The remote red source is outside the receiver's direct light range.
			// Its red contribution must arrive through the probe while the other
			// channels retain the independently known ambient radiance.
			for (uint32_t y = 16; y <= 20; y++)
				for (uint32_t x = 30; x <= 34; x++) {
					const size_t at = (y * target.Width + x) * 4;
					CHECK(observed[at] > .3f);
					// Red transport has its own inequality above; only the
					// analytically constant channels enter this image comparison.
					expected[at] = observed[at] = 0;
				}
		}
		ImageTolerance tolerance;
		tolerance.Absolute = 1.0 / 4096;
		CheckImage(
			renderer,
			std::string(index ? "portal-radiance-source-" : "portal-radiance-control-") +
				std::to_string(destinationSource),
			"lens-b",
			"white albedo=1 ambient=.25 direct=0; destination source=" + std::to_string(destinationSource) +
				"\n" + graph::Write(document),
			{target.Width, target.Height, ImageFormat::Rgba32Float, std::as_bytes(std::span(expected))},
			{target.Width, target.Height, ImageFormat::Rgba32Float, std::as_bytes(std::span(observed))},
			tolerance
		);
	}
}

TEST_CASE(
	"first-person body exclusion preserves the character through a local portal",
	"[render][gpu][portal-character][eye-body][.]"
) {
	const bool blended = GENERATE(false, true);
	const bool imported = GENERATE(false, true);
	const std::array<uint32_t, 1> hiddenRows{0};
	FixtureDevice fixture;
	fixture.Initialise();
	(void)Install(fixture.Render);
	std::array<scene::DrawInstance, 3> rows{};
	for (size_t index = 0; index < rows.size(); ++index) {
		rows[index].Source = index + 1;
		rows[index].Mesh = CUBE;
		rows[index].Texture = WHITE;
		rows[index].CastShadow = false;
	}
	rows[0].Rig = imported ? 0 : 7;
	if (imported) rows[0].SourceWorld = core::Name("foreign.body");
	rows[0].Frame.Position = {0, 0, -6};
	rows[0].HalfExtent = {2, 2, .1f};
	rows[0].Tint = {0, 0, 1};
	rows[0].Transparency = blended ? .5f : 0.f;
	rows[1].Frame.Position = {3, 0, -4};
	rows[1].HalfExtent = {.75f, 1.5f, .01f};
	rows[1].Surface = 0;
	rows[1].Tint = {1, 1, 1};
	rows[2].Frame.Position = {0, 0, -10};
	rows[2].HalfExtent = {20, 20, .1f};
	rows[2].Tint = {.2f, 0, 0};
	render::PortalView portal;
	portal.Centre = {3, 0, -4};
	portal.Normal = {0, 0, 1};
	portal.First = {.75f, 0, 0};
	portal.Second = {0, 1.5f, 0};
	portal.Warp.Frame.Position = {-3, 0, 0};
	render::SceneTarget target{WIDTH, HEIGHT};
	render::View view;
	view.World = 1900;
	view.WorldName = core::Name("eye-body.portal");
	view.Pipeline = PIPELINE;
	view.Target = &target;
	view.Instances = rows;
	view.Portals = std::span(&portal, 1);
	view.OverrideLighting = true;
	view.Lighting.Ambient = {.5f, .5f, .5f};
	view.Lighting.Direct = {};
	const auto format = fixture.Render.Backend().ColourFormat;
	const bool bgra =
		format == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM || format == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM_SRGB;
	std::array<int, 3> portalBefore{};
	for (const bool hide : {false, true}) {
		CAPTURE(blended, hide);
		view.EyeRig = hide && !imported ? 7 : 0;
		view.EyeHiddenRows = hide && imported ? std::span(hiddenRows) : std::span<const uint32_t>{};
		render::OverlayImage overlay;
		fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false);
		const auto capture = CaptureResource(
			fixture.Render, core::Name("composed-image"), 0, WIDTH, HEIGHT, ImageFormat::Rgba8Unorm
		);
		const auto rgb = [&](uint32_t x) {
			const auto *pixel = capture.Bytes.data() + 48 * capture.RowStrideBytes + x * 4;
			return std::array{
				std::to_integer<int>(pixel[bgra ? 2 : 0]),
				std::to_integer<int>(pixel[1]),
				std::to_integer<int>(pixel[bgra ? 0 : 2])
			};
		};
		const auto centre = rgb(64), through = rgb(116);
		CAPTURE(centre, through);
		CHECK((hide ? centre[0] > centre[2] : centre[2] > centre[0]));
		CHECK(through[2] > through[0]);
		if (hide)
			CHECK(through == portalBefore);
		else
			portalBefore = through;
	}
}

TEST_CASE(
	"first-person body exclusion retains its shadow on the world",
	"[render][gpu][portal-character][eye-body][.]"
) {
	const bool imported = GENERATE(false, true);
	const std::array<uint32_t, 1> hiddenRows{0};
	FixtureDevice fixture;
	fixture.Initialise();
	(void)Install(fixture.Render);
	std::array<scene::DrawInstance, 3> rows{};
	for (size_t index = 0; index < rows.size(); ++index) {
		rows[index].Source = index + 1;
		rows[index].Mesh = CUBE;
		rows[index].Texture = WHITE;
	}
	rows[0].Rig = imported ? 0 : 7;
	if (imported) rows[0].SourceWorld = core::Name("foreign.body");
	rows[0].Frame.Position = {0, 1, -6};
	rows[0].HalfExtent = {.75f, 1, .5f};
	rows[0].Tint = {0, 0, 1};
	// A second caster keeps the negative control from reading a skipped pass's stale map.
	rows[1].Frame.Position = {0, -.1f, -6};
	rows[1].HalfExtent = {8, .1f, 8};
	rows[1].Tint = {1, 1, 1};
	rows[2].Frame.Position = {0, 1, -12};
	rows[2].HalfExtent = {8, 4, .1f};
	rows[2].Tint = {1, 0, 0};
	rows[2].CastShadow = false;
	render::SceneTarget target{WIDTH, HEIGHT};
	render::View view;
	view.World = 1901;
	view.WorldName = core::Name("eye-body.shadow");
	view.Pipeline = PIPELINE;
	view.Target = &target;
	view.Instances = rows;
	view.CameraFrame = CFrame::LookAt({0, 3, 3}, {0, 1, -6});
	view.OverrideLighting = true;
	view.Lighting.Direction = Vector3{.4f, -1, .2f}.Unit();
	view.Lighting.Ambient = {.1f, .1f, .1f};
	view.Lighting.OutdoorAmbient = view.Lighting.Ambient;
	view.Lighting.Direct = {.8f, .8f, .8f};
	std::vector<std::byte> initialDepth, previousColour;
	for (int phase = 0; phase < 3; ++phase) {
		CAPTURE(phase);
		view.EyeRig = phase != 0 && !imported ? 7 : 0;
		view.EyeHiddenRows = phase != 0 && imported ? std::span(hiddenRows) : std::span<const uint32_t>{};
		rows[0].CastShadow = phase != 2;
		render::OverlayImage overlay;
		fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false);
		const auto depth = CaptureResource(
			fixture.Render,
			core::Name("shadow"),
			0,
			render::SHADOW_RESOLUTION,
			render::SHADOW_RESOLUTION,
			ImageFormat::R32Float
		);
		size_t written = 0;
		for (size_t index = 0; index < depth.Bytes.size(); index += sizeof(float)) {
			float value = 0;
			std::memcpy(&value, depth.Bytes.data() + index, sizeof(value));
			written += std::isfinite(value) && value >= 0 && value < 1;
		}
		REQUIRE(written > 1000);
		if (phase == 0)
			initialDepth = depth.Bytes;
		else {
			const bool same = depth.Bytes == initialDepth;
			CHECK(same == (phase == 1));
		}
		const auto colour = CaptureResource(
			fixture.Render, core::Name("composed-image"), 0, WIDTH, HEIGHT, ImageFormat::Bgra8Unorm
		);
		const std::array phaseNames{"visible", "hidden", "unshadowed"};
		WriteImagePreview(
			core::Paths::Base() / (std::string("eye-body-shadow-") + phaseNames[phase] + ".ppm"),
			colour.View()
		);
		if (phase > 0) {
			size_t changed = 0;
			for (uint32_t y = 0; y < HEIGHT; ++y)
				for (uint32_t x = 0; x < WIDTH; ++x) {
					const size_t at = y * colour.RowStrideBytes + x * 4;
					changed += std::memcmp(colour.Bytes.data() + at, previousColour.data() + at, 3) != 0;
				}
			CAPTURE(changed);
			CHECK(changed > 10);
		}
		previousColour = colour.Bytes;
	}
}
