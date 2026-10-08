#include "SourceCommonExecution.hpp"

#include <engine/imagegraph/HostCapture.hpp>
#include <engine/imagegraph/SourceCommonRuntime.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.imagegraph.composer_scope_execution")
using namespace engine::imagegraph;
namespace {
	struct CountingHost final : HostNodeProvider {
		uint32_t Calls = 0;
		bool Capture(const HostNodeInvocation &, HostNodeCapture &, std::string &failure) override {
			++Calls;
			failure = "unexpected host capture";
			return false;
		}
	};
	Plan Checked(const Document &document, bool common = false) {
		Plan plan;
		Diagnostic diagnostic;
		const auto status = common ? CompileSourceCommonRuntime(document, plan, diagnostic)
								   : Compile(document, plan, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(status == Status::Ok);
		return plan;
	}
	Document CommonAudio() {
		Document document;
		document.FormatVersion = 11;
		document.Nodes = {{"audio", "pc.audio_window", "", {}, {}}};
		SourceCommonOwnerRecord owner;
		owner.SourceOwnerId = "audio";
		owner.NativeOwnerId = "audio";
		owner.SourceType = "Node_Audio_Window";
		owner.ShowUpdateTrigger = true;
		owner.UpdateAnimatorOwnerId = "audio";
		owner.UpdateAnimatorPort = "native:animator:1";
		document.SourceCommonOwners = {owner};
		document.SourceAnimators.emplace();
		DetachedSourceAnimator writer;
		writer.OwnerId = "audio";
		writer.Id = owner.UpdateAnimatorPort;
		writer.OriginalPort = "pxcx.update_in_trigger";
		writer.Type = ValueType::Boolean;
		writer.Writer = GroupSubtypeAnimator::Static;
		document.SourceAnimators->Detached = {writer};
		GroupSubtypeOverlay payload;
		payload.NodeId = "audio";
		payload.Port = writer.Id;
		payload.Fixed = true;
		document.SourceAnimators->DetachedValues = {payload};
		return document;
	}
}

TEST_CASE(
	"Image output works beside excluded nodes while explicit audio output refuses",
	"[imagegraph][composer_scope]"
) {
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {
		{"image", "image.solid", "", {}, {{"width", int64_t{2}}, {"height", int64_t{2}}}},
		{"audio", "image.audio_recording", "", {}, {{"source_id", std::string{"source"}}}}
	};
	document.Outputs = {{"image", "image", "image"}, {"audio", "audio", "samples"}};
	const auto plan = Checked(document);
	EvaluationRequest request;
	request.Scope = ComposerScope::ImageOnly;
	Diagnostic diagnostic;
	Image image;
	REQUIRE(Evaluate(document, plan, "image", request, image, diagnostic) == Status::Ok);
	CHECK(image.Width == 2);
	EvaluatedValue value{"sentinel", 73., {}};
	const auto before = value;
	CHECK(EvaluateValue(document, plan, "audio", request, value, diagnostic) == Status::UnsupportedExecution);
	CHECK(diagnostic.NodeId == "audio");
	CHECK(diagnostic.Message == "Audio is disabled in the image composer");
	CHECK(value == before);
}

TEST_CASE("Excluded video refuses before host capture and input inspection", "[imagegraph][composer_scope]") {
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {{"video", "pc.image_mp4", "", {}, {}}};
	document.Outputs = {{"image", "video", "surface_out"}};
	const auto plan = Checked(document);
	CountingHost host;
	EvaluationRequest request;
	request.Scope = ComposerScope::ImageOnly;
	request.HostProvider = &host;
	Diagnostic diagnostic;
	Image image;
	CHECK(Evaluate(document, plan, "image", request, image, diagnostic) == Status::UnsupportedExecution);
	CHECK(diagnostic.NodeId == "video");
	CHECK(diagnostic.Message == "Video is disabled in the image composer");
	EvaluationSnapshot snapshot;
	CHECK(
		EvaluateNodeInputs(document, plan, "video", request, snapshot, diagnostic) ==
		Status::UnsupportedExecution
	);
	CHECK(host.Calls == 0);
}

TEST_CASE(
	"Automatic common Step skips excluded owner while explicit callbacks refuse atomically",
	"[imagegraph][composer_scope]"
) {
	const auto document = CommonAudio();
	const auto plan = Checked(document, true);
	EvaluationRequest request;
	request.Scope = ComposerScope::ImageOnly;
	GroupRenderSession session;
	Diagnostic diagnostic;
	REQUIRE(
		InitializeNativeSourceCommonRuntime(
			document, plan, request, SourceNodeInitialState::Loaded, session, diagnostic
		) == Status::Ok
	);
	const auto before = session;
	REQUIRE(NativeSourceStepBounded(document, plan, request, {}, session, diagnostic) == Status::Ok);
	CHECK(session.Common == before.Common);
	CHECK(session.CommonAnimators == before.CommonAnimators);
	CHECK(session.SourceCommonWrites == before.SourceCommonWrites);
	for (const auto mode :
		 {detail::SourceCommonInvocationMode::DirectUpdate,
		  detail::SourceCommonInvocationMode::SourceDoUpdate}) {
		detail::EvaluationBudget budget(Limits::MaximumEvaluationBytes);
		detail::AllocationReservation charge;
		const auto used = budget.Used();
		CHECK(
			detail::InvokeSourceCommonCallback(
				document, plan, request, 0, mode, session, budget, charge, diagnostic
			) == Status::UnsupportedExecution
		);
		CHECK(diagnostic.NodeId == "audio");
		CHECK(diagnostic.Message == "Audio is disabled in the image composer");
		CHECK(session.Common == before.Common);
		CHECK(session.SourceCommonWrites == before.SourceCommonWrites);
		CHECK(budget.Used() == used);
	}
}

TEST_CASE("Invalid scope cannot grant an image node unrestricted execution", "[imagegraph][composer_scope]") {
	Node node{"image", "image.solid", "", {}, {}};
	const auto scope = static_cast<ComposerScope>(255);
	Diagnostic diagnostic;
	CHECK_FALSE(ComposerNodeEnabled(node, scope));
	CHECK_FALSE(ComposerExportEnabled(".png", scope));
	CHECK(CheckComposerNodeScope(node, scope, diagnostic) == Status::InvalidValue);
	CHECK(diagnostic.NodeId == "image");
}
