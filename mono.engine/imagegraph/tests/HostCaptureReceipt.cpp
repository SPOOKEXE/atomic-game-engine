#include <engine/imagegraph/HostCapture.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>
TEST_SUITE_ID("engine.imagegraph.hostcapturereceipt")
namespace {
	using namespace engine::imagegraph;
	struct Fixture {
		Node Authored{"stage", "pc.hlsl", "", {}, {{"main", std::string("output.color=float4(1,0,0,1);")}}};
		EvaluationRequest Request{.Tick = 7, .Seed = 13};
		std::vector<AuthoredValue> Inputs{{"main", std::string("output.color=float4(1,0,0,1);")}};
		Image Base{1, 1, {10, 20, 30, 255}, 0};
		std::vector<HostResolvedImage> Images{{"base_texture", &Base}};
		HostNodeInvocation Invocation{Authored, Request, Inputs, Images, Limits::MaximumEvaluationBytes};
	};
}
TEST_CASE(
	"resolved host receipt preserves frozen controls and hashes without executing producers",
	"[imagegraph][host-receipt]"
) {
	Fixture fixture;
	HostNodeCapture receipt;
	uint64_t bytes = 0;
	Diagnostic diagnostic;
	REQUIRE(
		PrepareResolvedHostCapture(
			fixture.Invocation, Limits::MaximumEvaluationBytes, receipt, bytes, diagnostic
		) == Status::Ok
	);
	CHECK(receipt.Authored == fixture.Authored);
	CHECK(receipt.Tick == 7);
	CHECK(receipt.Inputs == fixture.Inputs);
	REQUIRE(receipt.InputImages.size() == 1);
	CHECK(receipt.InputImages[0].Hash == SurfaceHash(fixture.Base));
	CHECK(receipt.Images.empty());
	CHECK(bytes == *HostCaptureRetainedPayloadBytes(receipt));
	const auto retained = receipt.Inputs;
	const auto retainedBytes = bytes;
	CHECK(
		PrepareResolvedHostCapture(fixture.Invocation, 1, receipt, bytes, diagnostic) == Status::LimitExceeded
	);
	CHECK(receipt.Inputs == retained);
	CHECK(bytes == retainedBytes);
}
TEST_CASE(
	"resolved host receipt rejects duplicates invalid images and retained capacity before replacement",
	"[imagegraph][host-receipt]"
) {
	Fixture fixture;
	HostNodeCapture receipt;
	uint64_t bytes = 42;
	Diagnostic diagnostic;
	fixture.Inputs.push_back(fixture.Inputs.front());
	fixture.Invocation.Inputs = fixture.Inputs;
	CHECK(
		PrepareResolvedHostCapture(
			fixture.Invocation, Limits::MaximumEvaluationBytes, receipt, bytes, diagnostic
		) == Status::DuplicateId
	);
	CHECK(receipt.Inputs.empty());
	CHECK(bytes == 42);
	fixture.Inputs.pop_back();
	fixture.Invocation.Inputs = fixture.Inputs;
	fixture.Base.Pixels.pop_back();
	CHECK(
		PrepareResolvedHostCapture(
			fixture.Invocation, Limits::MaximumEvaluationBytes, receipt, bytes, diagnostic
		) == Status::InvalidValue
	);
	fixture.Base.Pixels.push_back(255);
	receipt.Authored.Values.reserve(4096);
	CHECK(*HostCaptureRetainedPayloadBytes(receipt) >= 4096 * sizeof(AuthoredValue));
	CHECK(
		PrepareResolvedHostCapture(fixture.Invocation, 4096, receipt, bytes, diagnostic) ==
		Status::LimitExceeded
	);
	CHECK(receipt.Authored.Values.capacity() >= 4096);
	CHECK(bytes == 42);
}

TEST_CASE(
	"resolved receipt admits sorting scratch and refuses invalid whole frame before copying",
	"[imagegraph][host-receipt]"
) {
	Fixture fixture;
	HostNodeCapture receipt;
	uint64_t bytes = 0;
	Diagnostic diagnostic;
	fixture.Request.Tick = Limits::MaximumTick + 1;
	CHECK(
		PrepareResolvedHostCapture(
			fixture.Invocation, Limits::MaximumEvaluationBytes, receipt, bytes, diagnostic
		) == Status::InvalidValue
	);
	CHECK(receipt.Inputs.empty());
	fixture.Request.Tick = 0;
	for (size_t i = 0; i < 512; ++i)
		fixture.Inputs.push_back({"control_" + std::to_string(i), int64_t(i)});
	fixture.Invocation.Inputs = fixture.Inputs;
	REQUIRE(
		PrepareResolvedHostCapture(
			fixture.Invocation, Limits::MaximumEvaluationBytes, receipt, bytes, diagnostic
		) == Status::Ok
	);
	CHECK(receipt.Inputs.size() == 513);
	const auto retained = receipt.Inputs;
	CHECK(
		PrepareResolvedHostCapture(fixture.Invocation, bytes + 1, receipt, bytes, diagnostic) ==
		Status::LimitExceeded
	);
	CHECK(receipt.Inputs == retained);
}
