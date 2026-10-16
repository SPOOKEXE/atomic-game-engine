#include "RenderFixture.hpp"

#include <engine/computegraph/Reference.hpp>
#include <engine/render/ComputeGraph.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <iterator>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

TEST_SUITE_ID("engine.render.computegraphgpu")
TEST_DEPENDS("engine.computegraph.runtime")
TEST_DEPENDS("engine.computegraph.reference")

namespace {
	namespace cg = engine::computegraph;
	using engine::render::ComputeGraph;
	using engine::render::ComputeGraphProgram;
	using engine::render::ComputeKernelCode;
	constexpr uint32_t GROUP = 64;
	constexpr uint32_t MAXIMUM_INPUT = 1024u * 1024u;
	constexpr uint64_t DEVICE_BUDGET = 256u * 1024u * 1024u;

	cg::Owner Owner() {
		return {"compute-gpu-fixture", 1, 1, UINT32_MAX};
	}
	uint32_t Groups(uint32_t count) {
		return count / GROUP + uint32_t(count % GROUP != 0);
	}
	cg::KernelPort Read(std::string name, uint32_t slot, bool scalar = false) {
		return {.Name = std::move(name), .Binding = slot, .MinimumBytes = scalar ? 4u : 0u};
	}
	cg::KernelPort Write(std::string name, uint32_t slot) {
		return {.Name = std::move(name), .Permission = cg::Access::Write, .Set = 1, .Binding = slot};
	}
	ComputeGraphProgram CompactProgram(uint32_t capacity) {
		ComputeGraphProgram program;
		auto &document = program.Definition;
		const auto buffer = [&](std::string name, uint32_t count) {
			document.Buffers.push_back({.Name = std::move(name), .Capacity = count, .LogicalCount = count});
		};
		const auto error = [&](std::string name, uint32_t work) {
			buffer(name, std::max(1u, Groups(work)));
			program.ErrorBuffers.push_back(std::move(name));
		};
		document.Buffers.push_back(
			{.Name = "input", .Capacity = capacity, .Permission = cg::Access::Read, .External = true}
		);
		const auto scratch = std::max(1u, capacity);
		buffer("flags", scratch);
		buffer("count", 1);
		document.Buffers.push_back({.Name = "packed", .Capacity = capacity, .CountBuffer = "count"});
		document.Buffers.push_back({.Name = "doubled", .Capacity = capacity, .CountBuffer = "count"});
		document.Kernels = {
			{.Name = "classify", .Ports = {Read("Values", 0), Write("Flags", 0)}},
			{.Name = "scan",
			 .Ports = {Read("Values", 0), Write("Prefix", 0), Write("Sums", 1), Write("Errors", 2)}},
			{.Name = "add-offsets",
			 .Ports = {Read("Local", 0), Read("Parent", 1), Write("Prefix", 0), Write("Errors", 1)}},
			{.Name = "scatter",
			 .Ports =
				 {Read("Values", 0),
				  Read("Flags", 1),
				  Read("Prefix", 2),
				  Read("Count", 3, true),
				  Write("Packed", 0),
				  Write("Errors", 1)}},
			{.Name = "double",
			 .Ports = {Read("Values", 0), Read("Count", 1, true), Write("Result", 0), Write("Errors", 1)}},
		};
		program.Kernels = {
			{.Name = "classify", .BuiltIn = "compute-classify.comp"},
			{.Name = "scan", .BuiltIn = "compute-scan.comp"},
			{.Name = "add-offsets", .BuiltIn = "compute-add-offsets.comp"},
			{.Name = "scatter", .BuiltIn = "compute-scatter.comp"},
			{.Name = "double", .BuiltIn = "compute-double.comp"},
		};
		document.Passes.push_back(
			{.Name = "classify",
			 .Kernel = "classify",
			 .DispatchBuffer = "flags",
			 .Bindings = {{"Values", "input"}, {"Flags", "flags"}}}
		);
		std::vector<std::string> prefixes;
		std::vector<uint32_t> extents;
		std::string source = "flags";
		uint32_t extent = scratch;
		for (uint32_t level = 0;; ++level) {
			const auto label = std::to_string(level);
			const auto prefix = "local-" + label;
			const auto sums = Groups(extent) == 1 ? "count" : "sums-" + label;
			const auto errors = "scan-errors-" + label;
			buffer(prefix, extent);
			if (sums != "count") buffer(sums, Groups(extent));
			error(errors, extent);
			document.Passes.push_back(
				{.Name = "scan-" + label,
				 .Kernel = "scan",
				 .DispatchBuffer = source,
				 .Bindings = {{"Values", source}, {"Prefix", prefix}, {"Sums", sums}, {"Errors", errors}}}
			);
			prefixes.push_back(prefix);
			extents.push_back(extent);
			if (Groups(extent) == 1) break;
			source = sums;
			extent = Groups(extent);
		}
		std::string global = prefixes.back();
		for (size_t level = prefixes.size() - 1; level > 0;) {
			--level;
			const auto label = std::to_string(level);
			const auto next = "global-" + label;
			const auto errors = "add-errors-" + label;
			buffer(next, extents[level]);
			error(errors, extents[level]);
			document.Passes.push_back(
				{.Name = "add-" + label,
				 .Kernel = "add-offsets",
				 .DispatchBuffer = prefixes[level],
				 .Bindings = {
					 {"Local", prefixes[level]}, {"Parent", global}, {"Prefix", next}, {"Errors", errors}
				 }}
			);
			global = next;
		}
		error("scatter-errors", scratch);
		error("double-errors", capacity);
		document.Passes.push_back(
			{.Name = "scatter",
			 .Kernel = "scatter",
			 .DispatchBuffer = "flags",
			 .Bindings = {
				 {"Values", "input"},
				 {"Flags", "flags"},
				 {"Prefix", global},
				 {"Count", "count"},
				 {"Packed", "packed"},
				 {"Errors", "scatter-errors"}
			 }}
		);
		document.Passes.push_back(
			{.Name = "double",
			 .Kernel = "double",
			 .DispatchBuffer = "packed",
			 .Bindings = {
				 {"Values", "packed"}, {"Count", "count"}, {"Result", "doubled"}, {"Errors", "double-errors"}
			 }}
		);
		return program;
	}

	cg::Request Request(std::span<const uint32_t> values, uint32_t maximumResult = MAXIMUM_INPUT) {
		cg::Upload upload{.Buffer = "input", .Count = static_cast<uint32_t>(values.size())};
		upload.Bytes.resize(values.size_bytes());
		if (!values.empty()) std::memcpy(upload.Bytes.data(), values.data(), values.size_bytes());
		return {.Identity = Owner(), .Inputs = {std::move(upload)}, .Outputs = {{"doubled", maximumResult}}};
	}
	cg::Completion Finish(ComputeGraph &graph, uint64_t ticket) {
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
		cg::Diagnostic diagnostic;
		cg::Completion completion;
		while (std::chrono::steady_clock::now() < deadline) {
			graph.Poll();
			const auto status = graph.Take(ticket, completion, diagnostic);
			if (status == cg::Status::Ok) return completion;
			INFO(graph.LastError());
			REQUIRE(status == cg::Status::NotReady);
			SDL_Delay(1);
		}
		FAIL("compute graph fence or result did not complete within the fixture deadline");
		return {};
	}
	std::vector<uint32_t> Values(const cg::Completion &completion) {
		REQUIRE(completion.Code == cg::Status::Ok);
		REQUIRE(completion.Outputs.size() == 1);
		const auto &buffer = completion.Outputs.front();
		REQUIRE(buffer.Type == cg::ElementType::U32);
		REQUIRE(buffer.Bytes.size() == uint64_t(buffer.Count) * 4);
		std::vector<uint32_t> result(buffer.Count);
		if (!result.empty()) std::memcpy(result.data(), buffer.Bytes.data(), buffer.Bytes.size());
		return result;
	}
	void Drain(ComputeGraph &graph) {
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
		while (graph.PendingGpuCount() && std::chrono::steady_clock::now() < deadline) {
			graph.Poll();
			if (graph.PendingGpuCount()) SDL_Delay(1);
		}
		REQUIRE(graph.PendingGpuCount() == 0);
	}
}

TEST_CASE(
	"real typed compute compacts through hierarchical scans into a guarded GPU consumer",
	"[render][computegraph][gpu]"
) {
	engine::render::test::FixtureDevice fixture;
	fixture.Initialise();
	ComputeGraph graph(fixture.Render.Backend().Device, Owner());
	cg::Diagnostic diagnostic;
	REQUIRE(graph.Install(CompactProgram(MAXIMUM_INPUT), diagnostic) == cg::Status::Ok);
	for (const uint32_t count : {0u, 1u, 65u, 4097u, 65537u, MAXIMUM_INPUT, 257u, 0u}) {
		CAPTURE(count);
		std::vector<uint32_t> input(count);
		for (uint32_t index = 0; index < count; ++index)
			input[index] = index % 3 == 0 ? 0 : index + 1;
		std::vector<uint32_t> expected;
		REQUIRE(
			cg::CompactDoubleU32(input, MAXIMUM_INPUT, MAXIMUM_INPUT, expected, diagnostic) == cg::Status::Ok
		);
		uint64_t ticket = 0;
		REQUIRE(graph.Submit(Request(input), ticket, diagnostic) == cg::Status::Ok);
		cg::Completion pending;
		CHECK(graph.Take(ticket, pending, diagnostic) == cg::Status::NotReady);
		const auto actual = Values(Finish(graph, ticket));
		REQUIRE(actual.size() == expected.size());
		const auto difference = std::mismatch(actual.begin(), actual.end(), expected.begin());
		if (difference.first != actual.end()) {
			INFO("first differing output " << std::distance(actual.begin(), difference.first));
			INFO("actual " << *difference.first << ", expected " << *difference.second);
			CHECK(difference.first == actual.end());
		}
		CHECK(graph.ReservedDeviceBytes() == 0);
	}
	std::vector<uint32_t> zeros(4097, 0);
	uint64_t ticket = 0;
	REQUIRE(graph.Submit(Request(zeros), ticket, diagnostic) == cg::Status::Ok);
	CHECK(Values(Finish(graph, ticket)).empty());
	std::vector<uint32_t> oversized(MAXIMUM_INPUT + 1, 1);
	ticket = 91;
	CHECK(graph.Submit(Request(oversized), ticket, diagnostic) == cg::Status::InvalidCount);
	CHECK(ticket == 91);
	CHECK(graph.ReservedDeviceBytes() == 0);
}

TEST_CASE(
	"zero authored capacity still produces a real zero GPU count with legal sentinels",
	"[render][computegraph][gpu]"
) {
	engine::render::test::FixtureDevice fixture;
	fixture.Initialise();
	ComputeGraph graph(fixture.Render.Backend().Device, Owner());
	cg::Diagnostic diagnostic;
	REQUIRE(graph.Install(CompactProgram(0), diagnostic) == cg::Status::Ok);
	uint64_t ticket = 0;
	REQUIRE(graph.Submit(Request({}, 0), ticket, diagnostic) == cg::Status::Ok);
	CHECK(Values(Finish(graph, ticket)).empty());
}

TEST_CASE(
	"real compute refuses diagnostic overflow and bounded readback without publishing partial output",
	"[render][computegraph][gpu]"
) {
	engine::render::test::FixtureDevice fixture;
	fixture.Initialise();
	ComputeGraph graph(fixture.Render.Backend().Device, Owner());
	cg::Diagnostic diagnostic;
	REQUIRE(graph.Install(CompactProgram(257), diagnostic) == cg::Status::Ok);
	for (const auto &input : {std::vector<uint32_t>{3, 0, 7, 0, 9}, std::vector<uint32_t>{UINT32_MAX}}) {
		uint64_t ticket = 0;
		REQUIRE(graph.Submit(Request(input, 1), ticket, diagnostic) == cg::Status::Ok);
		const auto completion = Finish(graph, ticket);
		CHECK(completion.Code == cg::Status::OutputOverflow);
		CHECK(completion.Outputs.empty());
	}
}

TEST_CASE(
	"compute device reservations include canceled in-flight jobs and outside renderer buffers",
	"[render][computegraph][gpu]"
) {
	engine::render::test::FixtureDevice fixture;
	fixture.Initialise();
	ComputeGraph graph(fixture.Render.Backend().Device, Owner());
	cg::Diagnostic diagnostic;
	REQUIRE(graph.Install(CompactProgram(257), diagnostic) == cg::Status::Ok);
	const std::vector<uint32_t> input{3, 0, 7};
	REQUIRE(graph.SetOutsideDeviceBytes(DEVICE_BUDGET));
	uint64_t ticket = 91;
	CHECK(graph.Submit(Request(input, 257), ticket, diagnostic) == cg::Status::LimitExceeded);
	CHECK(ticket == 91);
	REQUIRE(graph.SetOutsideDeviceBytes(0));
	REQUIRE(graph.Submit(Request(input, 257), ticket, diagnostic) == cg::Status::Ok);
	const auto reserved = graph.ReservedDeviceBytes();
	REQUIRE(reserved > 0);
	CHECK_FALSE(graph.SetOutsideDeviceBytes(DEVICE_BUDGET));
	REQUIRE(graph.SetOutsideDeviceBytes(DEVICE_BUDGET - reserved * 2 + 1));
	REQUIRE(graph.Cancel(ticket));
	CHECK(graph.ReservedDeviceBytes() == reserved);
	CHECK(graph.PendingGpuCount() == 1);
	uint64_t refused = 91;
	CHECK(graph.Submit(Request(input, 257), refused, diagnostic) == cg::Status::LimitExceeded);
	CHECK(refused == 91);
	Drain(graph);
	CHECK(graph.ReservedDeviceBytes() == 0);
	cg::Completion canceled;
	CHECK(graph.Take(ticket, canceled, diagnostic) == cg::Status::UnknownTicket);
	REQUIRE(graph.Submit(Request(input, 257), ticket, diagnostic) == cg::Status::Ok);
	CHECK(Values(Finish(graph, ticket)) == std::vector<uint32_t>{6, 14});
}

TEST_CASE(
	"compute executable reflection refuses authored lies and preserves the installed kernel package",
	"[render][computegraph][gpu]"
) {
	engine::render::test::FixtureDevice fixture;
	fixture.Initialise();
	ComputeGraph graph(fixture.Render.Backend().Device, Owner());
	cg::Diagnostic diagnostic;
	const auto valid = CompactProgram(257);
	REQUIRE(graph.Install(valid, diagnostic) == cg::Status::Ok);
	auto wrong = valid;
	wrong.Definition.Kernels[0].Ports[0].Type = cg::ElementType::F32;
	CHECK(graph.Install(wrong, diagnostic) == cg::Status::ReflectionMismatch);
	CHECK_FALSE(graph.LastError().empty());
	wrong = valid;
	wrong.Definition.Kernels[0].Local[0] = 32;
	CHECK(graph.Install(wrong, diagnostic) == cg::Status::ReflectionMismatch);
	wrong = valid;
	wrong.Kernels[0].BuiltIn.clear();
	wrong.Kernels[0].Source = "#version 450\nthis is not a shader";
	CHECK(graph.Install(wrong, diagnostic) == cg::Status::ReflectionMismatch);
	CHECK_FALSE(graph.LastError().empty());
	uint64_t ticket = 0;
	REQUIRE(graph.Submit(Request(std::vector<uint32_t>{3, 0, 7}, 257), ticket, diagnostic) == cg::Status::Ok);
	CHECK(Values(Finish(graph, ticket)) == std::vector<uint32_t>{6, 14});
}

TEST_CASE(
	"named authored typed kernels compile real GLSL and execute on the renderer device",
	"[render][computegraph][gpu]"
) {
	engine::render::test::FixtureDevice fixture;
	fixture.Initialise();
	ComputeGraph graph(fixture.Render.Backend().Device, Owner());
	for (const auto type : {cg::ElementType::U32, cg::ElementType::F32, cg::ElementType::F32x4}) {
		CAPTURE(static_cast<int>(type));
		const auto stride = cg::ElementStride(type);
		ComputeGraphProgram program;
		program.Definition.Buffers = {
			{.Name = "input",
			 .Type = type,
			 .Stride = stride,
			 .Capacity = 1,
			 .LogicalCount = 1,
			 .Permission = cg::Access::Read,
			 .External = true},
			{.Name = "output", .Type = type, .Stride = stride, .Capacity = 1, .LogicalCount = 1},
		};
		program.Definition.Kernels = {
			{.Name = "authored",
			 .Ports = {
				 {.Name = "Values", .Type = type, .Stride = stride},
				 {.Name = "Result",
				  .Type = type,
				  .Stride = stride,
				  .Permission = cg::Access::Write,
				  .Set = 1},
			 }}
		};
		program.Definition.Passes = {
			{.Name = "invoke",
			 .Kernel = "authored",
			 .DispatchBuffer = "input",
			 .Bindings = {{"Values", "input"}, {"Result", "output"}}}
		};
		const std::string scalar = type == cg::ElementType::U32	  ? "uint"
								   : type == cg::ElementType::F32 ? "float"
																  : "vec4";
		const std::string increment = type == cg::ElementType::U32	 ? "11u"
									  : type == cg::ElementType::F32 ? "11.0"
																	 : "vec4(11.0)";
		program.Kernels = {
			{.Name = "authored",
			 .Source = "#version 450\nlayout(local_size_x=64) in;\n"
					   "layout(set=0,binding=0,std430) readonly buffer Values { " +
					   scalar +
					   " Data[]; } values;\n"
					   "layout(set=1,binding=0,std430) writeonly buffer Result { " +
					   scalar +
					   " Data[]; } result;\n"
					   "layout(set=2,binding=0,std140) uniform ComputeParameters { uvec4 Dispatch; uvec4 "
					   "Buffers[32]; } parameters;\n"
					   "void main() { uint i=gl_GlobalInvocationID.x; if(i<parameters.Dispatch.x && "
					   "i<parameters.Buffers[0].x "
					   "&& i<parameters.Buffers[16].y) result.Data[i]=values.Data[i]+" +
					   increment + "; }\n"}
		};
		cg::Diagnostic diagnostic;
		INFO(graph.LastError());
		REQUIRE(graph.Install(program, diagnostic) == cg::Status::Ok);
		cg::Request request{
			.Identity = Owner(),
			.Inputs = {{.Buffer = "input", .Type = type, .Count = 1}},
			.Outputs = {{"output", 1}}
		};
		request.Inputs[0].Bytes.resize(stride);
		if (type == cg::ElementType::U32) {
			const uint32_t input = 31;
			std::memcpy(request.Inputs[0].Bytes.data(), &input, 4);
		} else {
			for (uint32_t offset = 0; offset < stride; offset += 4) {
				const float input = 31.0f;
				std::memcpy(request.Inputs[0].Bytes.data() + offset, &input, 4);
			}
		}
		uint64_t ticket = 0;
		REQUIRE(graph.Submit(request, ticket, diagnostic) == cg::Status::Ok);
		const auto completion = Finish(graph, ticket);
		REQUIRE(completion.Code == cg::Status::Ok);
		REQUIRE(completion.Outputs.size() == 1);
		const auto &bytes = completion.Outputs[0].Bytes;
		REQUIRE(bytes.size() == stride);
		if (type == cg::ElementType::U32) {
			uint32_t output = 0;
			std::memcpy(&output, bytes.data(), 4);
			CHECK(output == 42);
		} else {
			for (uint32_t offset = 0; offset < stride; offset += 4) {
				float output = 0;
				std::memcpy(&output, bytes.data() + offset, 4);
				CHECK(output == 42.0f);
			}
		}
	}
}

TEST_CASE(
	"the reserved readonly instance input is copied from an actual owner-supplied GPU buffer",
	"[render][computegraph][gpu]"
) {
	using namespace engine::render;
	engine::render::test::FixtureDevice fixture;
	fixture.Initialise();
	auto *device = static_cast<SDL_GPUDevice *>(fixture.Render.Backend().Device);
	const auto releaseBuffer = [device](SDL_GPUBuffer *buffer) { gpu::ReleaseBuffer(device, buffer); };
	const auto releaseTransfer = [device](SDL_GPUTransferBuffer *buffer) {
		gpu::ReleaseTransferBuffer(device, buffer);
	};
	SDL_GPUBufferCreateInfo info{};
	info.usage = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ;
	info.size = 16;
	std::unique_ptr<SDL_GPUBuffer, decltype(releaseBuffer)> instance(
		gpu::CreateBuffer(device, &info), releaseBuffer
	);
	SDL_GPUTransferBufferCreateInfo uploadInfo{};
	uploadInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
	uploadInfo.size = 16;
	std::unique_ptr<SDL_GPUTransferBuffer, decltype(releaseTransfer)> transfer(
		gpu::CreateTransferBuffer(device, &uploadInfo), releaseTransfer
	);
	REQUIRE(instance);
	REQUIRE(transfer);
	const std::array<uint32_t, 4> source{31, 0, 0, 0};
	void *mapped = SDL_MapGPUTransferBuffer(device, transfer.get(), false);
	REQUIRE(mapped);
	std::memcpy(mapped, source.data(), sizeof(source));
	SDL_UnmapGPUTransferBuffer(device, transfer.get());
	auto *command = SDL_AcquireGPUCommandBuffer(device);
	REQUIRE(command);
	auto *copy = SDL_BeginGPUCopyPass(command);
	REQUIRE(copy);
	const SDL_GPUTransferBufferLocation upload{transfer.get(), 0};
	const SDL_GPUBufferRegion target{instance.get(), 0, 16};
	SDL_UploadToGPUBuffer(copy, &upload, &target, false);
	SDL_EndGPUCopyPass(copy);
	REQUIRE(SDL_SubmitGPUCommandBuffer(command));
	ComputeGraphProgram program;
	program.Definition.Buffers = {
		{.Name = "engineInstanceBuffer",
		 .Type = cg::ElementType::EngineInstance,
		 .Stride = 16,
		 .Capacity = 1,
		 .LogicalCount = 1,
		 .Permission = cg::Access::Read,
		 .External = true},
		{.Name = "output", .Capacity = 1, .LogicalCount = 1},
	};
	program.Definition.Kernels = {
		{.Name = "instance-read",
		 .Ports = {
			 {.Name = "engineInstanceBuffer", .Type = cg::ElementType::EngineInstance, .Stride = 16},
			 Write("Result", 0),
		 }}
	};
	program.Definition.Passes = {
		{.Name = "read-instance",
		 .Kernel = "instance-read",
		 .DispatchBuffer = "engineInstanceBuffer",
		 .Bindings = {{"engineInstanceBuffer", "engineInstanceBuffer"}, {"Result", "output"}}}
	};
	program.Kernels = {
		{.Name = "instance-read",
		 .Source = "#version 450\nlayout(local_size_x=64) in;\nstruct InstanceRow { uvec4 Words; };\n"
				   "layout(set=0,binding=0,std430) readonly buffer engineInstanceBuffer { InstanceRow "
				   "Data[]; } instances;\n"
				   "layout(set=1,binding=0,std430) writeonly buffer Result { uint Data[]; } result;\n"
				   "void main() { if(gl_GlobalInvocationID.x==0u) "
				   "result.Data[0]=instances.Data[0].Words.x+11u; }\n"}
	};
	ComputeGraph graph(device, Owner());
	cg::Diagnostic diagnostic;
	REQUIRE(graph.Install(program, diagnostic) == cg::Status::Ok);
	REQUIRE(graph.SetOutsideDeviceBytes(16));
	REQUIRE(graph.SetInstanceBuffer(instance.get(), 1, 16));
	cg::Request request{.Identity = Owner(), .Outputs = {{"output", 1}}};
	uint64_t ticket = 0;
	REQUIRE(graph.Submit(request, ticket, diagnostic) == cg::Status::Ok);
	CHECK_FALSE(graph.SetInstanceBuffer(nullptr, 0, 0));
	CHECK(Values(Finish(graph, ticket)) == std::vector<uint32_t>{42});
	CHECK(graph.SetInstanceBuffer(nullptr, 0, 0));
}
