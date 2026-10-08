#include <engine/imagegraph/FeedbackHost.hpp>
#include <engine/imagegraph/FrameTime.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
TEST_SUITE_ID("engine.imagegraph.feedback_fractional_playback")
using namespace engine::imagegraph;
namespace {
	Document Graph(bool animated = true) {
		Document document;
		document.FormatVersion = 9;
		document.Project.emplace();
		document.Project->SurfaceWidth = document.Project->SurfaceHeight = 2;
		document.Nodes = {
			{"prior", "image.captured", "", {}, {{"source_id", std::string("feedback:out")}}},
			{"invert", "image.invert", "", {}, {{"include_alpha", false}, {"mix", 1.}}}
		};
		document.Links = {{"prior", "image", "invert", "image"}};
		document.Outputs = {{"out", "invert", "image"}};
		if (animated) {
			document.Keyframes = {{"invert", "mix", 1, 0., "linear"}, {"invert", "mix", 1, 1., "linear"}};
			REQUIRE(SetFrameTime(document.Keyframes[0], {1, .25, false}));
			REQUIRE(SetFrameTime(document.Keyframes[1], {1, .75, false}));
		}
		return document;
	}
	Plan Checked(const Document &document) {
		Plan plan;
		Diagnostic error;
		REQUIRE(Compile(document, plan, error) == Status::Ok);
		return plan;
	}
	EvaluationRequest Clock(double frame) {
		EvaluationRequest request;
		FrameTime time;
		REQUIRE(SplitFrameTime(frame, time));
		REQUIRE(SetFrameTime(request, time));
		return request;
	}
	void Pixel(const Image &image, uint8_t value) {
		REQUIRE(image.Pixels.size() == 16);
		for (size_t index = 0; index < 16; index += 4) {
			CHECK(image.Pixels[index] == value);
			CHECK(image.Pixels[index + 1] == value);
			CHECK(image.Pixels[index + 2] == value);
			CHECK(image.Pixels[index + 3] == 0);
		}
	}
	void Prepare(
		CapturedFeedbackHost &host,
		const Document &document,
		const Plan &plan,
		double frame,
		uint8_t pixel,
		bool capture = false,
		uint64_t revision = 1,
		uint64_t inputRevision = 1
	) {
		Diagnostic error;
		auto request = Clock(frame);
		const bool ready =
			capture
				? host.PrepareNodeInputs(document, plan, revision, inputRevision, "invert", request, error)
				: host.Prepare(document, plan, revision, inputRevision, request, error);
		INFO(error.Message << " frame=" << frame);
		REQUIRE(ready);
		REQUIRE(host.Output("out"));
		Pixel(*host.Output("out"), pixel);
		REQUIRE(
			host.PreparedFrame(revision, inputRevision) == std::optional<FrameTime>{GetFrameTime(request)}
		);
	}
}
TEST_CASE(
	"native fractional feedback evaluates fractional keys from one frame starting generation",
	"[feedback_fractional]"
) {
	const auto document = Graph();
	const auto plan = Checked(document);
	CapturedFeedbackHost host;
	REQUIRE(host.SetFeedbackSamplingProfile(FeedbackSamplingProfile::NativeFractional));
	Prepare(host, document, plan, 0, 0);
	Prepare(host, document, plan, 1, 0);
	Prepare(host, document, plan, 1.5, 128, true);
	REQUIRE(host.Snapshot().Images().size() == 1);
	Pixel(host.Snapshot().Images()[0].Data, 0);
	Prepare(host, document, plan, 1.75, 255, true);
	Pixel(host.Snapshot().Images()[0].Data, 0);
	Prepare(host, document, plan, 1.25, 0, true);
	Pixel(host.Snapshot().Images()[0].Data, 0);
	Prepare(host, document, plan, 1.5, 128, true);
	Prepare(host, document, plan, 2, 255, true);
	Pixel(host.Snapshot().Images()[0].Data, 0);
	Prepare(host, document, plan, 3, 0);
}
TEST_CASE("fractional seek restart and revisions reproduce canonical owned pixels", "[feedback_fractional]") {
	auto document = Graph();
	Document restored;
	Diagnostic error;
	REQUIRE(Read(Write(document), restored, error) == Status::Ok);
	REQUIRE(restored == document);
	const auto plan = Checked(restored);
	CapturedFeedbackHost host, fresh;
	REQUIRE(host.SetFeedbackSamplingProfile(FeedbackSamplingProfile::NativeFractional));
	REQUIRE(fresh.SetFeedbackSamplingProfile(FeedbackSamplingProfile::NativeFractional));
	Prepare(host, restored, plan, 3.5, 0);
	Prepare(host, restored, plan, 1.5, 128);
	Prepare(fresh, restored, plan, 1.5, 128);
	REQUIRE(*host.Output("out") == *fresh.Output("out"));
	Prepare(host, restored, plan, 0, 0);
	Prepare(host, restored, plan, 2.5, 255);
	host.RestartCycle();
	Prepare(host, restored, plan, 1.5, 128);
	Prepare(host, restored, plan, 1.75, 255, false, 2, 1);
	Prepare(host, restored, plan, 1.5, 128, false, 2, 2);
	host.Clear();
	CHECK(host.FeedbackProfile() == FeedbackSamplingProfile::NativeFractional);
	Prepare(host, restored, plan, 1.5, 128);
}
TEST_CASE(
	"fractional feedback failed replacement preserves pixels clock and input snapshot",
	"[feedback_fractional]"
) {
	const auto document = Graph();
	const auto plan = Checked(document);
	CapturedFeedbackHost host;
	REQUIRE(host.SetFeedbackSamplingProfile(FeedbackSamplingProfile::NativeFractional));
	Prepare(host, document, plan, 1.5, 128, true);
	const auto image = *host.Output("out");
	const auto snapshot = host.Snapshot().Images()[0].Data;
	const auto clock = host.PreparedFrame(1, 1);
	Diagnostic error;
	auto request = Clock(1.75);
	CHECK_FALSE(host.PrepareNodeInputs(document, plan, 1, 1, "invert", request, error, 1));
	CHECK(error.Code == Status::LimitExceeded);
	REQUIRE(host.Output("out"));
	CHECK(*host.Output("out") == image);
	CHECK(host.Snapshot().Images()[0].Data == snapshot);
	CHECK(host.PreparedFrame(1, 1) == clock);
	request = Clock(4097.5);
	CHECK_FALSE(host.Prepare(document, plan, 1, 1, request, error));
	CHECK(error.Code == Status::LimitExceeded);
	CHECK(*host.Output("out") == image);
	request = Clock(-.5);
	CHECK_FALSE(host.Prepare(document, plan, 1, 1, request, error));
	CHECK(error.Code == Status::InvalidValue);
	CHECK(*host.Output("out") == image);
	Prepare(host, document, plan, 1.75, 255, true);
}
TEST_CASE(
	"native fractional frame zero previews never consume the preceding preview", "[feedback_fractional]"
) {
	const auto document = Graph(false);
	const auto plan = Checked(document);
	CapturedFeedbackHost host;
	REQUIRE(host.SetFeedbackSamplingProfile(FeedbackSamplingProfile::NativeFractional));
	Prepare(host, document, plan, 0, 255);
	Prepare(host, document, plan, .25, 255, true);
	Pixel(host.Snapshot().Images()[0].Data, 0);
	Prepare(host, document, plan, .75, 255, true);
	Pixel(host.Snapshot().Images()[0].Data, 0);
	Prepare(host, document, plan, 1, 0, true);
	Pixel(host.Snapshot().Images()[0].Data, 255);
}
TEST_CASE(
	"feedback profile selection is explicit validated and retires preceding generations",
	"[feedback_fractional]"
) {
	const auto document = Graph(false);
	const auto plan = Checked(document);
	CapturedFeedbackHost host;
	REQUIRE(host.FeedbackProfile() == FeedbackSamplingProfile::LegacyFixedTicks);
	Prepare(host, document, plan, 0, 255);
	Diagnostic error;
	auto request = Clock(.5);
	CHECK_FALSE(host.Prepare(document, plan, 1, 1, request, error));
	CHECK(error.Code == Status::InvalidValue);
	const auto image = *host.Output("out");
	CHECK_FALSE(host.SetFeedbackSamplingProfile(static_cast<FeedbackSamplingProfile>(99)));
	CHECK(*host.Output("out") == image);
	REQUIRE(host.SetFeedbackSamplingProfile(FeedbackSamplingProfile::NativeFractional));
	CHECK_FALSE(host.Output("out"));
	Prepare(host, document, plan, .5, 255);
	REQUIRE(host.SetFeedbackSamplingProfile(FeedbackSamplingProfile::LegacyFixedTicks));
	CHECK_FALSE(host.Output("out"));
	request = Clock(.5);
	CHECK_FALSE(host.Prepare(document, plan, 1, 1, request, error));
	CHECK(error.Code == Status::InvalidValue);
}
