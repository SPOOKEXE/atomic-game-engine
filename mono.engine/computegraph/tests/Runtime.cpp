#include "Fixture.hpp"

#include <engine/computegraph/Runtime.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.computegraph.runtime")
TEST_DEPENDS("engine.computegraph.compile")

using namespace engine::computegraph;

namespace {
	Owner HostOwner() {
		return {"compute-world", 1, 1, UINT32_MAX};
	}
	Request InputRequest() {
		return {
			.Identity = HostOwner(),
			.Inputs = {{.Buffer = "input", .Count = 5, .Bytes = std::vector<std::byte>(20)}},
			.Outputs = {{"doubled", 5}},
		};
	}
	CompiledGraph AcceptedPlan() {
		const auto document = tests::CompactChain();
		CompiledGraph plan;
		Diagnostic diagnostic;
		REQUIRE(Compile(document, tests::Reflections(document), {}, {}, plan, diagnostic) == Status::Ok);
		return plan;
	}
	Completion Finished(uint64_t ticket) {
		return {
			.Ticket = ticket,
			.Identity = HostOwner(),
			.Outputs = {{.Buffer = "doubled", .Count = 3, .Bytes = std::vector<std::byte>(12)}},
		};
	}
}

TEST_CASE(
	"compute submissions own input bytes and counts until explicit completion and take",
	"[computegraph][runtime]"
) {
	auto plan = AcceptedPlan();
	auto request = InputRequest();
	Queue queue(HostOwner());
	Diagnostic diagnostic;
	uint64_t ticket = 0;
	REQUIRE(queue.Submit(plan, request, ticket, diagnostic) == Status::Ok);
	REQUIRE(ticket != 0);
	REQUIRE(queue.Submissions().size() == 1);
	CHECK(queue.Submissions()[0].Plan.Definition.Buffers[0].LogicalCount == 5);
	CHECK(queue.Submissions()[0].Plan.Passes[0].Work.Groups[0] == 1);
	CHECK(queue.Submissions()[0].Plan.Passes[1].Work.DeviceCount);
	CHECK(queue.Submissions()[0].Plan.Passes[1].Work.Elements == 257);
	CHECK(plan.Definition.Buffers[0].LogicalCount == 257);
	request.Inputs[0].Bytes[0] = std::byte{1};
	CHECK(queue.Submissions()[0].Data.Inputs[0].Bytes[0] == std::byte{0});
	Completion result;
	CHECK(queue.Take(ticket, result, diagnostic) == Status::NotReady);
	auto completed = Finished(ticket);
	REQUIRE(queue.Complete(completed, diagnostic) == Status::Ok);
	completed.Outputs[0].Bytes[0] = std::byte{1};
	REQUIRE(queue.Take(ticket, result, diagnostic) == Status::Ok);
	CHECK(result.Outputs[0].Count == 3);
	CHECK(result.Outputs[0].Bytes[0] == std::byte{0});
	CHECK(queue.Submissions().empty());
	CHECK(queue.Take(ticket, result, diagnostic) == Status::UnknownTicket);
}

TEST_CASE(
	"compute request admission enforces independent pending upload and output bounds",
	"[computegraph][runtime]"
) {
	const auto plan = AcceptedPlan();
	Diagnostic diagnostic;
	for (const size_t variant : {0u, 1u, 2u, 3u}) {
		CAPTURE(variant);
		RuntimeLimits limits;
		switch (variant) {
		case 0:
			limits.MaximumPending = 0;
			break;
		case 1:
			limits.MaximumUploadBytes = 19;
			break;
		case 2:
			limits.MaximumReadbackBytes = 19;
			break;
		case 3:
			limits.MaximumResultBytes = 19;
			break;
		}
		Queue queue(HostOwner(), limits);
		uint64_t ticket = 91;
		CHECK(queue.Submit(plan, InputRequest(), ticket, diagnostic) == Status::LimitExceeded);
		CHECK(ticket == 91);
		CHECK(queue.Submissions().empty());
	}
	RuntimeLimits limits;
	limits.MaximumUploadBytes = 20;
	limits.MaximumReadbackBytes = 20;
	limits.MaximumResultBytes = 20;
	Queue queue(HostOwner(), limits);
	uint64_t ticket = 0, refused = 91;
	REQUIRE(queue.Submit(plan, InputRequest(), ticket, diagnostic) == Status::Ok);
	REQUIRE(queue.Complete(Finished(ticket), diagnostic) == Status::Ok);
	CHECK(queue.Submit(plan, InputRequest(), refused, diagnostic) == Status::LimitExceeded);
	CHECK(refused == 91);
	CHECK(queue.Cancel(ticket));
	CHECK(queue.Submit(plan, InputRequest(), refused, diagnostic) == Status::Ok);
}

TEST_CASE(
	"compute completion checks owner count type bytes and terminal device failures transactionally",
	"[computegraph][runtime]"
) {
	Queue queue(HostOwner());
	Diagnostic diagnostic;
	uint64_t ticket = 0;
	REQUIRE(queue.Submit(AcceptedPlan(), InputRequest(), ticket, diagnostic) == Status::Ok);
	for (size_t variant = 0; variant < 6; ++variant) {
		CAPTURE(variant);
		auto result = Finished(ticket);
		switch (variant) {
		case 0:
			result.Identity.World = "another-world";
			break;
		case 1:
			result.Identity.Generation++;
			break;
		case 2:
			result.Outputs[0].Type = ElementType::F32;
			break;
		case 3:
			result.Outputs[0].Count = 6;
			result.Outputs[0].Bytes.resize(24);
			break;
		case 4:
			result.Outputs[0].Bytes.resize(8);
			break;
		case 5:
			result.Code = Status::OutputOverflow;
			break;
		}
		CHECK(queue.Complete(result, diagnostic) != Status::Ok);
		CHECK_FALSE(queue.Submissions()[0].Ready);
	}
	auto failed = Finished(ticket);
	failed.Code = Status::OutputOverflow;
	failed.Outputs.clear();
	REQUIRE(queue.Complete(failed, diagnostic) == Status::Ok);
	Completion output;
	REQUIRE(queue.Take(ticket, output, diagnostic) == Status::Ok);
	CHECK(output.Code == Status::OutputOverflow);
	CHECK(output.Outputs.empty());
}

TEST_CASE(
	"compute cancellation invalidates stale generations and bound view ownership", "[computegraph][runtime]"
) {
	Queue queue(HostOwner());
	Diagnostic diagnostic;
	uint64_t ticket = 0;
	REQUIRE(queue.Submit(AcceptedPlan(), InputRequest(), ticket, diagnostic) == Status::Ok);
	auto next = HostOwner();
	next.Generation++;
	queue.Reset(next);
	CHECK(queue.Complete(Finished(ticket), diagnostic) == Status::StaleOwner);
	CHECK(queue.Submissions().empty());
	CHECK_FALSE(queue.Cancel(ticket));
	auto document = tests::CompactChain();
	for (auto &buffer : document.Buffers) {
		buffer.Retention = Lifetime::View;
		buffer.Owner = HostOwner().World;
	}
	CompiledGraph plan;
	REQUIRE(Compile(document, tests::Reflections(document), {}, {}, plan, diagnostic) == Status::Ok);
	Queue noView(HostOwner());
	CHECK(noView.Submit(plan, InputRequest(), ticket, diagnostic) == Status::StaleOwner);
	auto viewOwner = HostOwner();
	viewOwner.View = 4;
	auto viewRequest = InputRequest();
	viewRequest.Identity = viewOwner;
	Queue viewQueue(viewOwner);
	CHECK(viewQueue.Submit(plan, viewRequest, ticket, diagnostic) == Status::Ok);
}

TEST_CASE(
	"compute input contracts and per-request dispatch refusal preserve pending work",
	"[computegraph][runtime]"
) {
	Queue queue(HostOwner());
	const auto plan = AcceptedPlan();
	Diagnostic diagnostic;
	for (size_t variant = 0; variant < 6; ++variant) {
		CAPTURE(variant);
		auto request = InputRequest();
		switch (variant) {
		case 0:
			request.Inputs.clear();
			break;
		case 1:
			request.Inputs[0].Type = ElementType::F32;
			break;
		case 2:
			request.Inputs[0].Count = 258;
			break;
		case 3:
			request.Inputs[0].Bytes.resize(19);
			break;
		case 4:
			request.Inputs.push_back(request.Inputs[0]);
			break;
		case 5:
			request.Identity.Runtime++;
			break;
		}
		uint64_t ticket = 91;
		CHECK(queue.Submit(plan, request, ticket, diagnostic) != Status::Ok);
		CHECK(ticket == 91);
		CHECK(queue.Submissions().empty());
	}
	const auto document = tests::CompactChain(257, 1);
	CompiledGraph bounded;
	DispatchLimits dispatch;
	dispatch.MaximumGroups[0] = 1;
	// The consuming pass uses capacity, so narrow that buffer for this dispatch test.
	auto narrow = document;
	narrow.Buffers[1].Capacity = 64;
	narrow.Buffers[3].Capacity = 64;
	REQUIRE(Compile(narrow, tests::Reflections(narrow), {}, dispatch, bounded, diagnostic) == Status::Ok);
	auto request = InputRequest();
	request.Inputs[0].Count = 65;
	request.Inputs[0].Bytes.resize(65 * 4);
	uint64_t ticket = 91;
	CHECK(queue.Submit(bounded, request, ticket, diagnostic) == Status::InvalidDispatch);
	CHECK(ticket == 91);
	CHECK(queue.Submissions().empty());
}

TEST_CASE(
	"compute results sharing a device count cannot publish different valid lengths", "[computegraph][runtime]"
) {
	Queue queue(HostOwner());
	auto request = InputRequest();
	request.Outputs.push_back({"packed", 5});
	Diagnostic diagnostic;
	uint64_t ticket = 0;
	REQUIRE(queue.Submit(AcceptedPlan(), request, ticket, diagnostic) == Status::Ok);
	auto completion = Finished(ticket);
	completion.Outputs.push_back({.Buffer = "packed", .Count = 2, .Bytes = std::vector<std::byte>(8)});
	CHECK(queue.Complete(completion, diagnostic) == Status::InvalidCount);
	CHECK_FALSE(queue.Submissions()[0].Ready);
	completion.Outputs[1].Count = 3;
	completion.Outputs[1].Bytes.resize(12);
	CHECK(queue.Complete(completion, diagnostic) == Status::Ok);
}
