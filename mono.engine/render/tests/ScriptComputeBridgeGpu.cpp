#include "RenderFixture.hpp"
#include "ScriptComputeBridge.hpp"

#include <engine/computegraph/Stock.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/examples/DemosLoader.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scripthost/Runtime.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iterator>
#include <thread>
#include <utility>
#include <vector>

TEST_SUITE_ID("engine.render.scriptcomputebridgegpu")
TEST_DEPENDS("engine.computegraph.stock")
TEST_DEPENDS("engine.scripthost.computegraphservice")

namespace {
	namespace cg = engine::computegraph;
	using engine::script::ComputeBridge;
	engine::script::ComputePoll
	Finish(engine::render::Renderer &render, ComputeBridge &endpoint, uint64_t ticket) {
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
		engine::script::ComputePoll reply;
		while (std::chrono::steady_clock::now() < deadline) {
			render.PumpCompute();
			REQUIRE(endpoint.Poll(ticket, reply) == cg::Status::Ok);
			if (reply.Ready) return reply;
			SDL_Delay(1);
		}
		FAIL("renderer compute bridge did not complete within its fixture deadline");
		return {};
	}
	engine::script::ComputePoll
	Finish(engine::render::ScriptComputeBridge &pool, ComputeBridge &endpoint, uint64_t ticket) {
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
		engine::script::ComputePoll reply;
		while (std::chrono::steady_clock::now() < deadline) {
			pool.Pump(0);
			REQUIRE(endpoint.Poll(ticket, reply) == cg::Status::Ok);
			if (reply.Ready) return reply;
			SDL_Delay(1);
		}
		FAIL("shared compute bridge did not complete within its fixture deadline");
		return {};
	}
	cg::Program CopyProgram(uint32_t capacity) {
		cg::Program program;
		program.Definition.Buffers = {
			{.Name = "input", .Capacity = capacity, .Permission = cg::Access::Read, .External = true},
			{.Name = "output", .Capacity = capacity, .LogicalCount = capacity}
		};
		program.Definition.Kernels = {
			{.Name = "copy",
			 .Ports = {{.Name = "Input"}, {.Name = "Output", .Permission = cg::Access::Write, .Set = 1}}}
		};
		program.Definition.Passes = {
			{.Name = "copy",
			 .Kernel = "copy",
			 .DispatchBuffer = "output",
			 .Bindings = {{"Input", "input"}, {"Output", "output"}}}
		};
		program.Kernels = {{.Name = "copy", .Source = R"(#version 450
layout(local_size_x=64) in;
layout(set=0,binding=0,std430) readonly buffer Input { uint Data[]; } inputValues;
layout(set=1,binding=0,std430) writeonly buffer Output { uint Data[]; } outputValues;
void main() { uint i=gl_GlobalInvocationID.x; if (i<inputValues.Data.length() && i<outputValues.Data.length()) outputValues.Data[i]=inputValues.Data[i]; }
)"}};
		return program;
	}
}
TEST_CASE(
	"renderer compute endpoints copy input, isolate owners and retain cancellation budgets",
	"[render][computegraph][gpu]"
) {
	engine::render::test::FixtureDevice fixture;
	fixture.Initialise();
	auto factory = fixture.Render.ComputeForWorld("bridge-world", 3);
	auto first = factory(71, 9), second = factory(72, 9);
	REQUIRE(first);
	REQUIRE(second);
	CHECK_FALSE(engine::render::ScriptComputeBridge::TakeProfile(*first).Enabled);
	REQUIRE(engine::render::ScriptComputeBridge::EnableProfiling(*first));
	CHECK((first->Identity() == cg::Owner{"bridge-world", 71, 9, 3}));
	uint64_t definition = 0;
	std::string detail;
	REQUIRE(first->Define("copy", CopyProgram(65), definition, detail) == cg::Status::Ok);
	engine::script::ComputePoll pending;
	REQUIRE(first->Poll(definition, pending) == cg::Status::Ok);
	CHECK_FALSE(pending.Ready);
	CHECK(second->Poll(definition, pending) == cg::Status::UnknownTicket);
	CHECK(Finish(fixture.Render, *first, definition).Code == cg::Status::Ok);
	REQUIRE(first->Release(definition));
	fixture.Render.PumpCompute();
	cg::Upload upload{.Buffer = "input", .Count = 65};
	upload.Bytes.resize(65 * 4);
	for (uint32_t i = 0; i < 65; ++i)
		std::memcpy(upload.Bytes.data() + i * 4, &i, 4);
	cg::Request request{.Identity = {}, .Inputs = {upload}, .Outputs = {{"output", 65}}};
	uint64_t ticket = 0;
	REQUIRE(first->Submit("copy", request, ticket, detail) == cg::Status::Ok);
	std::fill(request.Inputs.front().Bytes.begin(), request.Inputs.front().Bytes.end(), std::byte{0xff});
	const auto reply = Finish(fixture.Render, *first, ticket);
	REQUIRE(reply.Code == cg::Status::Ok);
	REQUIRE(reply.Outputs.size() == 1);
	CHECK(reply.Outputs.front().Count == 65);
	std::vector<std::byte> bytes;
	REQUIRE(first->Read(ticket, "output", 0, 65536, bytes) == cg::Status::Ok);
	CHECK(bytes == upload.Bytes);
	CHECK(first->Read(ticket, "output", 0, 65537, bytes) == cg::Status::LimitExceeded);
	CHECK(first->Read(ticket, "output", 261, 1, bytes) == cg::Status::InvalidCount);
	CHECK(second->Read(ticket, "output", 0, 1, bytes) == cg::Status::UnknownTicket);
	REQUIRE(first->Release(ticket));
	fixture.Render.PumpCompute();
	const auto profile = engine::render::ScriptComputeBridge::TakeProfile(*first);
	REQUIRE(profile.Enabled);
	CHECK(profile.Definitions == 1);
	CHECK(profile.Submitted == 1);
	CHECK(profile.Completed == 1);
	CHECK(profile.Released == 2);
	CHECK(profile.ReadBytes == 260);
	CHECK(profile.ReadCalls == 1);
	CHECK(profile.CurrentEntries == 0);
	CHECK(profile.CurrentDeviceBytes == 0);
	CHECK(profile.CurrentResultBytes == 0);
	CHECK(profile.PeakUploadBytes >= 3 * 260);
	CHECK(profile.PeakResultBytes == 2 * 260);
	CHECK(profile.DroppedRecords == 0);
	REQUIRE(profile.Jobs.size() == 1);
	const auto &job = profile.Jobs.front();
	CHECK(job.Ticket == ticket);
	CHECK(job.Owner == first->Identity());
	CHECK(job.Released);
	CHECK(job.ReadBytes == 260);
	CHECK(job.SubmitToReleaseNanoseconds >= job.SubmitToReadyNanoseconds);
	if (job.DeviceCountsAvailable) {
		CHECK(job.Device.Ticket != 0);
		CHECK(job.Device.Dispatches == 1);
		CHECK(job.Device.Workgroups == 2);
		CHECK(job.Device.UploadBytes == 260);
		CHECK(job.Device.ReadbackBytes == 260);
	}
	CHECK(engine::render::ScriptComputeBridge::TakeProfile(*first).Jobs.empty());
	request.Inputs.front() = upload;
	REQUIRE(first->Submit("copy", request, ticket, detail) == cg::Status::Ok);
	fixture.Render.PumpCompute();
	REQUIRE(first->Cancel(ticket));
	REQUIRE(first->Release(ticket));
	const auto drainCancelled = [&](uint64_t cancelledTicket) {
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
		bool found = false;
		do {
			fixture.Render.PumpCompute();
			const auto observed = engine::render::ScriptComputeBridge::TakeProfile(*first);
			CHECK(observed.DroppedRecords == 0);
			for (const auto &record : observed.Jobs) {
				CHECK(record.Ticket == cancelledTicket);
				CHECK(record.Cancelled);
				CHECK(record.Released);
				CHECK(record.Retired);
				found = true;
			}
			if (found) break;
			std::this_thread::yield();
		} while (std::chrono::steady_clock::now() < deadline);
		REQUIRE(found);
	};
	drainCancelled(ticket);
	// Alternate queued and device-submitted cancellation beyond the profile's capacity.
	for (size_t cycle = 0; cycle < 20; ++cycle) {
		REQUIRE(first->Submit("copy", request, ticket, detail) == cg::Status::Ok);
		if (cycle % 2) fixture.Render.PumpCompute();
		REQUIRE(first->Cancel(ticket));
		REQUIRE(first->Release(ticket));
		drainCancelled(ticket);
	}
	const auto cancellationTotals = engine::render::ScriptComputeBridge::TakeProfile(*first);
	CHECK(cancellationTotals.Cancelled == 21);
	CHECK(cancellationTotals.RetiredJobs == 22);
	CHECK(cancellationTotals.CurrentEntries == 0);
	CHECK(cancellationTotals.CurrentDeviceBytes == 0);
	CHECK(cancellationTotals.DroppedRecords == 0);
	first->Detach();
	fixture.Render.PumpCompute();
	CHECK(first->Poll(ticket, pending) == cg::Status::StaleOwner);
	uint64_t rejected = 123;
	CHECK(first->Submit("copy", request, rejected, detail) == cg::Status::StaleOwner);
	CHECK(rejected == 123);
}
TEST_CASE(
	"paired BufferCompute examples execute authored GPU kernels through the actual runtime and renderer pump",
	"[render][computegraph][gpu]"
) {
	engine::scene::EnsureClassTree();
	engine::scene::RegisterSceneComponents();
	engine::render::test::FixtureDevice fixture;
	fixture.Initialise();
	const engine::examples::DemosLoader demos;
	for (const auto language : {engine::script::Language::Luau, engine::script::Language::JavaScript}) {
		CAPTURE(static_cast<int>(language));
		const auto path = demos.Resolve(
			engine::examples::DemoKind::Script,
			language == engine::script::Language::Luau ? "BufferCompute.luau" : "BufferCompute.js"
		);
		REQUIRE_FALSE(path.empty());
		std::ifstream file(path);
		REQUIRE(file.good());
		const std::string source((std::istreambuf_iterator<char>(file)), {});
		engine::ecs::Store store(language == engine::script::Language::Luau ? "compute-luau" : "compute-js");
		engine::script::RuntimeLimits limits;
		limits.Role = engine::script::HostRole::OfBoth();
		limits.Compute = fixture.Render.ComputeForWorld(std::string(store.Name()));
		auto runtime = engine::script::MakeRuntime(store, language, limits);
		REQUIRE(runtime);
		INFO(runtime->LastError());
		REQUIRE(runtime->Run(source, path.string()));
		const auto workspace = engine::scene::WorkspaceOf(store);
		REQUIRE(workspace != engine::ecs::NULL_ENTITY);
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
		while (store.FindFirstChild(workspace, "BufferComputeComplete") == engine::ecs::NULL_ENTITY &&
			   std::chrono::steady_clock::now() < deadline) {
			fixture.Render.PumpCompute();
			store.ClearChanges();
			store.AdvanceTick(1.0 / 60.0);
			INFO(runtime->LastError());
			REQUIRE(runtime->Heartbeat(1.0 / 60.0));
			store.FlushSignals();
			SDL_Delay(1);
		}
		CHECK(store.FindFirstChild(workspace, "BufferComputeComplete") != engine::ecs::NULL_ENTITY);
		runtime.reset();
		fixture.Render.PumpCompute();
	}
}

TEST_CASE(
	"one renderer compute inbox accounts for other owners and canceled device reservations",
	"[render][computegraph][gpu]"
) {
	engine::render::test::FixtureDevice fixture;
	fixture.Initialise();
	auto pool = std::make_shared<engine::render::ScriptComputeBridge>(fixture.Render.Backend().Device);
	auto factory = pool->ForWorld("budget-world");
	auto first = factory(81, 1), second = factory(82, 1);
	REQUIRE(first);
	REQUIRE(second);
	REQUIRE(engine::render::ScriptComputeBridge::EnableProfiling(*first));
	std::string detail;
	uint64_t firstDefinition = 0, secondDefinition = 0;
	REQUIRE(first->Define("copy", CopyProgram(65), firstDefinition, detail) == cg::Status::Ok);
	REQUIRE(second->Define("copy", CopyProgram(65), secondDefinition, detail) == cg::Status::Ok);
	pool->Pump(0);
	engine::script::ComputePoll reply;
	REQUIRE(first->Poll(firstDefinition, reply) == cg::Status::Ok);
	REQUIRE(reply.Ready);
	REQUIRE(reply.Code == cg::Status::Ok);
	REQUIRE(second->Poll(secondDefinition, reply) == cg::Status::Ok);
	REQUIRE(reply.Ready);
	REQUIRE(reply.Code == cg::Status::Ok);
	REQUIRE(first->Release(firstDefinition));
	REQUIRE(second->Release(secondDefinition));
	pool->Pump(0);
	cg::Upload upload{.Buffer = "input", .Count = 65};
	upload.Bytes.resize(260);
	cg::Request request{.Identity = {}, .Inputs = {upload}};
	uint64_t firstTicket = 0, secondTicket = 0;
	REQUIRE(first->Submit("copy", request, firstTicket, detail) == cg::Status::Ok);
	REQUIRE(second->Submit("copy", request, secondTicket, detail) == cg::Status::Ok);
	constexpr uint64_t payload = 520, ceiling = 256u * 1024u * 1024u;
	pool->Pump(ceiling - payload);
	CHECK(pool->ReservedDeviceBytes() == payload);
	REQUIRE(second->Poll(secondTicket, reply) == cg::Status::Ok);
	CHECK(reply.Ready);
	CHECK(reply.Code == cg::Status::LimitExceeded);
	CHECK(reply.Outputs.empty());
	REQUIRE(first->Cancel(firstTicket));
	REQUIRE(first->Release(firstTicket));
	REQUIRE(second->Release(secondTicket));
	CHECK(pool->ReservedDeviceBytes() == payload);
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
	while (pool->ReservedDeviceBytes() != 0 && std::chrono::steady_clock::now() < deadline) {
		pool->Pump(ceiling - payload);
		SDL_Delay(1);
	}
	CHECK(pool->ReservedDeviceBytes() == 0);
	const auto budgetProfile = engine::render::ScriptComputeBridge::TakeProfile(*first);
	CHECK(budgetProfile.Refused == 1);
	CHECK(budgetProfile.DroppedRecords == 0);
	REQUIRE(budgetProfile.Jobs.size() == 2);
	for (const auto &job : budgetProfile.Jobs) {
		CHECK(job.Released);
		CHECK(job.Retired);
	}
	first->Detach();
	second->Detach();
	pool->Pump(0);
}

TEST_CASE(
	"script compute bridge retains its exact maximum result charge until release",
	"[render][computegraph][gpu][boundary]"
) {
	constexpr uint32_t maximumResult = 2'097'152;
	constexpr size_t maximumResultBytes = size_t(maximumResult) * sizeof(uint32_t);
	constexpr size_t readChunkBytes = 65'536;
	engine::render::test::FixtureDevice fixture;
	fixture.Initialise();
	auto pool = std::make_shared<engine::render::ScriptComputeBridge>(fixture.Render.Backend().Device);
	auto factory = pool->ForWorld("result-budget-world");
	auto first = factory(91, 1), second = factory(92, 1);
	REQUIRE(first);
	REQUIRE(second);
	std::string detail;
	uint64_t largeDefinition = 0, oversizedDefinition = 0, smallDefinition = 0;
	REQUIRE(first->Define("large", CopyProgram(maximumResult), largeDefinition, detail) == cg::Status::Ok);
	REQUIRE(
		first->Define("oversized", CopyProgram(maximumResult + 1), oversizedDefinition, detail) ==
		cg::Status::Ok
	);
	REQUIRE(second->Define("small", CopyProgram(65), smallDefinition, detail) == cg::Status::Ok);
	pool->Pump(0);
	engine::script::ComputePoll reply;
	for (const auto &[endpoint, definition] : {
			 std::pair{first.get(), largeDefinition},
			 std::pair{first.get(), oversizedDefinition},
			 std::pair{second.get(), smallDefinition},
		 }) {
		REQUIRE(endpoint->Poll(definition, reply) == cg::Status::Ok);
		REQUIRE(reply.Ready);
		REQUIRE(reply.Code == cg::Status::Ok);
	}
	REQUIRE(first->Release(largeDefinition));
	REQUIRE(first->Release(oversizedDefinition));
	REQUIRE(second->Release(smallDefinition));
	pool->Pump(0);
	CHECK(pool->ReservedDeviceBytes() == 0);

	cg::Upload smallUpload{.Buffer = "input", .Count = 65};
	smallUpload.Bytes.resize(65 * sizeof(uint32_t));
	const uint32_t smallValue = 17;
	for (uint32_t index = 0; index < 65; ++index) {
		std::memcpy(
			smallUpload.Bytes.data() + size_t(index) * sizeof(smallValue), &smallValue, sizeof(smallValue)
		);
	}
	cg::Request oversizedRequest{
		.Identity = {}, .Inputs = {smallUpload}, .Outputs = {{"output", maximumResult + 1}}
	};
	const auto reservationBeforeRefusal = pool->ReservedDeviceBytes();
	uint64_t ticket = 123;
	CHECK(first->Submit("oversized", oversizedRequest, ticket, detail) == cg::Status::LimitExceeded);
	CHECK(ticket == 123);
	CHECK(pool->ReservedDeviceBytes() == reservationBeforeRefusal);

	std::vector<std::byte> expected(maximumResultBytes);
	for (uint32_t index = 0; index < maximumResult; ++index) {
		const uint32_t value = index ^ 0xa5a5a5a5u;
		std::memcpy(expected.data() + size_t(index) * sizeof(value), &value, sizeof(value));
	}
	cg::Upload largeUpload{.Buffer = "input", .Count = maximumResult, .Bytes = expected};
	cg::Request largeRequest{
		.Identity = {}, .Inputs = {std::move(largeUpload)}, .Outputs = {{"output", maximumResult}}
	};
	uint64_t largeTicket = 0;
	REQUIRE(first->Submit("large", std::move(largeRequest), largeTicket, detail) == cg::Status::Ok);
	reply = Finish(*pool, *first, largeTicket);
	REQUIRE(reply.Code == cg::Status::Ok);
	REQUIRE(reply.Outputs.size() == 1);
	CHECK(reply.Outputs.front().Count == maximumResult);
	CHECK(reply.Outputs.front().Bytes == maximumResultBytes);
	CHECK(pool->ReservedDeviceBytes() == 0);

	std::vector<std::byte> actual;
	for (size_t offset = 0; offset < maximumResultBytes; offset += readChunkBytes) {
		INFO("output byte offset " << offset);
		REQUIRE(first->Read(largeTicket, "output", offset, readChunkBytes, actual) == cg::Status::Ok);
		REQUIRE(actual.size() == readChunkBytes);
		CHECK(std::memcmp(actual.data(), expected.data() + offset, readChunkBytes) == 0);
	}

	cg::Request smallRequest{.Identity = {}, .Inputs = {smallUpload}, .Outputs = {{"output", 65}}};
	ticket = 321;
	CHECK(second->Submit("small", smallRequest, ticket, detail) == cg::Status::LimitExceeded);
	CHECK(ticket == 321);
	CHECK(pool->ReservedDeviceBytes() == 0);
	REQUIRE(first->Release(largeTicket));
	pool->Pump(0);

	uint64_t smallTicket = 0;
	REQUIRE(second->Submit("small", smallRequest, smallTicket, detail) == cg::Status::Ok);
	reply = Finish(*pool, *second, smallTicket);
	REQUIRE(reply.Code == cg::Status::Ok);
	REQUIRE(reply.Outputs.size() == 1);
	CHECK(reply.Outputs.front().Count == 65);
	CHECK(reply.Outputs.front().Bytes == 65 * sizeof(uint32_t));
	REQUIRE(second->Read(smallTicket, "output", 0, 65 * sizeof(uint32_t), actual) == cg::Status::Ok);
	REQUIRE(actual.size() == 65 * sizeof(uint32_t));
	CHECK(std::memcmp(actual.data(), smallUpload.Bytes.data(), actual.size()) == 0);
	REQUIRE(second->Release(smallTicket));
	pool->Pump(0);
	CHECK(pool->ReservedDeviceBytes() == 0);
	first->Detach();
	second->Detach();
	pool->Pump(0);
}
