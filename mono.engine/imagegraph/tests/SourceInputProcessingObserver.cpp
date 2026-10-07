#include <engine/imagegraph/FrameTime.hpp>
#include <engine/imagegraph/SourceInputProcessingObserver.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.imagegraph.source_input_processing_observer")
using namespace engine::imagegraph;
namespace {
	struct Observer : SourceInputProcessingObserver {
		std::string Selected = "mirror";
		unsigned Calls = 0;
		FrameTime Frame;
		Vector2 Center;
		bool Refuse = false;
		std::string_view NodeId() const noexcept override {
			return Selected;
		}
		uint64_t RetainedBytes() const noexcept override {
			return 0;
		}
		Status Observe(
			FrameTime frame,
			std::span<const EvaluationInputValue> values,
			std::span<const EvaluationInputImage> images,
			std::span<const SnapshotImageArray> arrays,
			Diagnostic &error,
			uint64_t
		) override {
			++Calls;
			Frame = frame;
			REQUIRE(images.size() == 1);
			REQUIRE(arrays.empty());
			for (const auto &value : values)
				if (value.Port == "center") Center = std::get<Vector2>(value.Data);
			if (Refuse) {
				error = {Status::LimitExceeded, {}, {}, "receipt refused"};
				return error.Code;
			}
			return Status::Ok;
		}
	};
	struct FrameObserver final : Observer {
		FrameTime Interested{3, .5, true};
		mutable unsigned OwnerLookups = 0;
		bool ObservesFrame(FrameTime frame) const noexcept override {
			return frame == Interested;
		}
		bool ObservesNode(std::string_view nodeId) const noexcept override {
			++OwnerLookups;
			return Observer::ObservesNode(nodeId);
		}
	};

	struct BudgetObserver final : Observer {
		uint64_t Held = 0;
		bool Optional = true, Released = false;
		unsigned InitialRefusals = 0, OwnerRefusals = 0;
		uint64_t RetainedBytes() const noexcept override {
			return Held;
		}
		bool ObservesNode(std::string_view nodeId) const noexcept override {
			return !Released && Observer::ObservesNode(nodeId);
		}
		Status Observe(
			FrameTime frame,
			std::span<const EvaluationInputValue> values,
			std::span<const EvaluationInputImage> images,
			std::span<const SnapshotImageArray> arrays,
			Diagnostic &error,
			uint64_t maximumBytes
		) override {
			const auto status = Observer::Observe(frame, values, images, arrays, error, maximumBytes);
			Held = Limits::MaximumEvaluationBytes;
			return status;
		}
		Status CaptureRefused(std::string_view nodeId, Diagnostic &error) override {
			if (!Optional) return SourceInputProcessingObserver::CaptureRefused(nodeId, error);
			if (nodeId.empty())
				++InitialRefusals;
			else
				++OwnerRefusals;
			Held = 0;
			Released = true;
			error = {};
			return Status::Ok;
		}
	};

}
TEST_CASE(
	"normal source processing observes its processed expression map without another getter evaluation"
) {
	Document document;
	document.FormatVersion = 10;
	document.Nodes = {
		{"source",
		 "image.solid",
		 {},
		 {},
		 {{"width", int64_t{1}}, {"height", int64_t{1}}, {"colour", Colour{}}}},
		{"mirror", "pc.mirror_polar", {}, {}, {{"center", Vector2{.2, .3}}}}
	};
	document.Nodes.back().SourceInputExpressions = {{"center", "value + self.center", true}};
	document.Links = {{"source", "image", "mirror", "surface_in"}};
	document.Outputs = {{"out", "mirror", "surface_out"}};
	Plan plan;
	Diagnostic error;
	REQUIRE(Compile(document, plan, error) == Status::Ok);
	Observer observer;
	EvaluationRequest request;
	REQUIRE(SetFrameTime(request, {2, .25, true}));
	request.SourceInputObserver = &observer;
	Image output;
	REQUIRE(Evaluate(document, plan, "out", request, output, error) == Status::Ok);
	REQUIRE(observer.Calls == 1);
	REQUIRE(observer.Frame == GetFrameTime(request));
	REQUIRE(observer.Center.X == Catch::Approx(.4));
	REQUIRE(observer.Center.Y == Catch::Approx(.6));
	SECTION("unprocessed target causes no extra evaluation") {
		observer.Selected = "missing";
		observer.Calls = 0;
		REQUIRE(Evaluate(document, plan, "out", request, output, error) == Status::Ok);
		REQUIRE(observer.Calls == 0);
	}
	SECTION("receipt refusal preserves previously published output") {
		const auto before = output;
		observer.Refuse = true;
		REQUIRE(Evaluate(document, plan, "out", request, output, error) == Status::LimitExceeded);
		REQUIRE(output == before);
	}
}

TEST_CASE("source expression context owner follows inherited inputs and explicit overrides") {
	Document document;
	document.FormatVersion = 10;
	document.Nodes = {
		{"base", "pc.mirror_polar", {}, {}, {{"center", Vector2{.2, .3}}}},
		{"copy", "pc.mirror_polar", {}, {}, {{"center", Vector2{.7, .8}}}}
	};
	document.Nodes.back().InstanceBase = "base";
	document.Outputs = {{"out", "copy", "surface_out"}};
	Plan plan;
	Diagnostic error;
	const auto compiled = Compile(document, plan, error);
	INFO(error.Message);
	REQUIRE(compiled == Status::Ok);
	REQUIRE(SourceInputExpressionOwner(document, plan, "copy", "center") == "base");
	document.Nodes.back().InstanceOverrides = {"center"};
	REQUIRE(Compile(document, plan, error) == Status::Ok);
	REQUIRE(SourceInputExpressionOwner(document, plan, "copy", "center") == "copy");
	REQUIRE_FALSE(SourceInputExpressionOwner(document, plan, "missing", "center"));
}

TEST_CASE("historical processing clocks skip receipt owner lookup and input cloning") {
	Document document;
	document.FormatVersion = 10;
	document.Nodes = {
		{"source",
		 "image.solid",
		 {},
		 {},
		 {{"width", int64_t{128}}, {"height", int64_t{128}}, {"colour", Colour{}}}},
		{"mirror", "pc.mirror_polar", {}, {}, {{"center", Vector2{.2, .3}}}}
	};
	document.Links = {{"source", "image", "mirror", "surface_in"}};
	document.Outputs = {{"out", "mirror", "surface_out"}};
	Plan plan;
	Diagnostic error;
	REQUIRE(Compile(document, plan, error) == Status::Ok);
	EvaluationRequest request;
	REQUIRE(SetFrameTime(request, {2, .25, true}));
	// Use the ordinary evaluator's exact admission boundary. An extra input image
	// copy at this boundary must fail, so ignored clocks cannot hide a clone.
	uint64_t low = 1, high = Limits::MaximumEvaluationBytes;
	while (low < high) {
		const uint64_t middle = low + (high - low) / 2;
		Image trial;
		const auto status = Evaluate(document, plan, "out", request, trial, error, middle);
		REQUIRE((status == Status::Ok || status == Status::LimitExceeded));
		if (status == Status::Ok)
			high = middle;
		else
			low = middle + 1;
	}
	Image expected;
	REQUIRE(Evaluate(document, plan, "out", request, expected, error, low) == Status::Ok);
	FrameObserver observer;
	request.SourceInputObserver = &observer;
	Image output;
	REQUIRE(Evaluate(document, plan, "out", request, output, error, low) == Status::Ok);
	REQUIRE(output == expected);
	REQUIRE(observer.OwnerLookups == 0);
	REQUIRE(observer.Calls == 0);
	// The same observer captures its exact signed fractional target clock.
	REQUIRE(SetFrameTime(request, observer.Interested));
	REQUIRE(Evaluate(document, plan, "out", request, output, error) == Status::Ok);
	REQUIRE(observer.OwnerLookups > 0);
	REQUIRE(observer.Calls == 1);
	REQUIRE(observer.Frame == observer.Interested);
}

TEST_CASE("optional observer residency admission releases unavailable receipts without refusing pixels") {
	Document document;
	document.FormatVersion = 10;
	document.Nodes = {
		{"source",
		 "image.solid",
		 {},
		 {},
		 {{"width", int64_t{1}}, {"height", int64_t{1}}, {"colour", Colour{}}}},
		{"mirror", "pc.mirror_polar", {}, {}, {{"center", Vector2{.2, .3}}}}
	};
	document.Links = {{"source", "image", "mirror", "surface_in"}};
	document.Outputs = {{"out", "mirror", "surface_out"}};
	Plan plan;
	Diagnostic error;
	REQUIRE(Compile(document, plan, error) == Status::Ok);
	EvaluationRequest request;
	Image expected;
	constexpr uint64_t allowance = 1024 * 1024;
	REQUIRE(Evaluate(document, plan, "out", request, expected, error, allowance) == Status::Ok);
	BudgetObserver observer;
	bool initial = false;
	SECTION("initial receipt residency cannot be admitted") {
		initial = true;
		observer.Held = Limits::MaximumEvaluationBytes;
	}
	SECTION("processed receipt residency grows beyond the ledger") {}
	SECTION("mandatory observer retains refusal semantics") {
		observer.Optional = false;
	}
	request.SourceInputObserver = &observer;
	Image output;
	const auto status = Evaluate(document, plan, "out", request, output, error, allowance);
	if (!observer.Optional) {
		REQUIRE(status == Status::LimitExceeded);
		REQUIRE(observer.Calls == 1);
	} else {
		REQUIRE(status == Status::Ok);
		REQUIRE(error.Code == Status::Ok);
		REQUIRE(output == expected);
		REQUIRE(observer.Held == 0);
		REQUIRE(observer.InitialRefusals == (initial ? 1 : 0));
		REQUIRE(observer.OwnerRefusals == (initial ? 0 : 1));
		REQUIRE(observer.Calls == (initial ? 0 : 1));
	}
}
