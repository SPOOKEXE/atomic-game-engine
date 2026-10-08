#include "ImageGraphPreviewResult.hpp"

#include <engine/imagegraph/GroupRenderSession.hpp>
#include <engine/imagegraph/HostCapture.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("studio.imagegraph.disabled_groups")
TEST_DEPENDS("studio.imagegraph.preview_result")
TEST_DEPENDS("engine.imagegraph.feedback_host")

namespace {
	using namespace engine::imagegraph;

	Document GroupedNumber() {
		Document document;
		document.FormatVersion = 10;
		document.Groups = {{"group", "Group"}};
		document.Nodes = {{"number", "pc.number_simple", "group", {}, {{"value", 7.0}}}};
		document.Outputs = {{"result", "number", "number"}};
		return document;
	}

	void PreparePreview(
		Document &document,
		CapturedFeedbackHost &host,
		uint64_t revision,
		studio::ImageGraphPreviewValue &preview,
		Diagnostic &diagnostic
	) {
		Plan plan;
		REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
		EvaluationRequest request;
		REQUIRE(
			studio::detail::PrepareImageGraphPreviewResult(
				document, plan, "result", request, host, revision, 1, preview, diagnostic
			) == Status::Ok
		);
	}

	double Number(const studio::ImageGraphPreviewValue &preview) {
		const auto *evaluated = std::get_if<EvaluatedValue>(&preview);
		REQUIRE(evaluated);
		const auto *number = std::get_if<double>(&evaluated->Data);
		REQUIRE(number);
		return *number;
	}
}

TEST_CASE(
	"Studio preview retains group output across disable and authored edits",
	"[studio][imagegraph][disabled-groups]"
) {
	auto document = GroupedNumber();
	CapturedFeedbackHost host;
	studio::ImageGraphPreviewValue preview;
	Diagnostic diagnostic;
	PreparePreview(document, host, 1, preview, diagnostic);
	CHECK(Number(preview) == 7.0);
	document.Groups.front().RenderActive = false;
	document.Nodes.front().Values.front().Data = 19.0;
	PreparePreview(document, host, 2, preview, diagnostic);
	CHECK(Number(preview) == 7.0);
	document.Groups.front().RenderActive = true;
	PreparePreview(document, host, 3, preview, diagnostic);
	CHECK(Number(preview) == 19.0);
}

TEST_CASE(
	"Studio cold disabled group observes constructor output after save and reopen",
	"[studio][imagegraph][disabled-groups]"
) {
	auto document = GroupedNumber();
	document.Groups.front().RenderActive = false;
	Document reopened;
	Diagnostic diagnostic;
	REQUIRE(Read(Write(document), reopened, diagnostic) == Status::Ok);
	REQUIRE_FALSE(reopened.Groups.front().RenderActive);
	CapturedFeedbackHost host;
	studio::ImageGraphPreviewValue preview;
	PreparePreview(reopened, host, 1, preview, diagnostic);
	CHECK(Number(preview) == 0.0);
}

TEST_CASE(
	"Studio explicit group rendering updates held values without enabling automatic rendering",
	"[studio][imagegraph][disabled-groups]"
) {
	auto document = GroupedNumber();
	CapturedFeedbackHost host;
	studio::ImageGraphPreviewValue preview;
	Diagnostic diagnostic;
	PreparePreview(document, host, 1, preview, diagnostic);
	document.Groups.front().RenderActive = false;
	document.Nodes.front().Values.front().Data = 23.0;
	Plan plan;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	EvaluationRequest request;
	REQUIRE(host.ForceGroup(document, plan, "group", request, diagnostic));
	CHECK_FALSE(document.Groups.front().RenderActive);
	PreparePreview(document, host, 2, preview, diagnostic);
	CHECK(Number(preview) == 23.0);
}

TEST_CASE(
	"Studio refused group processing preserves prior output and host state",
	"[studio][imagegraph][disabled-groups]"
) {
	auto document = GroupedNumber();
	CapturedFeedbackHost host;
	studio::ImageGraphPreviewValue preview;
	Diagnostic diagnostic;
	PreparePreview(document, host, 1, preview, diagnostic);
	const auto retained = host.RetainedBytes();
	document.Nodes.front().Values.front().Data = 42.0;
	Plan plan;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	EvaluationRequest request;
	CHECK_FALSE(host.Prepare(document, plan, 2, 1, request, diagnostic, 1, "result"));
	CHECK(diagnostic.Code == Status::LimitExceeded);
	CHECK(host.RetainedBytes() == retained);
	CHECK(Number(preview) == 7.0);
	REQUIRE(host.Value("result"));
	const auto *held = std::get_if<EvaluatedValue>(&host.Value("result")->Output);
	REQUIRE(held);
	CHECK(std::get<double>(held->Data) == 7.0);
}

TEST_CASE(
	"Studio thumbnail observers borrow the main group pulse without processing producers",
	"[studio][imagegraph][disabled-groups]"
) {
	auto document = GroupedNumber();
	CapturedFeedbackHost mainHost;
	studio::ImageGraphPreviewValue mainPreview;
	Diagnostic diagnostic;
	PreparePreview(document, mainHost, 1, mainPreview, diagnostic);
	const auto *pulse = mainHost.PreparedGroups(1, 1, {});
	REQUIRE(pulse);
	const auto beforeOutputs = pulse->Outputs;
	const auto beforeReadiness = pulse->Nodes;
	Document thumbnail = document;
	thumbnail.Outputs.front().Id = "embedded-preview";
	Plan plan;
	REQUIRE(Compile(thumbnail, plan, diagnostic) == Status::Ok);
	EvaluationRequest request;
	request.GroupRender = pulse;
	CapturedFeedbackHost thumbnailHost;
	studio::ImageGraphPreviewValue thumbnailPreview;
	REQUIRE(
		studio::detail::PrepareImageGraphPreviewResult(
			thumbnail, plan, "embedded-preview", request, thumbnailHost, 1, 1, thumbnailPreview, diagnostic
		) == Status::Ok
	);
	CHECK(Number(thumbnailPreview) == 7.0);
	CHECK(pulse->Outputs == beforeOutputs);
	CHECK(pulse->Nodes == beforeReadiness);
	CHECK_FALSE(mainHost.PreparedGroups(2, 1, {}));
	CHECK_FALSE(mainHost.PreparedGroups(1, 2, {}));
	CHECK_FALSE(mainHost.PreparedGroups(1, 1, {1, 0, false}));
}

TEST_CASE(
	"Studio same pulse output and input selection never repeats source host work",
	"[studio][imagegraph][disabled-groups]"
) {
	struct FileObservation final : HostNodeProvider {
		size_t Calls = 0;
		bool Capture(
			const HostNodeInvocation &invocation, HostNodeCapture &capture, std::string &failure
		) override {
			uint64_t bytes = 0;
			Diagnostic diagnostic;
			if (PrepareResolvedHostCapture(
					invocation, invocation.MaximumOperationBytes, capture, bytes, diagnostic
				) != Status::Ok) {
				failure = diagnostic.Message;
				return false;
			}
			++Calls;
			capture.Outputs = {
				{"content", std::string{"observed text"}}, {"path", std::string{"recorded.txt"}}
			};
			return true;
		}
	} provider;
	Document document;
	document.FormatVersion = 10;
	document.Groups = {{"group", "Group"}};
	document.Nodes = {{"read", "pc.text_file_read", "group", {}, {{"path", std::string{"recorded.txt"}}}}};
	document.Outputs = {{"content", "read", "content"}, {"path", "read", "path"}};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	EvaluationRequest request;
	request.HostProvider = &provider;
	CapturedFeedbackHost host;
	REQUIRE(
		host.Prepare(document, plan, 1, 1, request, diagnostic, Limits::MaximumEvaluationBytes, "content")
	);
	REQUIRE(provider.Calls == 1);
	const auto *pulse = host.PreparedGroups(1, 1, {});
	REQUIRE(pulse);
	const auto outputs = pulse->Outputs;
	const auto readiness = pulse->Nodes;
	REQUIRE(host.Prepare(document, plan, 1, 1, request, diagnostic, Limits::MaximumEvaluationBytes, "path"));
	CHECK(provider.Calls == 1);
	REQUIRE(host.Value("path"));
	CHECK(std::get<std::string>(std::get<EvaluatedValue>(host.Value("path")->Output).Data) == "recorded.txt");
	REQUIRE(host.PrepareNodeInputs(document, plan, 1, 1, "read", request, diagnostic));
	CHECK(provider.Calls == 1);
	REQUIRE(host.PreparedGroups(1, 1, {}));
	CHECK(host.PreparedGroups(1, 1, {})->Outputs == outputs);
	CHECK(host.PreparedGroups(1, 1, {})->Nodes == readiness);
}

TEST_CASE(
	"Studio purity callbacks refresh for explicit attribute changes but not value edits",
	"[studio][imagegraph][disabled-groups]"
) {
	auto document = GroupedNumber();
	CapturedFeedbackHost host;
	studio::ImageGraphPreviewValue preview;
	Diagnostic diagnostic;
	PreparePreview(document, host, 1, preview, diagnostic);
	const auto *loaded = host.PreparedGroups(1, 1, {});
	REQUIRE(loaded);
	REQUIRE(loaded->Purities.size() == 1);
	const auto initialPurity = loaded->Purities.front();
	document.Nodes.front().Values.front().Data = 13.0;
	PreparePreview(document, host, 2, preview, diagnostic);
	REQUIRE(host.PreparedGroups(2, 1, {}));
	CHECK(host.PreparedGroups(2, 1, {})->Purities.front() == initialPurity);
	document.Groups.front().RenderActive = false;
	document.Groups.front().PureFunction = false;
	document.Nodes.front().Values.front().Data = 23.0;
	Plan plan;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	EvaluationRequest request;
	REQUIRE(host.ForceGroup(document, plan, "group", request, diagnostic));
	PreparePreview(document, host, 3, preview, diagnostic);
	CHECK(Number(preview) == 13.0);
	REQUIRE(host.PreparedGroups(3, 1, {}));
	CHECK(host.PreparedGroups(3, 1, {})->Purities.front().State == SourceGroupPurity::Nonpure);
	document.Groups.front().PureFunction = true;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	REQUIRE(host.ForceGroup(document, plan, "group", request, diagnostic));
	PreparePreview(document, host, 4, preview, diagnostic);
	CHECK(Number(preview) == 23.0);
}

TEST_CASE(
	"Studio animation mode changes refresh cached group purity before forcing",
	"[studio][imagegraph][disabled-groups]"
) {
	auto document = GroupedNumber();
	CapturedFeedbackHost host;
	studio::ImageGraphPreviewValue preview;
	Diagnostic diagnostic;
	PreparePreview(document, host, 1, preview, diagnostic);
	document.Groups.front().RenderActive = false;
	document.Nodes.front().SourceAnimatedInputs = {"value"};
	document.Keyframes = {{"number", "value", 0, 11.0}, {"number", "value", 10, 19.0}};
	Plan plan;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	EvaluationRequest request;
	REQUIRE(host.ForceGroup(document, plan, "group", request, diagnostic));
	PreparePreview(document, host, 2, preview, diagnostic);
	CHECK(Number(preview) == 7.0);
	REQUIRE(host.PreparedGroups(2, 1, {}));
	CHECK(host.PreparedGroups(2, 1, {})->Purities.front().State == SourceGroupPurity::Nonpure);
	document.Keyframes.front().Data = 29.0;
	document.Keyframes.push_back({"number", "value", 5, 31.0});
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	REQUIRE(host.ForceGroup(document, plan, "group", request, diagnostic));
	PreparePreview(document, host, 3, preview, diagnostic);
	CHECK(Number(preview) == 7.0);
	document.Nodes.front().SourceAnimatedInputs.clear();
	document.Nodes.front().SourceStaticInputs = {"value"};
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	REQUIRE(host.ForceGroup(document, plan, "group", request, diagnostic));
	PreparePreview(document, host, 4, preview, diagnostic);
	CHECK(Number(preview) == 29.0);
	CHECK(host.PreparedGroups(4, 1, {})->Purities.front().State == SourceGroupPurity::Pure);
}
