#include "ImageGraphCameraAdapter.hpp"
#include "ImageGraphSdfAdapter.hpp"
#include "ImageGraphSkyboxCache.hpp"

#include <engine/ecs/Store.hpp>
#include <engine/render/ImageGraphTransform3D.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/render/SourceSkyboxGroup.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <client/ImageGraphRuntime.hpp>
#include <fstream>

TEST_SUITE_ID("client.imagegraphruntime.sourceskybox")
TEST_DEPENDS("engine.render.sourcecamera3d")
TEST_DEPENDS("engine.render.sourcesdf")

namespace {
	using namespace engine;
	using namespace engine::render::imagegraph;
	void Check(bool okay, const char *text) {
		INFO(text);
		REQUIRE(okay);
	}
	imagegraph::Document Camera() {
		imagegraph::Document document;
		document.FormatVersion = 9;
		document.Nodes = {
			{"cube", "pc.3_d_mesh_cube", "", {}, {}},
			{"scene", "pc.3_d_scene", "", {}, {}},
			{"camera",
			 "pc.3_d_camera",
			 "",
			 {},
			 {{"dimension", imagegraph::Vector2{64, 64}}, {"dimension_unit", imagegraph::EnumValue{0}}}}
		};
		document.Nodes[1].DynamicInputs = {{"cube", imagegraph::ValueType::Mesh, std::nullopt}};
		document.Links = {{"cube", "mesh", "scene", "cube"}, {"scene", "scene", "camera", "scene"}};
		document.Outputs = {{"out", "camera", "rendered"}};
		return document;
	}
	imagegraph::Document Sdf() {
		imagegraph::Document document;
		document.FormatVersion = 9;
		document.Nodes = {
			{"sphere", "pc.rm_primitive", "", {}, {{"shape", imagegraph::EnumValue{1}}, {"radius", .7}}},
			{"render",
			 "pc.rm_render",
			 "",
			 {},
			 {{"dimension", imagegraph::Vector2{64, 64}},
			  {"dimension_unit", imagegraph::EnumValue{0}},
			  {"attribute_texture_size", 1024.0}}}
		};
		document.Links = {{"sphere", "sdf_object", "render", "sdf_object"}};
		document.Outputs = {{"out", "render", "surface_out"}};
		return document;
	}
	void RuntimeCaller() {
		scene::RegisterSceneComponents();
		scene::RegisterSceneClasses();
		ecs::Store store("source-skybox-real-caller");
		scene::InstallServices(store);
		const auto sky = store.CreateInstance(ecs::Classes::Find(core::Name("SkyboxTextures")), "GraphSky");
		Check(sky != ecs::NULL_ENTITY, "real skybox instance exists");
		Check(store.SetParent(sky, store.FindFirstRoot("Lighting")), "real skybox enters Lighting");
		const std::array<core::Name, 6> names{
			core::Name("front"),
			core::Name("back"),
			core::Name("left"),
			core::Name("right"),
			core::Name("up"),
			core::Name("down")
		};
		auto *textures = store.GetMutable<scene::SkyboxTextures>(sky);
		Check(textures != nullptr, "real skybox texture component exists");
		textures->Front = names[0];
		textures->Back = names[1];
		textures->Left = names[2];
		textures->Right = names[3];
		textures->Up = names[4];
		textures->Down = names[5];
		const core::Name graph("source-skybox-real-caller"), owner("source-skybox-caller-owner");
		const core::Name staging(
			std::string("engine.imagegraph.skybox.staging.") + std::to_string(owner.Id())
		);
		const auto assets = std::filesystem::temp_directory_path() / "source-skybox-outside53-caller";
		std::filesystem::create_directories(assets / "imagegraphs");
		struct Cleanup {
			std::filesystem::path Path;
			~Cleanup() {
				std::filesystem::remove_all(Path);
			}
		} cleanup{assets};
		auto document = Camera();
		document.Outputs.push_back({"depth", "camera", "depth"});
		const auto path = client::ImageGraphDocumentPath(assets, graph);
		{
			std::ofstream file(path);
			file << imagegraph::Write(document);
		}
		std::array<ecs::Entity, 6> entities;
		std::array<scene::ImageGraphBinding, 6> selectors;
		for (size_t i = 0; i < 6; ++i) {
			entities[i] = store.Create();
			selectors[i].Graph = graph;
			selectors[i].Output = core::Name("out");
			selectors[i].Texture = names[i];
			Check(
				scene::SetImageGraphBinding(store, entities[i], selectors[i]),
				"real scene accepts authored face binding"
			);
		}
		render::Renderer renderer;
		client::ImageGraphRuntime runtime;
		Check(
			runtime.Refresh(store, renderer, owner, assets) == 0,
			"real caller does not publish pending camera faces"
		);
		Check(runtime.LastError().empty(), "real caller accepts complete source cohort");
		Check(runtime.DocumentParses() == 1, "six faces share one parsed source document");
		for (size_t i = 0; i < 4; ++i)
			Check(
				renderer.SourceOutputStatus(staging, names[i], i + 1) == render::SourceTextureStatus::Pending,
				"real caller queued private generation"
			);
		Check(
			renderer.SourceOutputStatus(staging, names[4], 5) == render::SourceTextureStatus::Absent,
			"fifth face obeys actual queue backpressure"
		);
		Check(
			renderer.SourceOutputStatus(owner, names[0], 1) == render::SourceTextureStatus::Absent,
			"private face cannot become visible early"
		);
		const std::array active{owner};
		runtime.RetireInactiveOwners(renderer, active);
		runtime.BeginFrame();
		Check(
			runtime.Refresh(store, renderer, owner, assets) == 0,
			"unchanged pending cohort survives frame and owner retirement sweep"
		);
		Check(runtime.DocumentParses() == 1, "pending refresh does not reparse unchanged source");
		selectors[2].Seed = 23;
		Check(scene::SetImageGraphBinding(store, entities[2], selectors[2]), "authored seed update accepted");
		runtime.BeginFrame();
		Check(
			runtime.Refresh(store, renderer, owner, assets) == 0,
			"seed mutation admits replacement cohort without publication"
		);
		for (size_t i = 0; i < 4; ++i) {
			Check(
				renderer.SourceOutputStatus(staging, names[i], i + 1) == render::SourceTextureStatus::Absent,
				"old cohort cancelled on seed mutation"
			);
			Check(
				renderer.SourceOutputStatus(staging, names[i], i + 7) == render::SourceTextureStatus::Pending,
				"replacement uses shared native generation counter"
			);
		}
		selectors[5].Output = core::Name("depth");
		Check(
			scene::SetImageGraphBinding(store, entities[5], selectors[5]), "numeric output selector authored"
		);
		runtime.BeginFrame();
		Check(
			runtime.Refresh(store, renderer, owner, assets) == 0,
			"numeric source cohort refused without publication"
		);
		Check(
			runtime.LastError().find("display") != std::string::npos,
			"numeric cohort gets concrete display admission diagnostic"
		);
		for (size_t i = 0; i < 4; ++i)
			Check(
				renderer.SourceOutputStatus(staging, names[i], i + 7) == render::SourceTextureStatus::Absent,
				"invalid replacement cancels all prior private requests"
			);

		selectors[5].Output = core::Name("out");
		Check(scene::SetImageGraphBinding(store, entities[5], selectors[5]), "display output restored");
		runtime.BeginFrame();
		Check(runtime.Refresh(store, renderer, owner, assets) == 0, "restored cohort stays private");
		auto pendingGeneration = [&](size_t index) {
			uint64_t result = 0;
			for (uint64_t generation = 1; generation < 100; ++generation)
				if (renderer.SourceOutputStatus(staging, names[index], generation) ==
					render::SourceTextureStatus::Pending) {
					Check(result == 0, "one pending generation per authored face");
					result = generation;
				}
			return result;
		};
		std::array<uint64_t, 4> restored;
		for (size_t i = 0; i < 4; ++i) {
			restored[i] = pendingGeneration(i);
			Check(restored[i] != 0, "restored source queue exists");
		}
		{
			std::ofstream file(path, std::ios::app);
			file << "\n";
		}
		runtime.BeginFrame();
		Check(
			runtime.Refresh(store, renderer, owner, assets) == 0,
			"source-byte mutation replaces private cohort"
		);
		Check(runtime.DocumentParses() == 2, "source mutation reparses document once");
		for (size_t i = 0; i < 4; ++i) {
			Check(
				renderer.SourceOutputStatus(staging, names[i], restored[i]) ==
					render::SourceTextureStatus::Absent,
				"file mutation cancels prior source generation"
			);
			restored[i] = pendingGeneration(i);
			Check(restored[i] != 0, "file mutation captures new generation");
		}
		store.Remove<scene::ImageGraphBinding>(entities[5]);
		runtime.BeginFrame();
		Check(runtime.Refresh(store, renderer, owner, assets) == 0, "incomplete group cannot publish");
		for (size_t i = 0; i < 4; ++i)
			Check(
				renderer.SourceOutputStatus(staging, names[i], restored[i]) ==
					render::SourceTextureStatus::Absent,
				"incomplete group cancels private generation"
			);
		Check(scene::SetImageGraphBinding(store, entities[5], selectors[5]), "sixth binding restored");
		runtime.BeginFrame();
		Check(runtime.Refresh(store, renderer, owner, assets) == 0, "restored six-face group queues again");
		for (size_t i = 0; i < 4; ++i) {
			restored[i] = pendingGeneration(i);
			Check(restored[i] != 0, "cohort pending before owner retirement");
		}
		runtime.RetireInactiveOwners(renderer, {});
		for (size_t i = 0; i < 4; ++i)
			Check(
				renderer.SourceOutputStatus(staging, names[i], restored[i]) ==
					render::SourceTextureStatus::Absent,
				"inactive owner retirement cancels private generation"
			);
		runtime.Clear(renderer);
	}

}

TEST_CASE(
	"grouped camera and SDF skybox admission keeps incomplete results private",
	"[client][imagegraph][source_skybox]"
) {

	imagegraph::Diagnostic diagnostic;
	imagegraph::Plan plan;
	auto cameraDoc = Camera();
	Check(imagegraph::Compile(cameraDoc, plan, diagnostic) == imagegraph::Status::Ok, "camera compiles");
	SourceCamera3DRequest camera;
	Check(
		client::detail::BuildCameraRequest(
			cameraDoc, plan, cameraDoc.Nodes.back(), "rendered", 0, 5, true, camera, diagnostic
		),
		"real camera adapter builds display request"
	);
	SourceCamera3DRequest numericCamera;
	Check(
		client::detail::BuildCameraRequest(
			cameraDoc, plan, cameraDoc.Nodes.back(), "depth", 0, 5, true, numericCamera, diagnostic
		),
		"real numeric camera adapter preserves numeric format"
	);
	Check(numericCamera.Output == SourceCamera3DOutput::Depth, "depth selector remains explicitly numeric");
	auto sdfDoc = Sdf();
	Check(imagegraph::Compile(sdfDoc, plan, diagnostic) == imagegraph::Status::Ok, "SDF compiles");
	SourceSdfRequest sdf;
	Check(
		client::detail::BuildSdfRequest(
			sdfDoc, plan, sdfDoc.Nodes.back(), "surface_out", 0, 7, true, sdf, diagnostic
		),
		"real SDF adapter builds display request"
	);
	SourceSkyboxRequest request;
	request.Owner = core::Name("skybox.owner");
	request.StagingOwner = core::Name("skybox.staging");
	for (size_t i = 0; i < 6; ++i) {
		request.Targets[i] = {core::Name(std::string("skybox.face.") + std::to_string(i)), i + 1};
		if (i % 2)
			request.Faces[i] = sdf;
		else
			request.Faces[i] = camera;
	}
	std::string error;
	auto depth = request;
	depth.Faces[0] = numericCamera;
	Check(
		ValidateSourceSkybox(depth, error) == SourceSkyboxStatus::Invalid,
		"display skybox rejects numeric camera selector regardless of pixel format"
	);
	Check(
		ValidateSourceSkybox(request, error) == SourceSkyboxStatus::Pending,
		"six real source request faces admitted"
	);
	Check(error.empty(), "valid group diagnostic empty");
	std::array<scene::ImageGraphBinding, 6> selectors;
	std::array<ecs::Entity, 6> entities{};
	std::array<std::filesystem::file_time_type, 6> modified{};
	std::array<uintmax_t, 6> fileBytes{};
	for (size_t i = 0; i < 6; ++i) {
		selectors[i].Graph = core::Name("sky.graph");
		selectors[i].Output = core::Name("out");
		selectors[i].Texture = request.Targets[i].Name;
		selectors[i].TickPolicy = scene::ImageGraphTickPolicy::World;
	}
	client::detail::SourceSkyboxCaptureView captured{42, selectors, entities, modified, fileBytes};
	Check(
		client::detail::SameSourceSkyboxCapture(captured, captured),
		"captured authoring inputs remain valid as world time advances"
	);
	auto changedSelectors = selectors;
	changedSelectors[2].Seed = 99;
	Check(
		!client::detail::SameSourceSkyboxCapture(
			captured, {42, changedSelectors, entities, modified, fileBytes}
		),
		"seed mutation invalidates cohort"
	);
	changedSelectors = selectors;
	changedSelectors[4].Output = core::Name("other");
	Check(
		!client::detail::SameSourceSkyboxCapture(
			captured, {42, changedSelectors, entities, modified, fileBytes}
		),
		"output selector mutation invalidates cohort"
	);
	auto changedModified = modified;
	changedModified[1] += std::chrono::seconds(1);
	Check(
		!client::detail::SameSourceSkyboxCapture(
			captured, {42, selectors, entities, changedModified, fileBytes}
		),
		"source timestamp mutation invalidates cohort"
	);
	auto changedBytes = fileBytes;
	changedBytes[5] = 1;
	Check(
		!client::detail::SameSourceSkyboxCapture(captured, {42, selectors, entities, modified, changedBytes}),
		"source byte count mutation invalidates cohort"
	);
	Check(
		!client::detail::SameSourceSkyboxCapture(captured, {43, selectors, entities, modified, fileBytes}),
		"world incarnation invalidates cohort"
	);
	Check(
		!client::detail::SameSourceSkyboxCapture(
			captured, {42, std::span(selectors).first(5), entities, modified, fileBytes}
		),
		"incomplete binding group invalidates cohort"
	);
	// This uses real headless queue admission only. No rendering or mock completion is asserted.
	render::Renderer renderer;
	for (size_t i = 0; i < 4; ++i)
		Check(
			renderer.QueueSourceSdf(
				{request.StagingOwner, request.Targets[i].Name, request.Targets[i].Generation, sdf}
			) == TransformImage3DQueueResult::Queued,
			"real queue admits one of four slots without GPU execution"
		);
	Check(
		renderer.QueueSourceSdf(
			{request.StagingOwner, request.Targets[4].Name, request.Targets[4].Generation, sdf}
		) == TransformImage3DQueueResult::Full,
		"real four-slot queue supplies backpressure"
	);
	const auto originalStatus = renderer.SourceOutputStatus(
		request.StagingOwner, request.Targets[0].Name, request.Targets[0].Generation
	);
	Check(
		originalStatus == render::SourceTextureStatus::Pending,
		"source queue retains its exact admitted generation"
	);
	auto mismatchedTargets = request.Targets;
	++mismatchedTargets[0].Generation;
	Check(
		renderer.SourceOutputStatus(
			request.StagingOwner, mismatchedTargets[0].Name, mismatchedTargets[0].Generation
		) == render::SourceTextureStatus::Absent,
		"mismatched source generation cannot claim readiness"
	);
	Check(
		!renderer.PromoteTextureGroup(request.StagingOwner, request.Owner, mismatchedTargets),
		"mismatched source publication refused before texture mutation"
	);
	Check(
		renderer.SourceOutputStatus(
			request.StagingOwner, request.Targets[0].Name, request.Targets[0].Generation
		) == originalStatus,
		"refused promotion preserves actual pending generation"
	);
	renderer.DropTransformImage3DOwner(request.StagingOwner);
	Check(
		renderer.QueueSourceSdf(
			{request.StagingOwner, request.Targets[4].Name, request.Targets[4].Generation, sdf}
		) == TransformImage3DQueueResult::Queued,
		"actual cancellation frees unrecorded slots"
	);
	renderer.DropTransformImage3DOwner(request.StagingOwner);

	Check(
		!renderer.PromoteTextureGroup(request.StagingOwner, request.Owner, request.Targets),
		"absent source generations cannot be relabeled as published"
	);
	assets::TextureData oversized;
	oversized.Width = 8192;
	oversized.Height = 2049;
	oversized.Pixels.resize(uint64_t(oversized.Width) * oversized.Height * 4);
	Check(oversized.IsValid(), "oversized native stage fixture is structurally valid");
	Check(
		!renderer.StageSourceTexture(request.StagingOwner, request.Targets[0], oversized),
		"native source stage refuses pixel payload over its byte cap"
	);
	Check(
		renderer.SourceOutputStatus(
			request.StagingOwner, request.Targets[0].Name, request.Targets[0].Generation
		) == render::SourceTextureStatus::Absent,
		"refused CPU stage publishes no generation"
	);
	SourceSkyboxRequest mixed = request;
	assets::TextureData cpu;
	cpu.Width = cpu.Height = 1;
	cpu.Pixels.assign(4, std::byte{0});
	mixed.Faces[5] = cpu;
	Check(
		ValidateSourceSkybox(mixed, error) == SourceSkyboxStatus::Pending,
		"mixed CPU and real source faces pass group admission"
	);
	SourceSkyboxGroup mixedGroup;
	Check(
		BeginSourceSkybox(renderer, mixedGroup, std::move(mixed), error) == SourceSkyboxStatus::Pending,
		"mixed group captures real source requests"
	);
	Check(
		RefreshSourceSkybox(renderer, mixedGroup, error) == SourceSkyboxStatus::Failed,
		"headless CPU upload refusal fails whole mixed cohort"
	);
	for (size_t i = 0; i < 4; ++i)
		Check(
			renderer.SourceOutputStatus(
				request.StagingOwner, request.Targets[i].Name, request.Targets[i].Generation
			) == render::SourceTextureStatus::Absent,
			"failed mixed upload cancels earlier native queue admissions"
		);
	CancelSourceSkybox(renderer, mixedGroup);
}

TEST_CASE(
	"source skybox runtime captures authored bindings and cancels changed cohorts",
	"[client][imagegraph][source_skybox]"
) {
	RuntimeCaller();
}

TEST_CASE(
	"skybox partial preparation stays private when a late Transform host refuses", "[client][source-skybox]"
) {
	using namespace engine;
	scene::RegisterSceneComponents();
	scene::RegisterSceneClasses();
	ecs::Store store("transform-skybox-admission");
	scene::InstallServices(store);
	const auto sky = store.CreateInstance(ecs::Classes::Find(core::Name("SkyboxTextures")), "GraphSky");
	REQUIRE(store.SetParent(sky, store.FindFirstRoot("Lighting")));
	const std::array<core::Name, 6> names{
		core::Name("front"),
		core::Name("back"),
		core::Name("left"),
		core::Name("right"),
		core::Name("up"),
		core::Name("down")
	};
	auto *textures = store.GetMutable<scene::SkyboxTextures>(sky);
	REQUIRE(textures);
	textures->Front = names[0];
	textures->Back = names[1];
	textures->Left = names[2];
	textures->Right = names[3];
	textures->Up = names[4];
	textures->Down = names[5];
	const core::Name graph("transform-skybox"), owner("transform-skybox-owner");
	const auto directory = std::filesystem::temp_directory_path() / "pc-transform-skybox66";
	std::filesystem::create_directories(directory / "imagegraphs");
	struct Cleanup {
		std::filesystem::path Path;
		~Cleanup() {
			std::filesystem::remove_all(Path);
		}
	} cleanup{directory};
	auto document = Camera();
	document.Nodes.push_back(
		{"image",
		 "image.solid",
		 "",
		 {},
		 {{"width", int64_t(64)}, {"height", int64_t(64)}, {"colour", imagegraph::Colour{255, 0, 0, 255}}}}
	);
	document.Nodes.push_back({"transform", "pc.3_d_transform_image", "", {}, {}});
	document.Links.push_back({"image", "image", "transform", "surface"});
	document.Outputs.push_back({"transform", "transform", "rendered"});
	{
		std::ofstream file(client::ImageGraphDocumentPath(directory, graph));
		file << imagegraph::Write(document);
	}
	std::array<ecs::Entity, 6> entities;
	for (size_t i = 0; i < 6; ++i) {
		entities[i] = store.Create();
		scene::ImageGraphBinding selector;
		selector.Graph = graph;
		selector.Output = core::Name(i == 5 ? "transform" : "out");
		selector.Texture = names[i];
		REQUIRE(scene::SetImageGraphBinding(store, entities[i], selector));
	}
	render::Renderer renderer;
	client::ImageGraphRuntime runtime;
	REQUIRE(runtime.Refresh(store, renderer, owner, directory) == 0);
	INFO(runtime.LastError());
	CHECK(runtime.LastError().find("renderer device") != std::string::npos);
	CHECK(runtime.DocumentParses() == 1);
	const core::Name staging("engine.imagegraph.skybox.staging." + std::to_string(owner.Id()));
	for (const auto name : names) {
		CHECK(renderer.SourceOutputStatus(staging, name, 1) == render::SourceTextureStatus::Absent);
		CHECK(renderer.SourceOutputStatus(owner, name, 1) == render::SourceTextureStatus::Absent);
	}
	store.Remove<scene::ImageGraphBinding>(entities[5]);
	runtime.BeginFrame();
	REQUIRE(runtime.Refresh(store, renderer, owner, directory) == 0);
	CHECK(runtime.LastError().find("six distinct") != std::string::npos);
	runtime.Clear(renderer);
}

TEST_CASE(
	"failed animated skybox preparation retries on its own advancing world clock", "[client][source-skybox]"
) {
	using namespace engine;
	scene::RegisterSceneComponents();
	scene::RegisterSceneClasses();
	ecs::Store store("animated-transform-skybox-admission");
	scene::InstallServices(store);
	const auto sky = store.CreateInstance(ecs::Classes::Find(core::Name("SkyboxTextures")), "GraphSky");
	REQUIRE(store.SetParent(sky, store.FindFirstRoot("Lighting")));
	const std::array<core::Name, 6> names{
		core::Name("front"),
		core::Name("back"),
		core::Name("left"),
		core::Name("right"),
		core::Name("up"),
		core::Name("down")
	};
	auto *textures = store.GetMutable<scene::SkyboxTextures>(sky);
	REQUIRE(textures);
	textures->Front = names[0];
	textures->Back = names[1];
	textures->Left = names[2];
	textures->Right = names[3];
	textures->Up = names[4];
	textures->Down = names[5];
	const core::Name graph("transform-skybox"), owner("animated-transform-skybox-owner");
	const auto directory = std::filesystem::temp_directory_path() / "pc-animated-transform-skybox66";
	std::filesystem::create_directories(directory / "imagegraphs");
	struct Cleanup {
		std::filesystem::path Path;
		~Cleanup() {
			std::filesystem::remove_all(Path);
		}
	} cleanup{directory};
	auto document = Camera();
	imagegraph::Document transformDocument;
	transformDocument.FormatVersion = 9;
	const core::Name transformGraph("animated-transform-face");
	transformDocument.Nodes.push_back(
		{"image",
		 "image.solid",
		 "",
		 {},
		 {{"width", int64_t(64)}, {"height", int64_t(64)}, {"colour", imagegraph::Colour{255, 0, 0, 255}}}}
	);
	transformDocument.Nodes.push_back({"transform", "pc.3_d_transform_image", "", {}, {}});
	transformDocument.Links.push_back({"image", "image", "transform", "surface"});
	transformDocument.Outputs.push_back({"transform", "transform", "rendered"});
	{
		std::ofstream file(client::ImageGraphDocumentPath(directory, graph));
		file << imagegraph::Write(document);
	}
	transformDocument.Keyframes = {
		{"image", "width", 0, int64_t{0}, "step"}, {"image", "width", 1, int64_t{64}, "step"}
	};
	{
		std::ofstream file(client::ImageGraphDocumentPath(directory, transformGraph));
		file << imagegraph::Write(transformDocument);
	}
	std::array<ecs::Entity, 6> entities;
	for (size_t i = 0; i < 6; ++i) {
		entities[i] = store.Create();
		scene::ImageGraphBinding selector;
		selector.Graph = i == 5 ? transformGraph : graph;
		selector.Output = core::Name(i == 5 ? "transform" : "out");
		selector.Texture = names[i];
		selector.TickPolicy = scene::ImageGraphTickPolicy::World;
		REQUIRE(scene::SetImageGraphBinding(store, entities[i], selector));
	}
	render::Renderer renderer;
	client::ImageGraphRuntime runtime;
	REQUIRE(runtime.Refresh(store, renderer, owner, directory) == 0);
	INFO(runtime.LastError());
	CHECK(runtime.LastError().find("solid dimensions") != std::string::npos);
	CHECK(runtime.DocumentParses() == 2);
	store.AdvanceTick(1.f / 60.f);
	runtime.BeginFrame();
	REQUIRE(runtime.Refresh(store, renderer, owner, directory) == 0);
	INFO(runtime.LastError());
	CHECK(runtime.LastError().find("renderer device") != std::string::npos);
	CHECK(runtime.DocumentParses() == 2);
	// Retiring a partial owner erases its failed admission and every entry before
	// the owner is later presented again.
	runtime.RetireInactiveOwners(renderer, {});
	runtime.BeginFrame();
	REQUIRE(runtime.Refresh(store, renderer, owner, directory) == 0);
	CHECK(runtime.LastError().find("renderer device") != std::string::npos);
	const core::Name staging("engine.imagegraph.skybox.staging." + std::to_string(owner.Id()));
	for (const auto name : names) {
		CHECK(renderer.SourceOutputStatus(staging, name, 1) == render::SourceTextureStatus::Absent);
		CHECK(renderer.SourceOutputStatus(owner, name, 1) == render::SourceTextureStatus::Absent);
	}
	store.Remove<scene::ImageGraphBinding>(entities[5]);
	runtime.BeginFrame();
	REQUIRE(runtime.Refresh(store, renderer, owner, directory) == 0);
	CHECK(runtime.LastError().find("six distinct") != std::string::npos);
	runtime.Clear(renderer);
}
