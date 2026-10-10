#include "RenderFixture.hpp"

#include <engine/core/Metrics.hpp>
#include <engine/core/Name.hpp>
#include <engine/graph/PipelineDocument.hpp>
#include <engine/graph/RenderGraph.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/render/ShaderCompiler.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>
#include <glm/gtc/packing.hpp>

#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <span>
#include <string>
#include <tuple>
#include <vector>
TEST_SUITE_ID("engine.render.compositordemogpu")
TEST_DEPENDS("engine.graph.pipelinedocument")
namespace {
	using namespace engine;
	using namespace engine::render;
	using namespace engine::render::test;
	double CounterValue(std::string_view name) {
		const auto snapshot = core::Metrics::Snapshot();
		for (const auto &counter : snapshot.Counters)
			if (counter.Name == core::Name(name)) return counter.Value;
		return 0;
	}

	void InstallBoundary(
		Renderer &renderer,
		graph::RenderGraph &pipeline,
		const char *name,
		std::span<const graph::ResourceId> resources,
		graph::NodeScope scope = graph::NodeScope::View
	) {
		const core::Name sinkKind(std::string(name) + "-sink");
		graph::NodeKindSpec spec;
		spec.Kind = sinkKind;
		spec.Scope = scope;
		spec.Queue = graph::ExecutionQueue::Cpu;
		spec.Category = graph::NodeCategory::Output;
		for (const auto resource : resources)
			spec.Inputs.push_back(
				{.Name = pipeline.FindResource(resource)->Name, .Kind = graph::ResourceKind::Texture}
			);
		REQUIRE(graph::RegisterNodeKind(std::move(spec)));
		REQUIRE(renderer.InstallNodeHandler(sinkKind, [](const graph::RunContext &) { return true; }));
		graph::Node sink;
		sink.Name = sinkKind;
		sink.Kind = sinkKind;
		sink.Scope = scope;
		sink.Reads.assign(resources.begin(), resources.end());
		REQUIRE(pipeline.AddNode(std::move(sink)).IsValid());
		REQUIRE(renderer.SetPipeline(core::Name(name), pipeline));
	}

	bool Constant16(
		std::span<const std::byte> image,
		uint32_t width,
		uint32_t height,
		const std::array<float, 4> &expected
	) {
		const size_t stride = (size_t(width) * 8 + 255) / 256 * 256;
		for (uint32_t y = 0; y < height; ++y)
			for (uint32_t x = 0; x < width; ++x)
				for (size_t channel = 0; channel < 4; ++channel) {
					uint16_t bits = 0;
					std::memcpy(&bits, image.data() + y * stride + x * 8 + channel * 2, 2);
					if (glm::unpackHalf1x16(bits) != expected[channel]) return false;
				}
		return true;
	}

	// Real shader commands in the engine's interface pass, so retained GUI pixels
	// are checked independently of the backend's per-frame readiness callback.
	struct SolidInterface : FrameOverlayHook {
		SDL_GPUDevice *Device = nullptr;
		SDL_GPUGraphicsPipeline *Pipeline = nullptr;
		bool Ready = true;
		uint32_t Records = 0;
		explicit SolidInterface(Renderer &renderer)
			: Device(static_cast<SDL_GPUDevice *>(renderer.Backend().Device)) {
			ShaderCompiler compiler;
			const auto vertex = compiler.Compile(
				R"(#version 450
void main(){ vec2 p=vec2((gl_VertexIndex<<1)&2,gl_VertexIndex&2); gl_Position=vec4(p*2.0-1.0,0.0,1.0); })",
				ShaderStage::Vertex
			);
			const auto fragment = compiler.Compile(
				R"(#version 450
layout(location=0) out vec4 colour;
void main(){ colour=vec4(0.25,0.0,0.0,0.5); })",
				ShaderStage::Fragment
			);
			REQUIRE_FALSE(vertex.Failed);
			REQUIRE_FALSE(fragment.Failed);
			const auto shader = [&](const ShaderCompilation &code, SDL_GPUShaderStage stage) {
				SDL_GPUShaderCreateInfo info{};
				info.code = reinterpret_cast<const uint8_t *>(code.SpirV.data());
				info.code_size = code.SpirV.size() * sizeof(uint32_t);
				info.entrypoint = "main";
				info.format = SDL_GPU_SHADERFORMAT_SPIRV;
				info.stage = stage;
				return SDL_CreateGPUShader(Device, &info);
			};
			auto *vertexShader = shader(vertex, SDL_GPU_SHADERSTAGE_VERTEX);
			auto *fragmentShader = shader(fragment, SDL_GPU_SHADERSTAGE_FRAGMENT);
			REQUIRE(vertexShader);
			REQUIRE(fragmentShader);
			SDL_GPUColorTargetDescription target{};
			target.format = static_cast<SDL_GPUTextureFormat>(renderer.Backend().ColourFormat);
			SDL_GPUGraphicsPipelineCreateInfo info{};
			info.vertex_shader = vertexShader;
			info.fragment_shader = fragmentShader;
			info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
			info.target_info.color_target_descriptions = &target;
			info.target_info.num_color_targets = 1;
			Pipeline = SDL_CreateGPUGraphicsPipeline(Device, &info);
			SDL_ReleaseGPUShader(Device, vertexShader);
			SDL_ReleaseGPUShader(Device, fragmentShader);
			REQUIRE(Pipeline);
		}
		~SolidInterface() override {
			SDL_ReleaseGPUGraphicsPipeline(Device, Pipeline);
		}
		bool Prepare(void *) override {
			return Ready;
		}
		void Record(void *, void *renderPass) override {
			auto *pass = static_cast<SDL_GPURenderPass *>(renderPass);
			SDL_BindGPUGraphicsPipeline(pass, Pipeline);
			SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
			++Records;
		}
	};

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
	// Authored RGBA sRGB is sampled into the headless native BGRA unorm target.
	// Permit one code value for the GPU's storage conversion rounding.
	size_t NativeConversionMismatch(const CapturedImage &source, const CapturedImage &native) {
		size_t different = 0;
		for (uint32_t y = 0; y < source.Height; ++y)
			for (uint32_t x = 0; x < source.Width; ++x) {
				const size_t from = y * source.RowStrideBytes + x * 4;
				const size_t to = y * native.RowStrideBytes + x * 4;
				bool mismatch = false;
				for (size_t channel = 0; channel < 3; ++channel) {
					const float encoded =
						float(std::to_integer<uint8_t>(source.Bytes[from + channel])) / 255.f;
					const float linear =
						encoded <= 0.04045f ? encoded / 12.92f : std::pow((encoded + 0.055f) / 1.055f, 2.4f);
					const int expected = int(std::round(linear * 255.f));
					const int actual = std::to_integer<uint8_t>(native.Bytes[to + 2 - channel]);
					mismatch |= std::abs(expected - actual) > 1;
				}
				different += mismatch;
			}
		return different;
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
	const auto clearBefore = CounterValue("render.colour_clear.commands");
	const auto clearBytesBefore = CounterValue("render.colour_clear.bytes");
	const auto inactiveFrame = fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false);
	CHECK(CounterValue("render.colour_clear.commands") - clearBefore == 1);
	CHECK(CounterValue("render.colour_clear.bytes") - clearBytesBefore == 41 * 31 * 8);
	CHECK(inactiveFrame.DrawCalls == 2);
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
	lighting.BloomIntensity = 0;
	fixture.Render.SetLighting(lighting);
	CHECK(fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false).DrawCalls == 2);
	const auto reset =
		CaptureResource(fixture.Render, core::Name("tonemapped"), view.Slot, 41, 31, ImageFormat::Rgba8Unorm);
	CHECK(Different(withoutBloom, reset) == 0);
	const auto bloom = Capture16(fixture.Render, core::Name("bloom-result"), view.Slot, 41, 31);
	bool clearedPixels = true;
	const size_t row = (41 * 8 + 255) / 256 * 256;
	for (uint32_t y = 0; y < 31; ++y)
		for (uint32_t x = 0; x < 41; ++x)
			for (size_t channel = 0; channel < 8; ++channel)
				clearedPixels = clearedPixels && bloom[y * row + x * 8 + channel] == std::byte{};
	CHECK(clearedPixels);
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

TEST_CASE(
	"inactive lighting effects preserve HDR pixels without effect draws", "[render][gpu][inactive-effects]"
) {
	for (const auto &[name, kind] :
		 std::array{std::pair{"inactive-dof", "dof"}, std::pair{"inactive-god-rays", "god-rays"}}) {
		INFO(kind);
		FixtureDevice fixture;
		fixture.Initialise();
		InstallEffect(fixture.Render, name, kind, Solid(), FocusDepth());
		SceneTarget target{41, 31};
		View view;
		view.World = 1;
		view.Pipeline = core::Name(name);
		view.Target = &target;
		OverlayImage overlay;
		scene::WorldLighting lighting;
		lighting.Direction = {0, 0, 1};
		const auto expectIdentity = [&] {
			fixture.Render.SetLighting(lighting);
			const auto copiesBefore = CounterValue("render.identity_copy.commands");
			const auto bytesBefore = CounterValue("render.identity_copy.bytes");
			const auto frame = fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false);
			CHECK(frame.DrawCalls == 2);
			CHECK(frame.ComputeDispatches == 0);
			CHECK(CounterValue("render.identity_copy.commands") - copiesBefore == 1);
			CHECK(CounterValue("render.identity_copy.bytes") - bytesBefore == 41 * 31 * 8);
			const auto source = Capture16(fixture.Render, core::Name("effect-source"), view.Slot, 41, 31);
			const auto output = Capture16(fixture.Render, core::Name("effect-result"), view.Slot, 41, 31);
			CHECK(Diff16(source, output, 41, 31) == 0);
		};
		expectIdentity();
		lighting.DepthOfFieldIntensity = 1;
		lighting.GodRayIntensity = 1;
		fixture.Render.SetLighting(lighting);
		CHECK(fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false).DrawCalls == 3);
		// An active image must be replaced when the same output becomes inactive.
		lighting.DepthOfFieldRadius = 0;
		lighting.GodRayRadius = 0;
		expectIdentity();
		if (std::string_view(kind) == "god-rays") {
			lighting.GodRayRadius = 16;
			lighting.Direction = {0, 0, -1};
			expectIdentity();
		}
	}
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

TEST_CASE(
	"disabled environment stages preserve histories across provider transitions",
	"[render][gpu][inactive-environment]"
) {
	FixtureDevice fixture;
	fixture.Initialise();
	graph::RenderGraph pipeline;
	const auto history = [&](const char *name) {
		return pipeline.AddResource(
			{.Name = core::Name(name),
			 .Kind = graph::ResourceKind::Storage,
			 .Format = graph::ResourceFormat::RGBA16F,
			 .Width = 32,
			 .Height = 16,
			 .Lifetime = graph::ResourceLifetime::History}
		);
	};
	const auto sky = history("environment-sky"), clouds = history("environment-clouds");
	REQUIRE(sky.IsValid());
	REQUIRE(clouds.IsValid());
	for (const auto &[name, reads, writes] : std::array{
			 std::tuple{"skybox-compute", std::vector<graph::ResourceId>{}, std::vector{sky}},
			 std::tuple{"clouds-compute", std::vector{sky}, std::vector{clouds}}
		 }) {
		graph::Node node;
		node.Name = core::Name(name);
		node.Kind = node.Name;
		node.Scope = graph::NodeScope::World;
		node.Reads = reads;
		node.Writes = writes;
		REQUIRE(pipeline.AddNode(std::move(node)).IsValid());
	}
	InstallBoundary(fixture.Render, pipeline, "inactive-environment", std::array{sky, clouds});
	SceneTarget target{32, 16};
	View view;
	view.World = 17;
	view.Pipeline = core::Name("inactive-environment");
	view.Target = &target;
	view.OverrideLighting = true;
	auto &environment = view.Lighting.EnvironmentState;
	OverlayImage overlay;
	const auto render = [&] { return fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false); };
	const auto skyImage = [&] {
		return Capture16(fixture.Render, core::Name("environment-sky"), view.Slot, 32, 16);
	};
	const auto cloudImage = [&] {
		return Capture16(fixture.Render, core::Name("environment-clouds"), view.Slot, 32, 16);
	};
	const auto copiesBefore = CounterValue("render.identity_environment_copy.commands");
	const auto clearBefore = CounterValue("render.empty_environment.clears");
	CHECK(render().ComputeDispatches == 0);
	CHECK(CounterValue("render.empty_environment.clears") - clearBefore == 1);
	const auto dark = skyImage();
	CHECK(Constant16(dark, 32, 16, {0.f, 0.f, 0.f, 1.f}));
	CHECK(Diff16(dark, cloudImage(), 32, 16) == 0);
	CHECK(CounterValue("render.identity_environment_copy.commands") - copiesBefore == 1);
	// An unchanged disabled provider keeps both completed outputs.
	const auto cachedCopies = CounterValue("render.identity_environment_copy.commands");
	const auto cachedClears = CounterValue("render.empty_environment.clears");
	environment.CloudTime = 91;
	environment.SkyCompute.Zenith = {0.3f, 0.2f, 0.1f};
	CHECK(render().ComputeDispatches == 0);
	CHECK(CounterValue("render.identity_environment_copy.commands") == cachedCopies);
	CHECK(CounterValue("render.empty_environment.clears") == cachedClears);
	// An atmosphere remains a live provider even without a selected skybox.
	environment.HasAtmosphere = true;
	CHECK(render().ComputeDispatches == 1);
	const auto atmosphereOnly = skyImage();
	environment.SkyCompute.Seed += 1;
	CHECK(render().ComputeDispatches == 0);
	CHECK(Diff16(atmosphereOnly, skyImage(), 32, 16) == 0);
	environment.HasAtmosphere = false;
	CHECK(render().ComputeDispatches == 0);
	CHECK(Constant16(skyImage(), 32, 16, {0.f, 0.f, 0.f, 1.f}));

	environment.Skybox = scene::SkyboxSource::Compute;
	CHECK(render().ComputeDispatches == 1);
	const auto activeSky = skyImage();
	CHECK(Diff16(activeSky, dark, 32, 16) > 1);
	CHECK(Diff16(activeSky, cloudImage(), 32, 16) == 0);
	// The procedural sky does not consume a disabled atmosphere's values.
	environment.Air.Colour = {0.9f, 0.1f, 0.3f};
	environment.Air.Glare = 4;
	CHECK(render().ComputeDispatches == 0);
	CHECK(Diff16(activeSky, skyImage(), 32, 16) == 0);
	CHECK(Diff16(activeSky, cloudImage(), 32, 16) == 0);
	// Enabling the edited provider must regenerate both outputs immediately.
	environment.HasAtmosphere = true;
	CHECK(render().ComputeDispatches == 1);
	const auto atmosphereSky = skyImage();
	CHECK(Diff16(activeSky, atmosphereSky, 32, 16) > 1);
	CHECK(Diff16(atmosphereSky, cloudImage(), 32, 16) == 0);
	environment.AirCompute.Rayleigh = 3;
	CHECK(render().ComputeDispatches == 0);
	CHECK(Diff16(atmosphereSky, skyImage(), 32, 16) == 0);
	environment.HasAtmosphere = false;
	CHECK(render().ComputeDispatches == 1);
	CHECK(Diff16(activeSky, skyImage(), 32, 16) == 0);
	environment.HasClouds = true;
	environment.CloudLayer.WindSpeed = 0;
	environment.CloudLayer.Cover = 1;
	environment.CloudLayer.Density = 1;
	CHECK(render().ComputeDispatches == 1);
	const auto activeClouds = cloudImage();
	CHECK(Diff16(activeSky, activeClouds, 32, 16) > 1);
	CHECK(render().ComputeDispatches == 0);

	environment.Skybox = scene::SkyboxSource::None;
	environment.HasClouds = false;
	CHECK(render().ComputeDispatches == 0);
	CHECK(Constant16(skyImage(), 32, 16, {0.f, 0.f, 0.f, 1.f}));
	CHECK(Constant16(cloudImage(), 32, 16, {0.f, 0.f, 0.f, 1.f}));
	environment.Skybox = scene::SkyboxSource::Compute;
	environment.HasClouds = true;
	CHECK(render().ComputeDispatches == 2);
	CHECK(Diff16(activeSky, skyImage(), 32, 16) == 0);
	CHECK(Diff16(activeClouds, cloudImage(), 32, 16) == 0);
}

TEST_CASE(
	"disabled clouds retain clamp and alpha semantics for authored input",
	"[render][gpu][inactive-environment]"
) {
	FixtureDevice fixture;
	fixture.Initialise();
	graph::RenderGraph pipeline;
	const auto source = pipeline.AddResource(
		{.Name = core::Name("authored-sky"),
		 .Kind = graph::ResourceKind::Storage,
		 .Format = graph::ResourceFormat::RGBA16F}
	);
	const auto result = pipeline.AddResource(
		{.Name = core::Name("authored-cloud-result"),
		 .Kind = graph::ResourceKind::Storage,
		 .Format = graph::ResourceFormat::RGBA16F}
	);
	// A builtin producer proves an opaque sky, then the authored writer below
	// replaces those actual texels and must invalidate both proof and cache.
	graph::Node sky;
	sky.Name = core::Name("authored-sky-generator");
	sky.Kind = core::Name("skybox-compute");
	sky.Scope = graph::NodeScope::World;
	sky.Writes = {source};
	REQUIRE(pipeline.AddNode(std::move(sky)).IsValid());
	graph::Node authoredSky;
	authoredSky.Name = core::Name("authored-sky-pass");
	authoredSky.Kind = core::Name("dispatch");
	authoredSky.Scope = graph::NodeScope::World;
	authoredSky.Writes = {source};
	authoredSky.Parameters.push_back({core::Name("source"), R"(#version 450
layout(local_size_x=8,local_size_y=8,local_size_z=1) in;
layout(set=1,binding=0,rgba16f) writeonly uniform image2D targetImage;
void main(){
    ivec2 pixel=ivec2(gl_GlobalInvocationID.xy);
    if(any(greaterThanEqual(pixel,imageSize(targetImage)))) return;
    imageStore(targetImage,pixel,vec4(-2.0,4.0,0.5,0.25));
})"});
	REQUIRE(pipeline.AddNode(std::move(authoredSky)).IsValid());
	graph::Node clouds;
	clouds.Name = core::Name("authored-clouds");
	clouds.Kind = core::Name("clouds-compute");
	clouds.Scope = graph::NodeScope::World;
	clouds.Reads = {source};
	clouds.Writes = {result};
	REQUIRE(pipeline.AddNode(std::move(clouds)).IsValid());
	InstallBoundary(fixture.Render, pipeline, "authored-clouds", std::array{source, result});
	SceneTarget target{32, 16};
	View view;
	view.World = 18;
	view.Pipeline = core::Name("authored-clouds");
	view.Target = &target;
	OverlayImage overlay;
	const auto clearsBefore = CounterValue("render.empty_environment.clears");
	CHECK(fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false).ComputeDispatches == 2);
	CHECK(CounterValue("render.empty_environment.clears") - clearsBefore == 1);
	CHECK(Constant16(
		Capture16(fixture.Render, core::Name("authored-sky"), view.Slot, 32, 16),
		32,
		16,
		{-2.f, 4.f, 0.5f, 0.25f}
	));
	CHECK(Constant16(
		Capture16(fixture.Render, core::Name("authored-cloud-result"), view.Slot, 32, 16),
		32,
		16,
		{0.f, 4.f, 0.5f, 1.f}
	));
	const auto repeatedClears = CounterValue("render.empty_environment.clears");
	CHECK(fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false).ComputeDispatches == 2);
	CHECK(CounterValue("render.empty_environment.clears") - repeatedClears == 1);
	CHECK(Constant16(
		Capture16(fixture.Render, core::Name("authored-cloud-result"), view.Slot, 32, 16),
		32,
		16,
		{0.f, 4.f, 0.5f, 1.f}
	));
}

TEST_CASE(
	"output composition skips cleared UI but retains drawn and authored interfaces",
	"[render][gpu][inactive-output]"
) {
	for (const auto &[authoredInterface, copyOverlay] : std::array{
			 std::pair{false, false}, std::pair{true, false}, std::pair{false, true}, std::pair{true, true}
		 }) {
		CAPTURE(authoredInterface, copyOverlay);
		FixtureDevice fixture;
		fixture.Initialise();
		graph::PipelineDocument document;
		for (const char *name :
			 {"output-authored-source",
			  "output-source",
			  "output-interface",
			  "output-unused-interface",
			  "output-composed",
			  "output-presented"})
			document.Record(
				{.Kind = graph::EditKind::AddResource,
				 .Name = core::Name(name),
				 .Resource = graph::ResourceKind::Colour,
				 .Format = graph::ResourceFormat::RGBA8_SRGB}
			);
		Raster(document, "output-source-pass", copyOverlay ? "output-authored-source" : "output-source");
		if (authoredInterface)
			Raster(document, "authored-interface-pass", "output-interface", R"(#version 450
layout(location=0) out vec4 colour;
void main(){ colour=vec4(0.25,0.0,0.0,0.5); })");
		if (copyOverlay) {
			// Convert authored raster colour once into the native presentation format;
			// the following overlay can then copy without another present node.
			document.Record(
				{.Kind = graph::EditKind::AddNode,
				 .Name = core::Name("output-colour-convert"),
				 .NodeKind = core::Name("present"),
				 .Scope = graph::NodeScope::Frame}
			);
			document.Record(
				{.Kind = graph::EditKind::Reads,
				 .Target = core::Name("output-authored-source"),
				 .Key = core::Name("source")}
			);
			document.Record(
				{.Kind = graph::EditKind::Writes,
				 .Target = core::Name("output-source"),
				 .Key = core::Name("colour")}
			);
		}
		document.Record(
			{.Kind = graph::EditKind::AddNode,
			 .Name = core::Name("output-interface-pass"),
			 .NodeKind = core::Name("interface"),
			 .Scope = graph::NodeScope::Frame}
		);
		document.Record(
			{.Kind = graph::EditKind::Writes,
			 .Target = core::Name(authoredInterface ? "output-unused-interface" : "output-interface"),
			 .Key = core::Name("colour")}
		);

		document.Record(
			{.Kind = graph::EditKind::AddNode,
			 .Name = core::Name("output-overlay-pass"),
			 .NodeKind = core::Name("overlay"),
			 .Scope = graph::NodeScope::Frame}
		);
		document.Record(
			{.Kind = graph::EditKind::Reads,
			 .Target = core::Name("output-source"),
			 .Key = core::Name("scene")}
		);
		document.Record(
			{.Kind = graph::EditKind::Reads,
			 .Target = core::Name("output-interface"),
			 .Key = core::Name("interface")}
		);
		document.Record(
			{.Kind = graph::EditKind::Writes,
			 .Target = core::Name("output-composed"),
			 .Key = core::Name("colour")}
		);
		if (!copyOverlay) {
			document.Record(
				{.Kind = graph::EditKind::AddNode,
				 .Name = core::Name("output-present-pass"),
				 .NodeKind = core::Name("present"),
				 .Scope = graph::NodeScope::Frame}
			);
			document.Record(
				{.Kind = graph::EditKind::Reads,
				 .Target = core::Name("output-composed"),
				 .Key = core::Name("source")}
			);
			document.Record(
				{.Kind = graph::EditKind::Writes,
				 .Target = core::Name("output-presented"),
				 .Key = core::Name("colour")}
			);
		}
		graph::RenderGraph pipeline;
		core::Name offender;
		REQUIRE(graph::Build(document, pipeline, offender) == graph::PipelineDocumentStatus::Ok);
		std::vector<graph::ResourceId> retained;
		for (uint32_t value = 1; value <= pipeline.ResourceCount(); ++value) {
			const graph::ResourceId resource{value};
			const auto name = pipeline.FindResource(resource)->Name;
			if (name == core::Name("output-source") || name == core::Name("output-interface") ||
				name == core::Name("output-composed") ||
				(!copyOverlay && name == core::Name("output-presented")))
				retained.push_back(resource);
		}
		const char *pipelineName = authoredInterface
									   ? (copyOverlay ? "authored-output-copy" : "authored-output-convert")
									   : (copyOverlay ? "inactive-output-copy" : "inactive-output-convert");
		InstallBoundary(fixture.Render, pipeline, pipelineName, retained, graph::NodeScope::Frame);
		SceneTarget target{41, 31};
		View view;
		view.World = 19;
		view.Pipeline = core::Name(pipelineName);
		view.Target = &target;
		OverlayImage overlay;
		const auto copiesBefore = CounterValue("render.identity_copy.commands");
		const auto empty = fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false);
		const auto source = CaptureResource(
			fixture.Render, core::Name("output-source"), view.Slot, 41, 31, ImageFormat::Rgba8Unorm
		);
		const auto composed = CaptureResource(
			fixture.Render, core::Name("output-composed"), view.Slot, 41, 31, ImageFormat::Rgba8Unorm
		);
		CHECK(empty.DrawCalls == (authoredInterface ? 4 : 2));
		CHECK(CounterValue("render.identity_copy.commands") - copiesBefore == 1);
		const auto changedPixels =
			copyOverlay ? Different(source, composed) : NativeConversionMismatch(source, composed);
		if (authoredInterface)
			CHECK(changedPixels > 1);
		else
			CHECK(changedPixels == 0);
		if (!copyOverlay)
			CHECK(
				Different(
					composed,
					CaptureResource(
						fixture.Render,
						core::Name("output-presented"),
						view.Slot,
						41,
						31,
						ImageFormat::Rgba8Unorm
					)
				) == 0
			);
		if (!authoredInterface) {
			SolidInterface interface(fixture.Render);
			CHECK(fixture.Render.Render(std::span(&view, 1), overlay, &interface, false).DrawCalls == 4);
			REQUIRE(interface.Records == 1);
			const auto withGui = CaptureResource(
				fixture.Render, core::Name("output-composed"), view.Slot, 41, 31, ImageFormat::Rgba8Unorm
			);
			CHECK((copyOverlay ? Different(source, withGui) : NativeConversionMismatch(source, withGui)) > 1);
			view.Damage.GameInterface = false;
			interface.Ready = false;
			CHECK(fixture.Render.Render(std::span(&view, 1), overlay, &interface, false).DrawCalls == 3);
			CHECK(interface.Records == 1);
			CHECK(
				Different(
					withGui,
					CaptureResource(
						fixture.Render,
						core::Name("output-composed"),
						view.Slot,
						41,
						31,
						ImageFormat::Rgba8Unorm
					)
				) == 0
			);
		}
	}
}
