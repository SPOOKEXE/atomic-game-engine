#include "Fixture.hpp"

#include <engine/computegraph/Compile.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>

TEST_SUITE_ID("engine.computegraph.compile")
TEST_DEPENDS("engine.computegraph.document")
TEST_DEPENDS("engine.graphcore.compile")

using namespace engine::computegraph;

TEST_CASE(
	"typed compute orders a compacted buffer and device count before its GPU consumer", "[computegraph]"
) {
	auto document = tests::CompactChain();
	// Dependency ownership comes from resources, not authored pass position.
	std::reverse(document.Passes.begin(), document.Passes.end());
	CompiledGraph plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, tests::Reflections(document), {}, {}, plan, diagnostic) == Status::Ok);
	CHECK(plan.Topology.Order == std::vector<uint32_t>{1, 0});
	CHECK(plan.Topology.Waves == std::vector<std::vector<uint32_t>>{{1}, {0}});
	REQUIRE(plan.Barriers.size() == 2);
	CHECK(plan.Barriers[0].Buffer == 1);
	CHECK(plan.Barriers[1].Buffer == 2);
	CHECK(plan.Passes[0].Work.DeviceCount);
	CHECK(plan.Passes[0].Work.Elements == 257);
	CHECK(plan.Passes[0].Work.Groups == std::array<uint32_t, 3>{5, 1, 1});
	CHECK(plan.Allocations[1].CountBuffer == 2);
	CHECK(plan.Bytes == 3 * 257 * 4 + 4);
	document.Buffers[0].Capacity = 1;
	CHECK(plan.Definition.Buffers[0].Capacity == 257);
}

TEST_CASE(
	"empty typed compute retains sentinel allocation and records no invalid dispatch", "[computegraph]"
) {
	const auto document = tests::CompactChain(0, 0);
	CompiledGraph plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, tests::Reflections(document), {}, {}, plan, diagnostic) == Status::Ok);
	CHECK(plan.Bytes == 16);
	for (const auto &pass : plan.Passes)
		CHECK(pass.Work.Groups == std::array<uint32_t, 3>{0, 0, 0});
}

TEST_CASE(
	"checked buffer dispatch covers odd counts and refuses local and device limits transactionally",
	"[computegraph]"
) {
	Dispatch resolved;
	Diagnostic diagnostic;
	REQUIRE(ResolveComputeDispatch(65, {64, 1, 1}, {}, resolved, diagnostic) == Status::Ok);
	CHECK(resolved.Groups == std::array<uint32_t, 3>{2, 1, 1});
	const auto previous = resolved;
	CHECK(ResolveComputeDispatch(UINT32_MAX, {1, 1, 1}, {}, resolved, diagnostic) == Status::InvalidDispatch);
	CHECK(resolved.Groups == previous.Groups);
	CHECK(ResolveComputeDispatch(1, {0, 1, 1}, {}, resolved, diagnostic) == Status::InvalidDispatch);
	CHECK(ResolveComputeDispatch(1, {64, 2, 1}, {}, resolved, diagnostic) == Status::InvalidDispatch);
	CHECK(
		ResolveComputeDispatch(0, {UINT32_MAX, UINT32_MAX, UINT32_MAX}, {}, resolved, diagnostic) ==
		Status::InvalidDispatch
	);
	DispatchLimits wide;
	wide.MaximumLocal = {UINT32_MAX, 1, 1};
	wide.MaximumThreads = UINT32_MAX;
	REQUIRE(ResolveComputeDispatch(UINT32_MAX, {UINT32_MAX, 1, 1}, wide, resolved, diagnostic) == Status::Ok);
	CHECK(resolved.Groups[0] == 1);
}

TEST_CASE(
	"kernel admission compares executable reflection instead of trusting authored metadata", "[computegraph]"
) {
	const auto document = tests::CompactChain();
	const auto &kernel = document.Kernels[0];
	const auto expected = tests::Reflections(document)[0];
	Diagnostic diagnostic;
	auto reflected = expected;
	std::reverse(reflected.Ports.begin(), reflected.Ports.end());
	CHECK(ValidateComputeBindings(kernel, reflected, {}, diagnostic) == Status::Ok);
	for (size_t variant = 0; variant < 8; ++variant) {
		CAPTURE(variant);
		reflected = expected;
		switch (variant) {
		case 0:
			reflected.Version = 2;
			break;
		case 1:
			reflected.Local[0] = 32;
			break;
		case 2:
			reflected.Ports[0].Type = ElementType::F32;
			break;
		case 3:
			reflected.Ports[0].Stride = 16;
			break;
		case 4:
			reflected.Ports[0].Permission = Access::ReadWrite;
			break;
		case 5:
			reflected.Ports[0].Binding = 1;
			break;
		case 6:
			reflected.Ports[0].Name = "missing";
			break;
		case 7:
			reflected.Ports[0].MinimumBytes = 16;
			break;
		}
		CHECK(ValidateComputeBindings(kernel, reflected, {}, diagnostic) == Status::ReflectionMismatch);
	}
}

TEST_CASE(
	"typed compute refuses invalid storage and bindings without replacing a good plan", "[computegraph]"
) {
	const auto baseline = tests::CompactChain();
	CompiledGraph plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(baseline, tests::Reflections(baseline), {}, {}, plan, diagnostic) == Status::Ok);
	for (size_t variant = 0; variant < 11; ++variant) {
		CAPTURE(variant);
		auto document = baseline;
		switch (variant) {
		case 0:
			document.Buffers[0].Stride = 8;
			break;
		case 1:
			document.Buffers[0].LogicalCount = 258;
			break;
		case 2:
			document.Buffers[1].CountBuffer = "missing";
			break;
		case 3:
			document.Buffers[2].Type = ElementType::F32;
			break;
		case 4:
			document.Passes[1].Bindings[1].Buffer = "input";
			break;
		case 5:
			document.Passes[1].Bindings[2].Buffer = "packed";
			break;
		case 6:
			document.Buffers[0].Capacity = UINT32_MAX;
			break;
		case 7:
			document.Buffers[1].Name = "input";
			break;
		case 8:
			document.Passes[1].Kernel = "missing";
			break;
		case 9:
			document.Passes[1].Bindings[0].Port = "missing";
			break;
		case 10:
			document.Buffers[1].External = true;
			break;
		}
		CHECK(Compile(document, tests::Reflections(document), {}, {}, plan, diagnostic) != Status::Ok);
		CHECK(plan.Bytes == 3088);
		CHECK(plan.Definition.Buffers[0].Capacity == 257);
	}
}

TEST_CASE(
	"typed compute detects missing producers multiple writers cycles and payload limits", "[computegraph]"
) {
	Diagnostic diagnostic;
	CompiledGraph plan;
	auto document = tests::CompactChain();
	document.Passes.erase(document.Passes.begin());
	CHECK(
		Compile(document, tests::Reflections(document), {}, {}, plan, diagnostic) == Status::MissingProducer
	);
	document = tests::CompactChain();
	document.Passes.push_back(document.Passes.front());
	document.Passes.back().Name = "second-compact";
	CHECK(
		Compile(document, tests::Reflections(document), {}, {}, plan, diagnostic) == Status::MultipleProducers
	);
	document = tests::CompactChain();
	document.Passes[0].Bindings[0].Buffer = "doubled";
	document.Passes[0].DispatchBuffer = "doubled";
	document.Buffers[3].CountBuffer.clear();
	document.Buffers[3].LogicalCount = 257;
	// Compaction now reads the consuming pass's output, closing a real resource cycle.
	CHECK(Compile(document, tests::Reflections(document), {}, {}, plan, diagnostic) == Status::Cycle);
	document = tests::CompactChain();
	Limits limits;
	limits.MaximumBufferBytes = 1024;
	CHECK(
		Compile(document, tests::Reflections(document), limits, {}, plan, diagnostic) == Status::LimitExceeded
	);
	limits = {};
	limits.MaximumTotalBytes = 3000;
	CHECK(
		Compile(document, tests::Reflections(document), limits, {}, plan, diagnostic) == Status::LimitExceeded
	);
	limits = {};
	limits.MaximumBindings = 5;
	CHECK(
		Compile(document, tests::Reflections(document), limits, {}, plan, diagnostic) == Status::LimitExceeded
	);
}

TEST_CASE("compute payload ceilings cannot be widened and include empty sentinels", "[computegraph]") {
	Document document;
	document.Buffers.push_back({
		.Name = "full",
		.Capacity = 16u * 1024u * 1024u,
		.Permission = Access::Read,
		.External = true,
	});
	Limits limits;
	limits.MaximumBufferBytes = UINT64_MAX;
	limits.MaximumTotalBytes = UINT64_MAX;
	CompiledGraph plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, {}, limits, {}, plan, diagnostic) == Status::Ok);
	CHECK(plan.Bytes == 64u * 1024u * 1024u);
	document.Buffers[0].Capacity++;
	CHECK(Compile(document, {}, limits, {}, plan, diagnostic) == Status::LimitExceeded);
	document.Buffers[0].Capacity--;
	for (const char *name : {"second", "third", "fourth"}) {
		auto buffer = document.Buffers[0];
		buffer.Name = name;
		document.Buffers.push_back(buffer);
	}
	REQUIRE(Compile(document, {}, limits, {}, plan, diagnostic) == Status::Ok);
	CHECK(plan.Bytes == 256u * 1024u * 1024u);
	document.Buffers.push_back({.Name = "empty", .Permission = Access::Read, .External = true});
	CHECK(Compile(document, {}, limits, {}, plan, diagnostic) == Status::LimitExceeded);
	CHECK(plan.Bytes == 256u * 1024u * 1024u);
}
