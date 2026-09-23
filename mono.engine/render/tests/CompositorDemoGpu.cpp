#include "RenderFixture.hpp"

#include <engine/core/Name.hpp>
#include <engine/graph/PipelineDocument.hpp>
#include <engine/graph/RenderGraph.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>
#include <glm/gtc/packing.hpp>

#include <chrono>
#include <cstddef>
#include <cstring>
#include <span>
#include <string>
#include <vector>
TEST_SUITE_ID("engine.render.compositordemogpu")
TEST_DEPENDS("engine.graph.pipelinedocument")
namespace {
	using namespace engine;
	using namespace engine::render;
	using namespace engine::render::test;
	std::string Solid() {
		return R"(#version 450
layout(location=0) in vec2 inUv; layout(location=0) out vec4 outColour;
layout(set=3,binding=0,std140) uniform Pass { mat4 a; mat4 b; vec4 c; vec4 d; uvec4 e; } pass;
void main(){ float band=step(0.5,fract(inUv.x*7.0)); outColour=vec4(mix(vec3(0.05,0.2,1.5),vec3(1.8,0.18,0.04),band),1.0); })";
	}
	std::string BloomSource() {
		return R"(#version 450
layout(location=0) in vec2 inUv; layout(location=0) out vec4 outColour;
layout(set=3,binding=0,std140) uniform Pass { mat4 a; mat4 b; vec4 c; vec4 d; uvec4 e; } pass;
void main(){ vec2 centre=abs(inUv-vec2(0.5)); float source=step(max(centre.x,centre.y),0.12); outColour=vec4(mix(vec3(0.03),vec3(8.0,2.0,0.5),source),1.0); })";
	}
	void
	Raster(graph::PipelineDocument &d, const char *n, const char *out, const std::string &source = Solid()) {
		d.Record(
			{.Kind = graph::EditKind::AddNode,
			 .Name = core::Name(n),
			 .NodeKind = core::Name("raster"),
			 .Scope = graph::NodeScope::View}
		);
		d.Record({.Kind = graph::EditKind::Writes, .Target = core::Name(out), .Key = core::Name("colour")});
		d.Record({.Kind = graph::EditKind::Set, .Key = core::Name("source"), .Value = source});
	}
	graph::RenderGraph Install(Renderer &r) {
		auto base = graph::CompositorDemoDocument();
		graph::PipelineDocument d;
		bool inserted = false;
		for (auto &e : base.Edits()) {
			if (e.Kind == graph::EditKind::AddNode && e.Name == core::Name("dof") && !inserted) {
				d.Record(
					{.Kind = graph::EditKind::Enable, .Name = core::Name("shader-lenses"), .Enabled = false}
				);
				Raster(d, "compositor-solid-source", "lens-b");
				inserted = true;
			}
			d.Record(e);
		}
		REQUIRE(inserted);
		core::Name sink("compositor-gpu-sink");
		graph::NodeKindSpec s;
		s.Kind = sink;
		s.Scope = graph::NodeScope::Frame;
		s.Queue = graph::ExecutionQueue::Cpu;
		s.Category = graph::NodeCategory::Output;
		for (auto n :
			 {"grade",
			  "hsv-grade",
			  "mixed",
			  "transformed",
			  "blur-horizontal",
			  "blurred",
			  "compositor-display",
			  "compositor-scene",
			  "compositor-output"})
			s.Inputs.push_back({.Name = core::Name(n), .Kind = graph::ResourceKind::Texture});
		REQUIRE(graph::RegisterNodeKind(std::move(s)));
		REQUIRE(r.InstallNodeHandler(sink, [](const graph::RunContext &) { return true; }));
		d.Record(
			{.Kind = graph::EditKind::AddNode,
			 .Name = sink,
			 .NodeKind = sink,
			 .Scope = graph::NodeScope::Frame}
		);
		for (auto n :
			 {"grade",
			  "hsv-grade",
			  "mixed",
			  "transformed",
			  "blur-horizontal",
			  "blurred",
			  "compositor-display",
			  "compositor-scene",
			  "compositor-output"})
			d.Record({.Kind = graph::EditKind::Reads, .Target = core::Name(n), .Key = core::Name(n)});
		graph::RenderGraph g;
		core::Name o;
		REQUIRE(graph::Build(d, g, o) == graph::PipelineDocumentStatus::Ok);
		REQUIRE(r.SetPipeline(core::Name("compositor-gpu"), g));
		return g;
	}
	graph::RenderGraph InstallBloom(Renderer &renderer) {
		graph::RenderGraph graph;
		const auto texture = [&graph](const char *name, graph::ResourceFormat format) {
			const graph::ResourceId resource = graph.AddResource(
				{.Name = core::Name(name), .Kind = graph::ResourceKind::Colour, .Format = format}
			);
			REQUIRE(resource.IsValid());
			return resource;
		};
		const graph::ResourceId source = texture("bloom-source", graph::ResourceFormat::RGBA16F);
		const graph::ResourceId bloom = texture("bloom-result", graph::ResourceFormat::RGBA16F);
		const graph::ResourceId tonemapped = texture("tonemapped", graph::ResourceFormat::RGBA8);

		graph::Node raster;
		raster.Name = core::Name("bloom-source-pass");
		raster.Kind = core::Name("raster");
		raster.Scope = graph::NodeScope::View;
		raster.Writes = {source};
		raster.Parameters.push_back({core::Name("source"), BloomSource()});
		REQUIRE(graph.AddNode(std::move(raster)).IsValid());

		for (const auto &[name, kind, reads, writes] : std::array{
				 std::tuple{"bloom-pass", "bloom", std::vector{source}, std::vector{bloom}},
				 std::tuple{"tonemap-pass", "tonemap", std::vector{source, bloom}, std::vector{tonemapped}},
			 }) {
			graph::Node node;
			node.Name = core::Name(name);
			node.Kind = core::Name(kind);
			node.Scope = graph::NodeScope::View;
			node.Reads = reads;
			node.Writes = writes;
			REQUIRE(graph.AddNode(std::move(node)).IsValid());
		}

		const core::Name sinkKind("bloom-test-boundary");
		graph::NodeKindSpec sinkSpec;
		sinkSpec.Kind = sinkKind;
		sinkSpec.Scope = graph::NodeScope::Frame;
		sinkSpec.Queue = graph::ExecutionQueue::Cpu;
		sinkSpec.Category = graph::NodeCategory::Output;
		sinkSpec.Inputs.push_back({.Name = core::Name("tonemapped"), .Kind = graph::ResourceKind::Texture});
		REQUIRE(graph::RegisterNodeKind(std::move(sinkSpec)));
		REQUIRE(renderer.InstallNodeHandler(sinkKind, [](const graph::RunContext &) { return true; }));
		graph::Node sink;
		sink.Name = sinkKind;
		sink.Kind = sinkKind;
		sink.Scope = graph::NodeScope::Frame;
		sink.Reads = {tonemapped};
		REQUIRE(graph.AddNode(std::move(sink)).IsValid());
		REQUIRE(renderer.SetPipeline(core::Name("bloom-gpu"), graph));
		return graph;
	}

	std::vector<std::byte> Capture16(Renderer &r, core::Name n, size_t slot, uint32_t w, uint32_t h) {
		auto *d = static_cast<SDL_GPUDevice *>(r.Backend().Device);
		auto *t = static_cast<SDL_GPUTexture *>(r.ResourceTexture(n, slot));
		REQUIRE(d);
		REQUIRE(t);
		size_t row = (size_t(w) * 8 + 255) / 256 * 256;
		SDL_GPUTransferBufferCreateInfo i{};
		i.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
		i.size = uint32_t(row * h);
		auto *b = SDL_CreateGPUTransferBuffer(d, &i);
		REQUIRE(b);
		auto *c = SDL_AcquireGPUCommandBuffer(d);
		REQUIRE(c);
		auto *p = SDL_BeginGPUCopyPass(c);
		REQUIRE(p);
		SDL_GPUTextureRegion src{};
		src.texture = t;
		src.w = w;
		src.h = h;
		src.d = 1;
		SDL_GPUTextureTransferInfo dst{};
		dst.transfer_buffer = b;
		dst.pixels_per_row = uint32_t(row / 8);
		dst.rows_per_layer = h;
		SDL_DownloadFromGPUTexture(p, &src, &dst);
		SDL_EndGPUCopyPass(p);
		auto *f = SDL_SubmitGPUCommandBufferAndAcquireFence(c);
		REQUIRE(f);
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while (!SDL_QueryGPUFence(d, f) && std::chrono::steady_clock::now() < deadline)
			SDL_Delay(1);
		REQUIRE(SDL_QueryGPUFence(d, f));
		std::vector<std::byte> out(row * h);
		const void *mapped = SDL_MapGPUTransferBuffer(d, b, false);
		REQUIRE(mapped != nullptr);
		std::memcpy(out.data(), mapped, out.size());
		SDL_UnmapGPUTransferBuffer(d, b);
		SDL_ReleaseGPUFence(d, f);
		SDL_ReleaseGPUTransferBuffer(d, b);
		return out;
	}
	double Diff16(std::span<const std::byte> a, std::span<const std::byte> b, uint32_t w, uint32_t h) {
		size_t row = (size_t(w) * 8 + 255) / 256 * 256;
		double sum = 0;
		for (uint32_t y = 0; y < h; y++)
			for (uint32_t x = 0; x < w; x++)
				for (size_t c = 0; c < 3; c++) {
					uint16_t l = 0, r = 0;
					std::memcpy(&l, a.data() + y * row + x * 8 + c * 2, 2);
					std::memcpy(&r, b.data() + y * row + x * 8 + c * 2, 2);
					sum += std::abs(glm::unpackHalf1x16(l) - glm::unpackHalf1x16(r));
				}
		return sum;
	}

	std::string FocusDepth() {
		return R"(#version 450
layout(location=0) in vec2 inUv; layout(location=0) out vec4 outColour;
layout(set=3,binding=0,std140) uniform Pass { mat4 a; mat4 b; vec4 c; vec4 d; uvec4 e; } pass;
void main(){ outColour=vec4(inUv.x < 0.5 ? 24.0 : 100.0); })";
	}
	std::string EmptyDepth() {
		return R"(#version 450
layout(location=0) in vec2 inUv; layout(location=0) out vec4 outColour;
layout(set=3,binding=0,std140) uniform Pass { mat4 a; mat4 b; vec4 c; vec4 d; uvec4 e; } pass;
void main(){ outColour=vec4(0.0); })";
	}
	std::string SunBlockerDepth() {
		return R"(#version 450
layout(location=0) in vec2 inUv; layout(location=0) out vec4 outColour;
layout(set=3,binding=0,std140) uniform Pass { mat4 a; mat4 b; vec4 c; vec4 d; uvec4 e; } pass;
void main(){ float blocked=step(0.40,inUv.x)*step(inUv.x,0.60)*step(0.40,inUv.y)*step(inUv.y,0.60); outColour=vec4(blocked); })";
	}
	graph::RenderGraph InstallEffect(
		Renderer &renderer,
		const char *pipelineName,
		const char *effectKind,
		const std::string &colour,
		const std::string &depth
	) {
		graph::RenderGraph graph;
		auto texture = [&graph](const char *name, graph::ResourceFormat format) {
			const graph::ResourceId resource = graph.AddResource(
				{.Name = core::Name(name), .Kind = graph::ResourceKind::Colour, .Format = format}
			);
			REQUIRE(resource.IsValid());
			return resource;
		};
		const graph::ResourceId source = texture("effect-source", graph::ResourceFormat::RGBA16F);
		const graph::ResourceId linearDepth = texture("effect-depth", graph::ResourceFormat::R32F);
		const graph::ResourceId result = texture("effect-result", graph::ResourceFormat::RGBA16F);
		auto raster = [&graph](const char *name, graph::ResourceId output, const std::string &sourceText) {
			graph::Node node;
			node.Name = core::Name(name);
			node.Kind = core::Name("raster");
			node.Scope = graph::NodeScope::View;
			node.Writes = {output};
			node.Parameters.push_back({core::Name("source"), sourceText});
			REQUIRE(graph.AddNode(std::move(node)).IsValid());
		};
		raster("effect-colour-pass", source, colour);
		raster("effect-depth-pass", linearDepth, depth);
		graph::Node effect;
		effect.Name = core::Name("effect-pass");
		effect.Kind = core::Name(effectKind);
		effect.Scope = graph::NodeScope::View;
		effect.Reads = {source, linearDepth};
		effect.Writes = {result};
		REQUIRE(graph.AddNode(std::move(effect)).IsValid());
		const core::Name sinkKind(std::string(pipelineName) + "-sink");
		graph::NodeKindSpec sinkSpec;
		sinkSpec.Kind = sinkKind;
		sinkSpec.Scope = graph::NodeScope::Frame;
		sinkSpec.Queue = graph::ExecutionQueue::Cpu;
		sinkSpec.Category = graph::NodeCategory::Output;
		sinkSpec.Inputs.push_back({.Name = core::Name("colour"), .Kind = graph::ResourceKind::Texture});
		REQUIRE(graph::RegisterNodeKind(std::move(sinkSpec)));
		REQUIRE(renderer.InstallNodeHandler(sinkKind, [](const graph::RunContext &) { return true; }));
		graph::Node sink;
		sink.Name = sinkKind;
		sink.Kind = sinkKind;
		sink.Scope = graph::NodeScope::Frame;
		sink.Reads = {result};
		REQUIRE(graph.AddNode(std::move(sink)).IsValid());
		REQUIRE(renderer.SetPipeline(core::Name(pipelineName), graph));
		return graph;
	}
	double Diff16Region(
		std::span<const std::byte> a,
		std::span<const std::byte> b,
		uint32_t w,
		uint32_t h,
		uint32_t firstX,
		uint32_t lastX
	) {
		const size_t row = (size_t(w) * 8 + 255) / 256 * 256;
		double sum = 0.0;
		for (uint32_t y = 0; y < h; y++)
			for (uint32_t x = firstX; x < lastX; x++)
				for (size_t c = 0; c < 3; c++) {
					uint16_t left = 0, right = 0;
					std::memcpy(&left, a.data() + y * row + x * 8 + c * 2, 2);
					std::memcpy(&right, b.data() + y * row + x * 8 + c * 2, 2);
					sum += std::abs(glm::unpackHalf1x16(left) - glm::unpackHalf1x16(right));
				}
		return sum;
	}
	size_t Lit(const CapturedImage &i) {
		size_t n = 0;
		for (uint32_t y = 0; y < i.Height; y++)
			for (uint32_t x = 0; x < i.Width; x++) {
				auto p = y * i.RowStrideBytes + x * 4;
				n += std::to_integer<uint8_t>(i.Bytes[p]) || std::to_integer<uint8_t>(i.Bytes[p + 1]) ||
					 std::to_integer<uint8_t>(i.Bytes[p + 2]);
			}
		return n;
	}
}

size_t Different(const CapturedImage &a, const CapturedImage &b) {
	size_t changed = 0;
	for (uint32_t y = 0; y < a.Height; ++y)
		for (uint32_t x = 0; x < a.Width; ++x) {
			const size_t at = y * a.RowStrideBytes + x * 4;
			changed += a.Bytes[at] != b.Bytes[at] || a.Bytes[at + 1] != b.Bytes[at + 1] ||
					   a.Bytes[at + 2] != b.Bytes[at + 2];
		}
	return changed;
}
TEST_CASE("compositor demo runs its authored image workflow on Vulkan", "[render][gpu][compositor][.] ") {
	FixtureDevice f;
	f.Initialise();
	Install(f.Render);
	SceneTarget t{41, 31};
	View v;
	v.World = 1;
	v.Pipeline = core::Name("compositor-gpu");
	v.Target = &t;
	OverlayImage o;
	o.Resize(41, 31);
	o.Fill(2, 3, 7, 6, 17, 231, 41, 255);
	auto frame = f.Render.Render(std::span(&v, 1), o, nullptr, false);
	for (auto n :
		 {"compositor-solid-source",
		  "grade-exposure",
		  "grade-hsv",
		  "mix-original",
		  "frame-transform",
		  "blur-x",
		  "blur-y",
		  "compositor-tonemap",
		  "compositor-present",
		  "compositor-interface-pass",
		  "compositor-overlay",
		  "compositor-output-image"})
		CHECK(frame.Ran(core::Name(n)));
	auto cap = [&](const char *n) {
		return CaptureResource(f.Render, core::Name(n), v.Slot, 41, 31, ImageFormat::Rgba8Unorm);
	};
	auto grade16 = Capture16(f.Render, core::Name("grade"), v.Slot, 41, 31);
	auto source16 = Capture16(f.Render, core::Name("lens-b"), v.Slot, 41, 31);
	auto hsv16 = Capture16(f.Render, core::Name("hsv-grade"), v.Slot, 41, 31);
	auto mixed16 = Capture16(f.Render, core::Name("mixed"), v.Slot, 41, 31);
	auto transformed16 = Capture16(f.Render, core::Name("transformed"), v.Slot, 41, 31);
	auto blurX16 = Capture16(f.Render, core::Name("blur-horizontal"), v.Slot, 41, 31);
	auto blurred16 = Capture16(f.Render, core::Name("blurred"), v.Slot, 41, 31);
	CHECK(Diff16(source16, grade16, 41, 31) > 1);
	CHECK(Diff16(grade16, hsv16, 41, 31) > 1);
	CHECK(Diff16(hsv16, mixed16, 41, 31) > 1);
	CHECK(Diff16(mixed16, transformed16, 41, 31) > 1);
	CHECK(Diff16(transformed16, blurX16, 41, 31) > 1);
	CHECK(Diff16(blurX16, blurred16, 41, 31) > 0.1);
	auto display = cap("compositor-display");
	auto scene = cap("compositor-scene");
	auto output = cap("compositor-output");
	CHECK(Lit(display) > 100);
	CHECK(Lit(scene) > 100);
	CHECK(Lit(output) > 100);
	// Debug overlays belong to a visible window. The headless compositor still
	// runs the overlay node, copying the scene unchanged into its final image.
	CHECK(Different(scene, output) == 0);
}

TEST_CASE("bloom spreads HDR highlights before tone mapping on Vulkan", "[render][gpu][bloom]") {
	FixtureDevice fixture;
	fixture.Initialise();
	InstallBloom(fixture.Render);
	SceneTarget target{41, 31};
	View view;
	view.World = 1;
	view.Pipeline = core::Name("bloom-gpu");
	view.Target = &target;
	OverlayImage overlay;

	scene::WorldLighting lighting;
	lighting.BloomThreshold = 0.5f;
	lighting.BloomRadius = 6.0f;
	fixture.Render.SetLighting(lighting);
	fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false);
	const auto withoutBloom =
		CaptureResource(fixture.Render, core::Name("tonemapped"), view.Slot, 41, 31, ImageFormat::Rgba8Unorm);

	lighting.BloomIntensity = 1.5f;
	fixture.Render.SetLighting(lighting);
	fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false);
	const auto withBloom =
		CaptureResource(fixture.Render, core::Name("tonemapped"), view.Slot, 41, 31, ImageFormat::Rgba8Unorm);

	CHECK(Different(withoutBloom, withBloom) > 100);
	const size_t neighbour = 15 * withBloom.RowStrideBytes + 14 * 4;
	CHECK(
		std::to_integer<uint8_t>(withBloom.Bytes[neighbour]) >
		std::to_integer<uint8_t>(withoutBloom.Bytes[neighbour])
	);
}

TEST_CASE("depth of field preserves focus and blurs distant HDR detail on Vulkan", "[render][gpu][dof]") {
	FixtureDevice fixture;
	fixture.Initialise();
	InstallEffect(fixture.Render, "dof-gpu", "dof", Solid(), FocusDepth());
	SceneTarget target{41, 31};
	View view;
	view.World = 1;
	view.Pipeline = core::Name("dof-gpu");
	view.Target = &target;
	OverlayImage overlay;
	scene::WorldLighting lighting;
	lighting.DepthOfFieldIntensity = 1.0f;
	lighting.DepthOfFieldFocusDistance = 24.0f;
	lighting.DepthOfFieldFocusRange = 2.0f;
	lighting.DepthOfFieldRadius = 6.0f;
	fixture.Render.SetLighting(lighting);
	fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false);
	const auto source = Capture16(fixture.Render, core::Name("effect-source"), view.Slot, 41, 31);
	const auto blurred = Capture16(fixture.Render, core::Name("effect-result"), view.Slot, 41, 31);
	CHECK(Diff16Region(source, blurred, 41, 31, 0, 20) < 0.01);
	CHECK(Diff16Region(source, blurred, 41, 31, 21, 41) > 10.0);
}

TEST_CASE("god rays stop at linear-depth occluders on Vulkan", "[render][gpu][god-rays]") {
	FixtureDevice clearFixture;
	clearFixture.Initialise();
	InstallEffect(clearFixture.Render, "god-rays-clear-gpu", "god-rays", BloomSource(), EmptyDepth());
	SceneTarget target{41, 31};
	View clearView;
	clearView.World = 1;
	clearView.Pipeline = core::Name("god-rays-clear-gpu");
	clearView.Target = &target;
	OverlayImage overlay;
	scene::WorldLighting lighting;
	lighting.Direction = {0.0f, 0.0f, 1.0f};
	lighting.GodRayIntensity = 2.0f;
	lighting.GodRayThreshold = 0.5f;
	lighting.GodRayRadius = 48.0f;
	clearFixture.Render.SetLighting(lighting);
	const auto clearFrame = clearFixture.Render.Render(std::span(&clearView, 1), overlay, nullptr, false);
	CHECK(clearFrame.Ran(core::Name("effect-pass")));
	const auto clearSource =
		Capture16(clearFixture.Render, core::Name("effect-source"), clearView.Slot, 41, 31);
	const auto unobscured =
		Capture16(clearFixture.Render, core::Name("effect-result"), clearView.Slot, 41, 31);
	CHECK(Diff16(clearSource, unobscured, 41, 31) > 1.0);

	FixtureDevice blockedFixture;
	blockedFixture.Initialise();
	InstallEffect(
		blockedFixture.Render, "god-rays-blocked-gpu", "god-rays", BloomSource(), SunBlockerDepth()
	);
	View blockedView;
	blockedView.World = 1;
	blockedView.Pipeline = core::Name("god-rays-blocked-gpu");
	blockedView.Target = &target;
	blockedFixture.Render.SetLighting(lighting);
	blockedFixture.Render.Render(std::span(&blockedView, 1), overlay, nullptr, false);
	const auto blocked =
		Capture16(blockedFixture.Render, core::Name("effect-result"), blockedView.Slot, 41, 31);
	CHECK(Diff16(unobscured, blocked, 41, 31) > 1.0);
}
