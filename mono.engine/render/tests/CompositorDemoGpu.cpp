#include "RenderFixture.hpp"

#include <engine/core/Name.hpp>
#include <engine/graph/PipelineDocument.hpp>
#include <engine/graph/RenderGraph.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>
#include <glm/gtc/packing.hpp>

#include <chrono>
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
	void Raster(graph::PipelineDocument &d, const char *n, const char *out) {
		d.Record(
			{.Kind = graph::EditKind::AddNode,
			 .Name = core::Name(n),
			 .NodeKind = core::Name("raster"),
			 .Scope = graph::NodeScope::View}
		);
		d.Record({.Kind = graph::EditKind::Writes, .Target = core::Name(out), .Key = core::Name("colour")});
		d.Record({.Kind = graph::EditKind::Set, .Key = core::Name("source"), .Value = Solid()});
	}
	graph::RenderGraph Install(Renderer &r) {
		auto base = graph::CompositorDemoDocument();
		graph::PipelineDocument d;
		bool inserted = false;
		for (auto &e : base.Edits()) {
			if (e.Kind == graph::EditKind::AddNode && e.Name == core::Name("grade-exposure") && !inserted) {
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
