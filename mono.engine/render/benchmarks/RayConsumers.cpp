// Existing renderer consumers, not a geometry ray tracer. Readbacks and
// semantic oracles run before the measured, explicitly serialized frames.

#include "RayConsumerProfile.hpp"

#include <engine/assets/Mesh.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/graph/PipelineCatalogue.hpp>
#include <engine/graph/PipelineDocument.hpp>
#include <engine/render/ShaderCompiler.hpp>
#include <engine/render/ShaderLibrary.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/ShaderLens.hpp>
#include <engine/testing/Bench.hpp>

#include <glm/ext/matrix_clip_space.hpp>

#include <functional>
#include <initializer_list>
#include <iostream>

TEST_SUITE_ID("engine.render.bench.ray-consumers")
TEST_DEPENDS("engine.render.hardrendergpu")
TEST_DEPENDS("engine.render.volumegpu")
TEST_DEPENDS("engine.render.portalfixtures")
TEST_DEPENDS("engine.render.weather-fallback")

namespace {
	using namespace ray_consumer_bench;
	constexpr std::array<const char *, 8> FAMILIES{
		"trace", "volume", "environment", "fallback", "god-rays", "lens", "capture", "mirror"
	};
	constexpr std::array<const char *, 5> SCENES{"inactive", "empty", "simple", "busy", "complex"};
	constexpr std::array<const char *, 12> PASSES{
		"bench-gi",
		"bench-ray",
		"bench-path",
		"fog",
		"clouds-compute",
		"sky",
		"god-rays",
		"shader-lenses",
		"surface-capture",
		"weather-integrate",
		"weather-reconstruct",
		"weather-shadow-map"
	};
	const core::Name PIPELINE("benchmark.ray.consumers");
	const core::Name MESH("benchmark.ray.plane");
	struct TraceQuality {
		const char *GiRaysPerPixel;
		const char *ReflectionSteps;
		const char *PathSamplesPerFrame;
		const char *PathBounces;
		uint32_t HistorySamples;
	};
	TraceQuality TraceQualityFor(std::string_view scene) {
		if (scene == "complex") return {"64", "128", "64", "8", 63};
		if (scene == "busy") return {"16", "64", "8", "4", 8};
		return {"1", "32", "1", "1", 1};
	}

	assets::MeshData PlaneMesh() {
		assets::MeshData mesh;
		mesh.Vertices = {
			{{-1, -1, 0}, {0, 0, 1}, {0, 1}},
			{{1, -1, 0}, {0, 0, 1}, {1, 1}},
			{{1, 1, 0}, {0, 0, 1}, {1, 0}},
			{{-1, 1, 0}, {0, 0, 1}, {0, 0}},
			{{-1, -1, 0}, {0, 0, -1}, {0, 1}},
			{{1, -1, 0}, {0, 0, -1}, {1, 1}},
			{{1, 1, 0}, {0, 0, -1}, {1, 0}},
			{{-1, 1, 0}, {0, 0, -1}, {0, 0}}
		};
		mesh.Indices = {0, 1, 2, 0, 2, 3, 4, 6, 5, 4, 7, 6};
		mesh.ComputeBounds();
		return mesh;
	}
	scene::DrawInstance
	Plane(uint64_t id, core::Vector3 at, core::Vector3 extent, core::Color3 colour = {.7f, .5f, .3f}) {
		scene::DrawInstance plane;
		plane.Source = id;
		plane.Mesh = MESH;
		plane.Frame.Position = at;
		plane.HalfExtent = extent;
		plane.Tint = colour;
		plane.CastShadow = false;
		return plane;
	}
	graph::ResourceId Resource(const graph::RenderGraph &graph, std::string_view name) {
		for (uint32_t id = 1; id <= graph.ResourceCount(); ++id) {
			const graph::ResourceId resource{id};
			const auto *desc = graph.FindResourceDesc(resource);
			if (desc && desc->Name == core::Name(name)) return resource;
		}
		throw std::runtime_error("ray benchmark graph resource missing: " + std::string(name));
	}
	void Pin(Device &device, graph::RenderGraph &graph, std::span<const char *const> resources) {
		const core::Name kind("ray-benchmark-boundary-" + std::to_string(resources.size()));
		if (!graph::NodeCatalogue::Find(kind)) {
			graph::NodeKindSpec spec;
			spec.Kind = kind;
			spec.Scope = graph::NodeScope::Frame;
			spec.Queue = graph::ExecutionQueue::Cpu;
			spec.Category = graph::NodeCategory::Output;
			for (size_t index = 0; index < resources.size(); ++index)
				spec.Inputs.push_back(
					{.Name = core::Name("image" + std::to_string(index)),
					 .Kind = graph::ResourceKind::Texture}
				);
			Require(
				graph::RegisterNodeKind(std::move(spec)), "could not register ray benchmark output boundary"
			);
		}
		Require(
			device.Renderer.InstallNodeHandler(kind, [](const graph::RunContext &) { return true; }),
			"could not install ray benchmark output boundary"
		);
		graph::Node sink;
		sink.Name = kind;
		sink.Kind = kind;
		sink.Scope = graph::NodeScope::Frame;
		for (size_t index = 0; index < resources.size(); ++index) {
			sink.Reads.push_back(Resource(graph, resources[index]));
			sink.ReadPorts.emplace_back("image" + std::to_string(index));
		}
		Require(graph.AddNode(std::move(sink)).IsValid(), "could not pin ray benchmark resources");
	}
	graph::ResourceId AddTrace(graph::RenderGraph &graph, std::string_view scene) {
		const auto quality = TraceQualityFor(scene);
		const auto image = [&](const char *name,
							   graph::ResourceLifetime lifetime = graph::ResourceLifetime::Transient) {
			return graph.AddResource(
				{.Name = core::Name(name),
				 .Kind = graph::ResourceKind::Storage,
				 .Format = graph::ResourceFormat::RGBA16F,
				 .Lifetime = lifetime}
			);
		};
		const auto gi = image("bench-indirect"), ray = image("bench-reflection"), path = image("bench-path"),
				   history = image("bench-path-history", graph::ResourceLifetime::History);
		const auto trace = [&](const char *name,
							   const char *kind,
							   std::initializer_list<graph::ResourceId> reads,
							   graph::ResourceId output) {
			graph::Node node;
			node.Name = core::Name(name);
			node.Kind = core::Name(kind);
			node.Scope = graph::NodeScope::View;
			node.Reads.assign(reads);
			node.Writes = {output};
			if (node.Kind == core::Name("global-illumination"))
				node.Parameters.push_back({core::Name("rays-per-pixel"), quality.GiRaysPerPixel});
			if (node.Kind == core::Name("raytrace")) {
				node.Parameters.push_back({core::Name("steps"), quality.ReflectionSteps});
				node.Parameters.push_back({core::Name("max-distance"), "24"});
				node.Parameters.push_back({core::Name("thickness"), "0.2"});
			}
			if (node.Kind == core::Name("pathtrace")) {
				node.Parameters.push_back({core::Name("samples-per-frame"), quality.PathSamplesPerFrame});
				node.Parameters.push_back({core::Name("max-bounces"), quality.PathBounces});
			}
			Require(graph.AddNode(std::move(node)).IsValid(), "trace graph node refused");
		};
		const auto albedo = Resource(graph, "albedo"), normal = Resource(graph, "normal"),
				   material = Resource(graph, "material"), depth = Resource(graph, "linear-depth");
		trace(
			"bench-gi",
			"global-illumination",
			{albedo, normal, material, depth, Resource(graph, "occlusion")},
			gi
		);
		trace("bench-ray", "raytrace", {Resource(graph, "lit"), depth, normal, material, gi}, ray);
		trace(
			"bench-path",
			"pathtrace",
			{Resource(graph, "view-camera"),
			 Resource(graph, "visible-entities"),
			 Resource(graph, "lod-instances"),
			 albedo,
			 normal,
			 material,
			 depth,
			 ray,
			 history},
			path
		);
		graph::Node copy;
		copy.Name = core::Name("bench-path-store");
		copy.Kind = core::Name("dispatch");
		copy.Scope = graph::NodeScope::View;
		copy.Reads = {path};
		copy.Writes = {history};
		copy.Parameters = {{core::Name("shader"), "attachment-copy.comp"}, {core::Name("uniforms"), "view"}};
		Require(graph.AddNode(std::move(copy)).IsValid(), "trace history copy refused");
		return path;
	}
	void
	InstallPipeline(Device &device, std::string_view family, std::string_view scene, bool oracle = true) {
		graph::PipelineDocument document;
		bool omit = false;
		const auto basis = graph::DefaultPbrDocument();
		for (auto edit : basis.Edits()) {
			if (family == "fallback") {
				if (edit.Kind == graph::EditKind::AddNode) {
					omit = false;
					for (const char *kind :
						 {"skybox-compute",
						  "clouds-compute",
						  "weather-residency",
						  "weather-field",
						  "weather-summary",
						  "weather-shadow-map",
						  "weather-integrate",
						  "weather-history",
						  "weather-reconstruct"})
						omit = omit || edit.NodeKind == core::Name(kind);
				}
				if (edit.Kind == graph::EditKind::AddResource) omit = false;
				if (omit) continue;
				if (edit.Kind == graph::EditKind::Reads) {
					if (edit.Target == core::Name("environment-clouds") ||
						edit.Target == core::Name("weather-shadow"))
						continue;
					if (edit.Target == core::Name("weather-lit")) edit.Target = core::Name("sky-lit");
				}
			}
			document.Record(std::move(edit));
		}
		graph::RenderGraph original;
		core::Name offender;
		Require(
			graph::Build(document, original, offender) == graph::PipelineDocumentStatus::Ok,
			std::string("ray benchmark graph build refused at ") + std::string(offender.Text())
		);
		// Final frame outputs follow every view consumer. Resource handles keep
		// their declaration order while trace nodes join the view block.
		graph::RenderGraph graph;
		for (uint32_t id = 1; id <= original.ResourceCount(); ++id)
			Require(
				graph.AddResource(*original.FindResourceDesc(graph::ResourceId{id})).IsValid(),
				"resource copy refused"
			);
		bool traceAdded = false;
		for (uint32_t id = 1; id <= original.Count(); ++id) {
			const auto *node = original.Find(graph::NodeId{id});
			if (node && node->Scope != graph::NodeScope::Frame) {
				graph::Node copy = *node;
				if (family == "trace" && scene != "inactive" && !traceAdded &&
					copy.Kind == core::Name("tonemap")) {
					const auto path = AddTrace(graph, scene);
					// Trace outputs join the singleton tonemap chain through its first image input.
					copy.Reads.front() = path;
					traceAdded = true;
				}
				Require(graph.AddNode(std::move(copy)).IsValid(), "view node copy refused");
			}
		}
		for (uint32_t id = 1; id <= original.Count(); ++id) {
			const auto *node = original.Find(graph::NodeId{id});
			if (node && node->Scope == graph::NodeScope::Frame)
				Require(graph.AddNode(*node).IsValid(), "frame node copy refused");
		}
		std::vector<const char *> pinned{
			"tonemapped", "volume-lit", "display", "lens-b", "dof", "god-rays", "linear-depth", "portaled"
		};
		if (family != "fallback") pinned.push_back("weather-lit");
		if (family == "environment") {
			pinned.push_back("environment-sky");
			pinned.push_back("environment-clouds");
		}
		if (family == "trace" && scene != "inactive")
			for (const char *name :
				 {"bench-indirect", "bench-reflection", "bench-path", "bench-path-history"})
				pinned.push_back(name);
		if (oracle) Pin(device, graph, pinned);
		Require(device.Renderer.SetPipeline(PIPELINE, graph), "ray benchmark pipeline admission failed");
	}
	struct Scene {
		render::SceneTarget Target;
		render::View View;
		render::OverlayImage Overlay;
		std::vector<scene::DrawInstance> Instances;
		std::vector<render::PortalView> Portals;
		std::vector<render::SurfaceView> Mirrors;
	};
	void Weather(scene::Environment &environment, bool empty) {
		environment.HasClouds = environment.HasWeatherCompute = environment.HasWeatherClock = true;
		environment.CloudLayer.Colour = {.8f, .1f, .05f};
		environment.CloudLayer.Cover = environment.CloudLayer.Density = 1;
		environment.Weather.CellSizeMetres = 2;
		environment.Weather.ExtentMetres = {256, 64, 256};
		environment.Weather.BaseHeightMetres = empty ? 1000 : 24;
		environment.Weather.ThicknessMetres = 16;
		environment.Weather.Coverage = 1;
		environment.Weather.VapourMixingRatio = .029f;
		environment.Weather.BaseTemperatureCelsius = -50;
		environment.Weather.FeatureCells = 8;
		environment.Weather.WindMetresPerSecond = {};
		environment.Weather.Seed = 17;
		environment.Weather.LodError = .0001f;
		environment.Weather.ShadowsEnabled = false;
		environment.Clock.Tick = environment.Clock.AnchorTick = environment.Clock.PhaseTick = 123;
		environment.Clock.WindQ20 = {};
		environment.Clock.Paused = true;
	}
	float ObliqueClipSkew(std::string_view family, std::string_view projection) {
		// Keep the lens's six-metre gradient visible at the bent samples.
		return projection == "oblique" ? (family == "lens" ? .04f : .08f) : 0.f;
	}
	void Projection(
		render::View &view,
		std::string_view family,
		std::string_view projection,
		uint32_t width,
		uint32_t height
	) {
		const float aspect = float(width) / height;
		if (projection == "orthographic")
			view.Projection = glm::orthoRH_ZO(
				-4.f * aspect, 4.f * aspect, -4.f, 4.f, view.Camera.NearPlane, view.Camera.FarPlane
			);
		else if (projection == "oblique") {
			auto matrix = scene::ResolveCamera(view.CameraFrame, view.Camera, aspect).Projection;
			for (uint32_t column = 0; column < 4; ++column)
				matrix[column][2] += ObliqueClipSkew(family, projection) * matrix[column][0];
			view.Projection = matrix;
		} else
			Require(projection == "perspective", "unknown ray benchmark projection");
	}
	void Configure(
		Device &device,
		Scene &scene,
		std::string_view family,
		std::string_view variant,
		std::string_view projection,
		uint32_t width,
		uint32_t height
	) {
		const bool inactive = variant == "inactive", empty = variant == "empty", busy = variant == "busy",
				   complex = variant == "complex";
		scene.Target = {width, height};
		auto &view = scene.View;
		view.Target = &scene.Target;
		view.Pipeline = PIPELINE;
		view.World = 19261;
		view.WorldName = core::Name("benchmark.ray.world");
		view.Camera.NearPlane = .1f;
		view.Camera.FarPlane = 64;
		view.Camera.MaxImageWidth = width;
		view.Camera.MaxImageHeight = height;
		view.OverrideLighting = true;
		view.Lighting.Ambient = {.6f, .6f, .6f};
		view.Lighting.OutdoorAmbient = {};
		view.Lighting.Direct = {};
		view.Lighting.FogStart = 1000;
		view.Lighting.FogEnd = 1001;
		view.Grid.Enabled = false;
		view.Lighting.Direction = {0, 0, 1};
		scene.Instances.push_back(Plane(1, {0, 0, -6}, {1.5f, 1.5f, .01f}));
		if (complex && family != "capture" && family != "mirror" && family != "lens") {
			for (uint32_t index = 0; index < 64; ++index)
				scene.Instances.push_back(Plane(
					index + 2,
					{float(index % 8) * .65f - 2.3f, float(index / 8) * .45f - 1.5f, -4 - float(index % 3)},
					{.2f, .2f, .01f},
					{float(index % 5) / 5, .4f, .8f}
				));
		}
		if (busy && family != "capture" && family != "mirror" && family != "lens") {
			// Leave the central ray clear so every projection retains the six-metre depth oracle.
			for (uint32_t index = 0; index < 16; ++index)
				scene.Instances.push_back(Plane(
					index + 2,
					{(float(index % 4) - 1.5f) * .9f, (float(index / 4) - 1.5f) * .7f, -4 - float(index % 3)},
					{.2f, .2f, .01f},
					{float(index % 5) / 5, .4f, .8f}
				));
		}
		if (family == "trace") {
			if (empty)
				scene.Instances.clear();
			else {
				const bool orthographic = projection == "orthographic";
				const core::Vector3 floorCentre =
					orthographic ? core::Vector3{0, -2, -4} : core::Vector3{0, -2, -5};
				const core::Vector3 floorExtent =
					orthographic ? core::Vector3{8, 4, .01f} : core::Vector3{8, 8, .01f};
				const float floorRotation = orthographic ? -1.3962634016f : -1.5707963268f;
				// The orthographic reflector reaches the six-metre wall while its centre stays below the
				// depth probe.
				auto floor = Plane(1000, floorCentre, floorExtent, {.6f, .6f, .6f});
				floor.Frame = core::CFrame::Angles(floorRotation, 0, 0);
				floor.Frame.Position = floorCentre;
				scene.Instances.push_back(floor);
			}
		}
		if (family == "volume" && !inactive) {
			view.Lighting.VolumeCount = complex ? 16 : busy ? 4 : 1;
			for (size_t index = 0; index < view.Lighting.VolumeCount; ++index) {
				auto &volume = view.Lighting.Volumes[index];
				volume.Enabled = true;
				volume.Frame.Position = {empty ? 10000.f : float(index) * .06f, 0, -3};
				volume.HalfExtent = {3, 3, 2};
				volume.Density = .25f;
				volume.Extinction = .5f;
				volume.NoiseStrength = complex ? .7f : busy ? .35f : 0;
				volume.Steps = complex ? 64 : busy ? 48 : 32;
				volume.ShadowSteps = complex ? 32 : busy ? 8 : 1;
				volume.Colour = {float(index % 3) / 3, .4f, .7f};
				volume.Seed = uint32_t(index + 1);
				volume.Shape =
					(busy || complex) && index % 2 ? scene::VolumeShape::Ellipsoid : scene::VolumeShape::Box;
			}
			if (!busy && !complex) {
				view.Lighting.Ambient = {};
				scene.Instances[0].EmissiveStrength = 1;
			} else {
				view.Lighting.Direct = {.8f, .7f, .6f};
				view.Lighting.Direction = {-.3f, -1, -.2f};
			}
		}
		if (family == "environment") {
			auto &environment = view.Lighting.EnvironmentState;
			environment.Skybox = scene::SkyboxSource::Compute;
			environment.SkyCompute.Enabled = true;
			environment.SkyCompute.SunSize = .15f;
			environment.HasClouds = environment.HasCloudCompute = !inactive;
			environment.CloudLayer.Enabled = !inactive;
			environment.CloudLayer.Density = empty ? 0 : .8f;
			environment.CloudLayer.Cover = .7f;
			environment.CloudVolume.Enabled = true;
			environment.CloudVolume.Steps = complex ? 64 : busy ? 32 : 12;
			environment.CloudVolume.Shader =
				complex ? scene::CloudComputeShader::Storm : scene::CloudComputeShader::Cumulus;
			environment.CloudTime = .75;
		}
		if (family == "fallback") {
			Weather(view.Lighting.EnvironmentState, false);
			view.Lighting.EnvironmentState.Weather.Enabled = !inactive;
			view.Lighting.EnvironmentState.Weather.Coverage = empty ? 0 : 1;
			view.Lighting.EnvironmentState.Weather.BaseHeightMetres = -4;
		}
		if (family == "god-rays") {
			view.Lighting.EnvironmentState.Skybox = scene::SkyboxSource::Compute;
			view.Lighting.EnvironmentState.SkyCompute.Enabled = true;
			view.Lighting.EnvironmentState.SkyCompute.SunSize = .18f;
			view.Lighting.GodRayIntensity = inactive ? 0 : 1.5f;
			view.Lighting.GodRayThreshold = empty ? 10000 : .15f;
			view.Lighting.GodRayRadius = complex ? 256 : busy ? 128 : 64;
		}
		if (family == "lens") {
			scene.Instances[0].HalfExtent = {16, 16, .01f};
			render::ShaderCompiler compiler;
			const auto gradient = compiler.Compile(
				"#version 450\nlayout(location=0)out vec4 colour;void "
				"main(){colour=vec4(0,0,gl_FragCoord.x/" +
					std::to_string(width) + ".0,1);}",
				render::ShaderStage::Fragment,
				"benchmark.lens.gradient"
			);
			Require(
				!gradient.Failed &&
					device.Renderer.AddShader(core::Name("benchmark.lens.gradient"), gradient.SpirV),
				"lens gradient shader refused"
			);
			scene.Instances[0].Shader = core::Name("benchmark.lens.gradient");
			scene::RegisterSceneClasses();
			ecs::Store shaders("benchmark.lens.shaders");
			const auto entity = shaders.CreateInstance(ecs::Classes::Find(core::Name("ShaderLens")), "Lens");
			shaders.GetMutable<scene::ShaderLens>(entity)->Shader = core::Name("gravitational-lens");
			render::ShaderLibrary library;
			Require(library.RefreshLenses(shaders) == 1, "built-in lens lookup refused");
			const auto *program = library.FindLens(core::Name("gravitational-lens"));
			Require(
				program && device.Renderer.AddLensShader(core::Name("gravitational-lens"), program->SpirV),
				"built-in lens install refused"
			);
			view.Lighting.ShaderLensCount = inactive ? 0 : complex ? 16 : busy ? 4 : 1;
			for (size_t index = 0; index < view.Lighting.ShaderLensCount; ++index) {
				auto &lens = view.Lighting.ShaderLenses[index];
				lens.Shader = core::Name("gravitational-lens");
				lens.EntityId = index + 1;
				lens.Frame.Position = {
					busy ? float(index) * .6f - .9f : float(index % 4) * .2f, 0, empty ? -20.f : -3.f
				};
				lens.Radius = 2;
				lens.InnerRadius = .4f;
				lens.Strength = 2;
				lens.Spin = .25f;
			}
			view.LensTimeSeconds = 0;
		}
		if (family == "capture") {
			scene.Instances.clear();
			Weather(view.Lighting.EnvironmentState, empty);
			view.Lighting.EnvironmentState.Weather.Enabled = !inactive;
			view.CameraFrame = core::CFrame::LookAt({0, 0, 4}, {});
			device.Renderer.SetPortalDepth(1);
			const size_t count = complex ? 4 : busy ? 2 : 1;
			const float halfWidth = complex ? .8f : busy ? 1.1f : 1.7f;
			for (size_t index = 0; index < count; ++index) {
				const float x = complex ? float(index) * 2 - 3 : busy ? float(index) * 3 - 1.5f : 0;
				auto pane = Plane(index + 1, {x, 0, 0}, {halfWidth, 1.4f, .01f});
				pane.Surface = int16_t(index);
				scene.Instances.push_back(pane);
				render::PortalView portal;
				portal.Index = int16_t(index);
				portal.Centre = {x, 0, 0};
				portal.Normal = {0, 0, 1};
				portal.First = {halfWidth, 0, 0};
				portal.Second = {0, 1.4f, 0};
				portal.Warp.Frame.Position = {100, 32, 32};
				scene.Portals.push_back(portal);
			}
			scene.Instances.push_back(Plane(100, {100, 32, 28}, {20, 20, .01f}, {.8f, .2f, .1f}));
		}
		if (family == "mirror") {
			scene.Instances.clear();
			Weather(view.Lighting.EnvironmentState, empty);
			view.Lighting.EnvironmentState.Weather.Enabled = !inactive;
			view.CameraFrame = core::CFrame::LookAt({0, 6, 8}, {});
			view.Camera.FarPlane = 512;
			device.Renderer.SetSurfaceBounces(1);
			// Keep orthographic pane centres and their nine-pixel probes inside the view.
			const bool busyOrthographic = busy && projection == "orthographic";
			const bool complexOrthographic = complex && projection == "orthographic";
			const float mirrorHalfWidth = busyOrthographic || complexOrthographic ? 2 : 3;
			const float mirrorHalfDepth = complexOrthographic ? 2 : 3;
			const size_t count = complex ? 4 : busy ? 2 : 1;
			for (size_t index = 0; index < count; ++index) {
				float centreX = 0, centreZ = 0;
				if (complexOrthographic) {
					centreX = float(index % 2) * 5 - 2.5f;
					centreZ = float(index / 2) * 5 - 2.5f;
				} else if (busyOrthographic)
					centreX = float(index) * 5 - 2.5f;
				else if (complex)
					centreX = float(index) * 7 - 10.5f;
				else if (busy)
					centreX = float(index) * 7 - 3.5f;
				const core::Vector3 centre{centreX, 0, centreZ};
				auto pane = Plane(index + 1, centre, {mirrorHalfWidth, mirrorHalfDepth, .01f}, {1, 1, 1});
				pane.Frame = core::CFrame::Angles(-1.5707963268f, 0, 0);
				pane.Frame.Position = centre;
				pane.Surface = int16_t(index);
				scene.Instances.push_back(pane);
				const scene::SurfacePane geometry{
					.Centre = centre,
					.Normal = {0, 1, 0},
					.First = {mirrorHalfWidth, 0, 0},
					.Second = {0, 0, -mirrorHalfDepth}
				};
				const auto reflected = scene::ReflectCamera(geometry, view.CameraFrame, {});
				Require(reflected.Renders, "mirror camera refused");
				render::SurfaceView surface;
				surface.Index = int16_t(index);
				surface.Frame = reflected.Frame;
				surface.Projection = scene::SurfaceProjection(reflected.Lens, reflected.Frame);
				surface.PaneCentre = centre;
				surface.PaneNormal = geometry.Normal;
				surface.PaneFirst = geometry.First;
				surface.PaneSecond = geometry.Second;
				surface.Width = width;
				surface.Height = height;
				scene.Mirrors.push_back(surface);
			}
		}
		view.Instances = scene.Instances;
		view.Portals = scene.Portals;
		view.Surfaces = scene.Mirrors;
		Projection(view, family, projection, width, height);
	}

	render::FrameResult Submit(Device &device, Scene &scene) {
		scene.View.Damage.Scene = true;
		const auto result = device.Renderer.Render(std::span(&scene.View, 1), scene.Overlay, nullptr, false);
		Require(result.Submitted, "oracle frame did not submit");
		device.Wait();
		return result;
	}
	void SetMirrorEye(Scene &scene, const core::CFrame &eye) {
		scene.View.CameraFrame = eye;
		for (auto &surface : scene.Mirrors) {
			const scene::SurfacePane pane{
				.Centre = surface.PaneCentre,
				.Normal = surface.PaneNormal,
				.First = surface.PaneFirst,
				.Second = surface.PaneSecond,
				.NearPlane = surface.PaneNear,
				.FarPlane = surface.PaneFar
			};
			const auto reflected = scene::ReflectCamera(pane, eye, {});
			Require(reflected.Renders, "moving mirror camera refused");
			surface.Frame = reflected.Frame;
			surface.Projection = scene::SurfaceProjection(reflected.Lens, reflected.Frame);
		}
	}
	core::CFrame MovingMirrorEye(const core::CFrame &base, uint32_t frame) {
		auto position = base.Position;
		position.X += .005f * float(1 + frame % 17);
		return core::CFrame::LookAt(position, core::Vector3::Zero);
	}
	Image Output(Device &device, Scene &scene, const char *name = "tonemapped", bool hdr = false) {
		return Read(
			device, core::Name(name), scene.View.Slot, scene.Target.Width, scene.Target.Height, hdr ? 8 : 4
		);
	}
	void VerifyMovingMirror(Device &device, Scene &scene, std::ostream &out) {
		const auto baseEye = scene.View.CameraFrame;
		const auto reference = Output(device, scene);

		const auto firstEye = MovingMirrorEye(baseEye, 0);
		SetMirrorEye(scene, firstEye);
		const auto firstFrame = Submit(device, scene);
		Require(firstFrame.SurfacePasses > 0, "moving mirror first pose did not capture a mirror");
		const auto first = Output(device, scene);
		Difference(first, first);

		const auto secondEye = MovingMirrorEye(baseEye, 1);
		SetMirrorEye(scene, secondEye);
		const auto secondFrame = Submit(device, scene);
		Require(secondFrame.SurfacePasses > 0, "moving mirror second pose did not capture a mirror");
		const auto second = Output(device, scene);
		Difference(second, second);

		SetMirrorEye(scene, baseEye);
		const auto returnFrame = Submit(device, scene);
		Require(returnFrame.SurfacePasses > 0, "moving mirror return pose did not capture a mirror");
		const auto returned = Output(device, scene);
		const double returnError = Difference(reference, returned);
		Require(returnError <= 2.0 / 255, "moving mirror return pose exceeded repeatability tolerance");
		out << "# ray-consumer-motion-oracle workload=moving_camera return_max_error=" << returnError
			<< " return_tolerance=0.0078431373 first_capture_passes=" << firstFrame.SurfacePasses
			<< " second_capture_passes=" << secondFrame.SurfacePasses
			<< " return_capture_passes=" << returnFrame.SurfacePasses << '\n';
	}
	void Verify(
		Device &device, Scene &scene, std::string_view family, std::string_view variant, std::ostream &out
	) {
		const bool inactive = variant == "inactive", empty = variant == "empty";
		const auto coldFrame = Submit(device, scene);
		const auto oracleFrame = Submit(device, scene);
		if (family == "capture")
			Require(oracleFrame.PortalPasses > 0, "portal consumer fixture did not capture a portal");
		if (family == "mirror")
			Require(
				coldFrame.SurfacePasses > 0,
				"mirror consumer fixture did not capture a mirror on initial frame"
			);
		const auto baseline = Output(device, scene);
		Submit(device, scene);
		const auto repeat = Output(device, scene);
		// The native depth buffer is hardware depth; this input is the dedicated
		// R32F axial distance produced by depth-linearise for these screen consumers.
		if (variant != "complex" && family != "capture" && family != "mirror" &&
			!(family == "trace" && empty)) {
			auto depth = Read(
				device,
				core::Name("linear-depth"),
				scene.View.Slot,
				scene.Target.Width,
				scene.Target.Height,
				4
			);
			depth.Scalar = true;
			Require(
				std::abs(depth.Pixel(depth.Width / 2, depth.Height / 2).x - 6.0f) <= .002f,
				"linear depth did not preserve the six-metre receiver under the selected projection"
			);
		}
		Require(
			Difference(baseline, repeat) <= 2.0 / 255,
			"fixed-input ray consumer exceeded two-code repeatability tolerance"
		);
		out << "# ray-consumer-oracle case=" << family << " scene=" << variant
			<< " rgba8_hash=" << assets::Hasher::Of(baseline.Bytes).ToHex()
			<< " repeat_max_error=" << Difference(baseline, repeat) << " repeat_tolerance=0.0078431373";
		if (family == "mirror")
			out << " cold_surface_passes=" << coldFrame.SurfacePasses
				<< " repeat_surface_passes=" << oracleFrame.SurfacePasses;
		if (family == "trace" && !inactive) {
			const auto first = Output(device, scene, "bench-path", true);
			const float expected = float(TraceQualityFor(variant).HistorySamples);
			Require(
				first.Pixel(first.Width / 2, first.Height / 2).a == expected,
				"pathtrace reset sample count is wrong"
			);
			scene.View.Damage = {};
			Require(
				device.Renderer.Render(std::span(&scene.View, 1), scene.Overlay, nullptr, false).Submitted,
				"path history oracle did not submit"
			);
			device.Wait();
			const auto accumulated = Output(device, scene, "bench-path", true);
			Require(
				accumulated.Pixel(first.Width / 2, first.Height / 2).a == std::min(expected * 2, 63.f),
				"pathtrace history did not accumulate"
			);
			Submit(device, scene);
			const auto reset = Output(device, scene, "bench-path", true);
			Require(
				reset.Pixel(first.Width / 2, first.Height / 2).a == expected,
				"pathtrace content change did not reset history"
			);
			for (const char *name : {"bench-indirect", "bench-reflection", "bench-path"}) {
				const auto image = Output(device, scene, name, true);
				Difference(image, image);
				float maximumRadiance = 0;
				size_t rayHits = 0;
				for (uint32_t y = 0; y < image.Height; ++y)
					for (uint32_t x = 0; x < image.Width; ++x) {
						const auto pixel = image.Pixel(x, y);
						maximumRadiance = std::max({maximumRadiance, pixel.r, pixel.g, pixel.b});
						rayHits += pixel.a > .5f;
					}
				Require(
					empty ? maximumRadiance <= .001f : maximumRadiance > .00001f,
					"trace consumer empty/positive radiance oracle failed"
				);
				if (std::string_view(name) == "bench-reflection" && !empty)
					Require(rayHits > 0, "reflective scene produced no screen-depth hits");
				out << ' ' << name << "_hash=" << assets::Hasher::Of(image.Bytes).ToHex();
			}
			out << " history=reset_accumulate_reset";
		}
		if (family == "volume" && !inactive) {
			const auto before = Output(device, scene, "volume-lit", true);
			std::reverse(
				scene.View.Lighting.Volumes.begin(),
				scene.View.Lighting.Volumes.begin() + scene.View.Lighting.VolumeCount
			);
			Submit(device, scene);
			const auto reversed = Output(device, scene, "volume-lit", true);
			Require(Difference(before, reversed) <= .004, "volume resolved order changed the image");
			if (empty)
				Require(
					Difference(before, Output(device, scene, "weather-lit", true)) <= .004,
					"nonintersecting volume changed pixels"
				);
			if (variant == "simple" && !scene.View.Projection.has_value()) {
				const auto base = Output(device, scene, "weather-lit", true);
				const uint32_t x = before.Width / 2, y = before.Height / 2;
				const float tangent = std::tan(scene.View.Camera.FieldOfViewRadians / 2);
				const float horizontal =
					(2 * (float(x) + .5f) / before.Width - 1) * tangent * float(before.Width) / before.Height;
				const float vertical = (1 - 2 * (float(y) + .5f) / before.Height) * tangent;
				const float rayLengthFactor = std::sqrt(1 + horizontal * horizontal + vertical * vertical);
				const float transmission = std::exp(-.5f * rayLengthFactor);
				const auto expected = glm::vec3(base.Pixel(x, y)) * transmission;
				const auto actual = glm::vec3(before.Pixel(x, y));
				Require(
					glm::length(actual - expected) <= .004f,
					"homogeneous fog failed analytic Beer-Lambert transmission"
				);
				out << " homogeneous_transmission=" << transmission << " analytic_tolerance=0.004";
			}
			out << " volume_order_tolerance=0.004";
		}
		if (family == "environment") {
			const auto sky = Read(device, core::Name("environment-sky"), scene.View.Slot, 1024, 512);
			const auto clouds = Read(device, core::Name("environment-clouds"), scene.View.Slot, 1024, 512);
			const double difference = Difference(sky, clouds);
			Require(
				inactive || empty ? difference <= .004 : difference > .001,
				"cloud environment identity/effect oracle failed"
			);
			out << " environment_max_effect=" << difference;
		}
		if (family == "god-rays") {
			const auto source = Output(device, scene, "dof", true),
					   effect = Output(device, scene, "god-rays", true);
			const double difference = Difference(source, effect);
			Require(
				inactive || empty ? difference <= .004 : difference > .0001,
				"god rays identity/effect oracle failed"
			);
			out << " shafts_max_effect=" << difference;
		}
		if (family == "lens") {
			const auto source = Output(device, scene, "display", true),
					   effect = Output(device, scene, "lens-b", true);
			const double difference = Difference(source, effect);
			Require(
				inactive || empty ? difference <= .004 : difference > .001,
				"lens identity/effect oracle failed"
			);
			out << " lens_max_effect=" << difference;
			if (variant == "simple") {
				const auto matrices =
					scene.View.Projection
						? scene::ResolveSurfaceCamera(scene.View.CameraFrame, *scene.View.Projection)
						: scene::ResolveCamera(
							  scene.View.CameraFrame,
							  scene.View.Camera,
							  float(scene.Target.Width) / scene.Target.Height
						  );
				const auto probeX = [&](float horizontal) {
					const auto clip = matrices.ViewProjection * glm::vec4(horizontal, 0, -3, 1);
					const auto x = uint32_t((clip.x / clip.w * .5f + .5f) * source.Width);
					return std::min(x, source.Width - 1);
				};
				const auto correctedBlue = [&](const Image &image, uint32_t x) {
					const auto pixel = image.Pixel(x, image.Height / 2);
					return pixel.b - .12f * pixel.r;
				};
				const uint32_t leftX = probeX(-.3f), rightX = probeX(.3f);
				const float leftSource = correctedBlue(source, leftX),
							leftEffect = correctedBlue(effect, leftX);
				const float rightSource = correctedBlue(source, rightX),
							rightEffect = correctedBlue(effect, rightX);
				const bool inward = leftEffect > leftSource + .01f && rightEffect < rightSource - .01f;
				if (!inward)
					std::cerr << "# ray-consumer-lens-failure left_x=" << leftX << " right_x=" << rightX
							  << " left_source=" << leftSource << " left_effect=" << leftEffect
							  << " right_source=" << rightSource << " right_effect=" << rightEffect << '\n';
				Require(inward, "gravitational lens failed inward gradient oracle");
				out << " lens_gradient=inward emission_red_correction=0.12";
			}
		}
		if (family == "fallback" || family == "capture" || family == "mirror") {
			const bool enabled = scene.View.Lighting.EnvironmentState.Weather.Enabled;
			scene.View.Lighting.EnvironmentState.Weather.Enabled = false;
			Submit(device, scene);
			const auto clear = Output(device, scene);
			scene.View.Lighting.EnvironmentState.Weather.Enabled = enabled;
			Submit(device, scene);
			const auto cloudy = Output(device, scene);
			const double difference = Difference(clear, cloudy);
			Require(
				inactive || empty ? difference <= 2.0 / 255 : difference > 2.0 / 255,
				"weather consuming path identity/effect oracle failed"
			);
			if ((family == "capture" || family == "mirror") && !inactive && !empty) {
				const auto matrices =
					scene.View.Projection
						? scene::ResolveSurfaceCamera(scene.View.CameraFrame, *scene.View.Projection)
						: scene::ResolveCamera(
							  scene.View.CameraFrame,
							  scene.View.Camera,
							  float(scene.Target.Width) / scene.Target.Height
						  );
				double paneEffect = 0;
				size_t testedPaneProbes = 0;
				for (const auto &instance : scene.Instances) {
					if (instance.Surface < 0) continue;
					const auto point =
						matrices.ViewProjection *
						glm::vec4(
							instance.Frame.Position.X, instance.Frame.Position.Y, instance.Frame.Position.Z, 1
						);
					if (point.w <= 0) continue;
					const int x = int((point.x / point.w * .5f + .5f) * clear.Width),
							  y = int((.5f - point.y / point.w * .5f) * clear.Height);
					if (x < 5 || y < 5 || x + 5 >= int(clear.Width) || y + 5 >= int(clear.Height)) continue;
					++testedPaneProbes;
					for (int dy = -4; dy <= 4; ++dy)
						for (int dx = -4; dx <= 4; ++dx) {
							const auto a = clear.Pixel(uint32_t(x + dx), uint32_t(y + dy)),
									   b = cloudy.Pixel(uint32_t(x + dx), uint32_t(y + dy));
							for (uint32_t channel = 0; channel < 3; ++channel)
								paneEffect = std::max(paneEffect, double(std::abs(a[channel] - b[channel])));
						}
				}
				Require(testedPaneProbes > 0, "captured aperture fixture has no valid interior probe");
				Require(paneEffect > 2.0 / 255, "weather changed no pixels inside the captured aperture");
				out << " tested_pane_probes=" << testedPaneProbes
					<< " pane_interior_weather_effect=" << paneEffect;
			}
			out << " weather_max_effect=" << difference;
		}
		out << " status=passed readback=outside_timing "
			   "baseline_hashes=compare_same_device_across_optimization_passes\n";
	}
	void RunScenario(
		std::string_view family,
		std::string_view scene,
		uint32_t width,
		uint32_t height,
		uint32_t frames,
		std::string_view projection,
		std::string_view workload = {}
	) {
		std::ostringstream cohort;
		{
			Device oracle;
			Require(oracle.Renderer.AddMesh(MESH, PlaneMesh()), "ray oracle plane mesh refused");
			InstallPipeline(oracle, family, scene);
			Scene fixture;
			Configure(oracle, fixture, family, scene, projection, width, height);
			Verify(oracle, fixture, family, scene, cohort);
			if (family == "mirror" && workload == "moving_camera")
				VerifyMovingMirror(oracle, fixture, cohort);
		}
		// A fresh device excludes oracle pins and readback allocations from the
		// measured device's lifetime peak as well as its steady residency.
		Device device;
		Require(
			device.Renderer.Capabilities().HasCompute && device.Renderer.Capabilities().HasStorageTextures,
			"required ray benchmark device capabilities unavailable"
		);
		Require(device.Renderer.AddMesh(MESH, PlaneMesh()), "ray benchmark plane mesh refused");
		InstallPipeline(device, family, scene, false);
		Scene fixture;
		Configure(device, fixture, family, scene, projection, width, height);
		const double fallbackBefore = Counter("render.weather.fragment_fallbacks"),
					 captureBefore = Counter("render.weather.capture_canonical_fallbacks"),
					 scratchBefore = Counter("render.weather.capture_scratch_image_creations"),
					 captureViewsBefore = Counter("render.weather.capture_views");
		const auto captureBase = fixture.View.CameraFrame;
		const auto report = Measure(
			device, std::span(&fixture.View, 1), fixture.Overlay, frames, PASSES, [&](uint32_t frame) {
				if (family == "mirror" && workload == "moving_camera")
					SetMirrorEye(fixture, MovingMirrorEye(captureBase, frame));
				fixture.View.Damage.Scene = true;
				if (family == "environment")
					fixture.View.Lighting.EnvironmentState.CloudTime = .75 + double(frame) / 60;
			}
		);
		if (family == "mirror" && workload == "moving_camera") {
			Require(
				report.FramesWithSurfaceCaptures == frames,
				"moving mirror missed a measured surface capture frame"
			);
			if (scene == "inactive")
				Require(
					Counter("render.weather.capture_views") == captureViewsBefore,
					"inactive moving mirror captured weather views"
				);
			else
				Require(
					Counter("render.weather.capture_views") - captureViewsBefore >= double(frames),
					"active moving mirror missed measured weather captures"
				);
		} else if (family == "mirror" && workload == "retained_static") {
			Require(
				report.FramesWithSurfaceCaptures == 0,
				"retained static mirror recaptured during measured frames"
			);
		}
		Print(device, report, family, scene, projection, width, height, cohort, workload);
		cohort << "# ray-consumer-work case=" << family << " scene=" << scene
			   << " oblique_clip_skew=" << ObliqueClipSkew(family, projection)
			   << " instances=" << fixture.Instances.size()
			   << " volume_count=" << fixture.View.Lighting.VolumeCount << " volume_steps="
			   << (fixture.View.Lighting.VolumeCount ? fixture.View.Lighting.Volumes[0].Steps : 0)
			   << " volume_shadow_steps="
			   << (fixture.View.Lighting.VolumeCount ? fixture.View.Lighting.Volumes[0].ShadowSteps : 0)
			   << " volume_noise_strength="
			   << (fixture.View.Lighting.VolumeCount ? fixture.View.Lighting.Volumes[0].NoiseStrength : 0)
			   << " lens_count=" << fixture.View.Lighting.ShaderLensCount
			   << " portal_count=" << fixture.Portals.size() << " mirror_count=" << fixture.Mirrors.size()
			   << " god_ray_radius=" << fixture.View.Lighting.GodRayRadius
			   << " cloud_steps=" << fixture.View.Lighting.EnvironmentState.CloudVolume.Steps
			   << " fallback_dispatches_including_warmup_and_drain="
			   << Counter("render.weather.fragment_fallbacks") - fallbackBefore
			   << " canonical_captures_including_warmup_and_drain="
			   << Counter("render.weather.capture_canonical_fallbacks") - captureBefore
			   << " capture_scratch_creations_including_warmup_and_drain="
			   << Counter("render.weather.capture_scratch_image_creations") - scratchBefore
			   << " capture_views_including_warmup_and_drain="
			   << Counter("render.weather.capture_views") - captureViewsBefore;
		if (family == "trace" && scene != "inactive") {
			const auto quality = TraceQualityFor(scene);
			cohort << " gi_rays_per_pixel=" << quality.GiRaysPerPixel
				   << " reflection_steps=" << quality.ReflectionSteps
				   << " path_samples_per_frame=" << quality.PathSamplesPerFrame
				   << " path_bounces=" << quality.PathBounces;
		}
		if (family == "mirror") {
			const std::string_view weatherMedium = scene == "inactive" ? "inactive"
												   : scene == "empty"  ? "active_empty"
																	   : "active";
			cohort << " workload=" << workload << " weather_medium=" << weatherMedium
				   << " mirror_capture=enabled"
				   << " frames_with_surface_captures=" << report.FramesWithSurfaceCaptures;
		}
		cohort << " oracle_scope=repeatability_effect_identity_history_volume_order "
				  "detailed_projection_portal_weather_parity=existing_gpu_suites\n";
		RetainCompletedReport(
			"ray-consumer/" + std::string(family) + "/" + std::string(scene) + "/" + std::string(projection) +
				(family == "mirror" ? "/" + std::string(workload) : ""),
			cohort.str()
		);
	}
}

BENCH("opt-in existing ray consumer device scenarios; use printed per-frame reports", 1) {
	if (Setting("MONO_RAY_GPU_BENCH", std::string_view("0")) != "1") return;
	const auto selected = Setting("MONO_RAY_GPU_BENCH_CASE", std::string_view("all"));
	const auto scene = Setting("MONO_RAY_GPU_BENCH_SCENE", std::string_view("all"));
	const auto projection = Setting("MONO_RAY_GPU_BENCH_PROJECTION", std::string_view("perspective"));
	const auto mirrorWorkload = Setting("MONO_RAY_GPU_BENCH_MIRROR_WORKLOAD", std::string_view("all"));
	Require(
		mirrorWorkload == "all" || mirrorWorkload == "retained_static" || mirrorWorkload == "moving_camera",
		"unknown MONO_RAY_GPU_BENCH_MIRROR_WORKLOAD"
	);
	Require(
		selected == "all" || std::find(FAMILIES.begin(), FAMILIES.end(), selected) != FAMILIES.end(),
		"unknown ray benchmark case"
	);
	Require(
		scene == "all" || std::find(SCENES.begin(), SCENES.end(), scene) != SCENES.end(),
		"unknown ray benchmark scene"
	);
	const auto width = Setting("MONO_RAY_GPU_BENCH_WIDTH", 1920u, 3840u),
			   height = Setting("MONO_RAY_GPU_BENCH_HEIGHT", 1080u, 2160u),
			   frames = Setting("MONO_RAY_GPU_BENCH_FRAMES", 256u, 4096u);
	for (const std::string_view family : FAMILIES)
		if (selected == "all" || selected == family)
			for (const auto variant : SCENES)
				if (scene == "all" || scene == variant) {
					auto run = [&](std::string_view workload) {
						try {
							RunScenario(family, variant, width, height, frames, projection, workload);
						} catch (const std::exception &failure) {
							throw std::runtime_error(
								"case=" + std::string(family) + " scene=" + std::string(variant) +
								" projection=" + std::string(projection) + " workload=" +
								(workload.empty() ? "none" : std::string(workload)) + ": " + failure.what()
							);
						}
					};
					if (family != "mirror")
						run({});
					else if (mirrorWorkload == "all" || mirrorWorkload == "retained_static")
						run("retained_static");
					if (family == "mirror" && (mirrorWorkload == "all" || mirrorWorkload == "moving_camera"))
						run("moving_camera");
				}
}
