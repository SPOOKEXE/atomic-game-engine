#include "SourceCommonExecution.hpp"

#include <engine/imagegraph/SourceCommonRuntime.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
TEST_SUITE_ID("engine.imagegraph.source_common_empty_overrides")
using namespace engine::imagegraph;
namespace {
	Document Graph(bool text = false, bool opaque = false, bool groupWrapper = false) {
		Document document;
		document.FormatVersion = 11;
		const std::string source = text ? "Node_Display_Text" : "Node_Frame";
		const std::string native = text ? "pc.display_text" : "pc.frame";
		Node node;
		node.Id = "annotation";
		node.Type = opaque ? "pxcx.opaque/" + source : native;
		if (groupWrapper) {
			node.Values = {{"source_type", node.Type}};
			node.Type = "internal.group_opaque";
		}
		node.Position = {17, 23};
		document.Nodes = {node};
		SourceCommonOwnerRecord owner;
		owner.SourceOwnerId = node.Id;
		owner.NativeOwnerId = node.Id;
		owner.SourceType = source;
		owner.ShowUpdateTrigger = true;
		owner.OutMeta = true;
		owner.UpdateAnimatorOwnerId = node.Id;
		owner.UpdateAnimatorPort = "native:animator:1";
		document.SourceCommonOwners = {owner};
		document.SourceAnimators.emplace();
		DetachedSourceAnimator writer;
		writer.OwnerId = node.Id;
		writer.Id = owner.UpdateAnimatorPort;
		writer.OriginalPort = "pxcx.update_in_trigger";
		writer.Type = ValueType::Boolean;
		writer.Writer = GroupSubtypeAnimator::Animated;
		document.SourceAnimators->Detached.push_back(writer);
		GroupSubtypeOverlay payload;
		payload.NodeId = node.Id;
		payload.Port = writer.Id;
		Keyframe key;
		key.NodeId = node.Id;
		key.Port = writer.Id;
		key.Data = false;
		payload.Keys = {key};
		document.SourceAnimators->DetachedValues = {payload};
		return document;
	}
	Plan Checked(const Document &document) {
		Plan plan;
		Diagnostic error;
		const auto code = CompileSourceCommonRuntime(document, plan, error);
		INFO(error.Message);
		REQUIRE(code == Status::Ok);
		return plan;
	}
	GroupRenderSession Cold(const Document &document) {
		GroupRenderSession session;
		Diagnostic error;
		const auto code = InitializeNativeSourceCommonRuntime(
			document, Checked(document), {}, SourceNodeInitialState::Loaded, session, error
		);
		INFO(error.Message);
		REQUIRE(code == Status::Ok);
		REQUIRE(session.Outputs.Nodes.size() == 1);
		REQUIRE(session.Outputs.Nodes[0].Outputs.empty());
		REQUIRE(session.Nodes.size() == 1);
		REQUIRE_FALSE(session.Nodes[0].Rendered);
		return session;
	}
	void Same(const GroupRenderSession &actual, const GroupRenderSession &before) {
		CHECK(actual.Outputs == before.Outputs);
		CHECK(actual.Nodes == before.Nodes);
		CHECK(actual.Common == before.Common);
		CHECK(actual.CommonAnimators == before.CommonAnimators);
		CHECK(actual.SourceCommonWrites == before.SourceCommonWrites);
		CHECK(actual.SourceCommonInputs == before.SourceCommonInputs);
		CHECK(actual.SourceCommonBindings == before.SourceCommonBindings);
		CHECK(actual.Purities == before.Purities);
		CHECK(actual.Replay.Data == before.Replay.Data);
		REQUIRE(actual.Replay.Outputs.size() == before.Replay.Outputs.size());
		for (size_t index = 0; index < actual.Replay.Outputs.size(); ++index) {
			const auto &first = actual.Replay.Outputs[index];
			const auto &second = before.Replay.Outputs[index];
			CHECK(first.Id == second.Id);
			REQUIRE(first.Output.index() == second.Output.index());
			if (const auto *image = std::get_if<Image>(&first.Output))
				CHECK(*image == std::get<Image>(second.Output));
			else if (const auto *array = std::get_if<ImageArray>(&first.Output)) {
				const auto &other = std::get<ImageArray>(second.Output);
				CHECK(array->Images == other.Images);
				CHECK(array->Items == other.Items);
			} else
				CHECK(std::get<EvaluatedValue>(first.Output) == std::get<EvaluatedValue>(second.Output));
		}
	}
	bool Update(const Document &document, const GroupRenderSession &session, uint64_t tick = 0) {
		EvaluatedValue value;
		Diagnostic error;
		EvaluationRequest request;
		request.Tick = tick;
		const auto code = ReadNativeSourceCommonGetter(
			document,
			Checked(document),
			"annotation",
			SourceCommonSelector::Update,
			request,
			session,
			value,
			error
		);
		INFO(error.Message);
		REQUIRE(code == Status::Ok);
		return std::get<bool>(value.Data);
	}
}
TEST_CASE(
	"empty overrides keep inherited source step metadata and update reset", "[source_common][empty_override]"
) {
	for (bool text : {false, true})
		for (bool paused : {false, true}) {
			auto document = Graph(text);
			auto session = Cold(document);
			REQUIRE(Update(document, session));
			EvaluationRequest request;
			request.SourceCachePlayback =
				SourceCachePlaybackObservation{!paused, SourceCacheSampling::ObservedFrame, true};
			const std::array names{SourceCommonRuntimeMetadataObservation{
				"annotation", document.SourceCommonOwners[0].SourceType, "observed runtime annotation"
			}};
			Diagnostic error;
			const auto code =
				NativeSourceStepBounded(document, Checked(document), request, {names, {}}, session, error);
			INFO(error.Message);
			REQUIRE(code == Status::Ok);
			CHECK(Update(document, session));
			CHECK_FALSE(Update(document, session, 1));
			REQUIRE(session.CommonAnimators.DetachedValues.size() == 1);
			const auto &keys = session.CommonAnimators.DetachedValues[0].Keys;
			REQUIRE(keys.size() == 1);
			CHECK(keys[0].Tick == 0);
			CHECK_FALSE(std::get<bool>(keys[0].Data));
			CHECK_FALSE(session.Common.Owners[0].Updated);
			CHECK(session.Common.Owners[0].Name == "observed runtime annotation");
			CHECK(session.Common.Owners[0].Position == (Vector2{17, 23}));
			CHECK_FALSE(session.Ready("annotation"));
			CHECK(session.Outputs.Nodes[0].Outputs.empty());
			REQUIRE(session.SourceCommonWrites.size() == 1);
			CHECK(session.SourceCommonWrites[0].Modified);
			CHECK(session.SourceCommonWrites[0].ForceDynamic);
			REQUIRE(session.SourceCommonInputs.size() == 1);
			CHECK_FALSE(session.SourceCommonInputs[0].Inputs);
		}
}
TEST_CASE(
	"inactive empty owner skips update reset and metadata without observations",
	"[source_common][empty_override]"
) {
	auto document = Graph();
	document.SourceCommonOwners[0].Active = false;
	auto session = Cold(document);
	const auto before = session;
	Diagnostic error;
	REQUIRE(NativeSourceStepBounded(document, Checked(document), {}, {}, session, error) == Status::Ok);
	Same(session, before);
	CHECK(Update(document, session));
}
TEST_CASE(
	"empty source callback is inert for direct and full wrappers independent of safe mode",
	"[source_common][empty_override]"
) {
	for (bool text : {false, true})
		for (bool safe : {false, true})
			for (bool graph : {false, true})
				for (auto mode :
					 {detail::SourceCommonInvocationMode::DirectUpdate,
					  detail::SourceCommonInvocationMode::SourceDoUpdate}) {
					auto document = Graph(text);
					document.SourceCommonOwners[0].UpdateGraph = graph;
					const auto plan = Checked(document);
					EvaluationRequest request;
					request.SourceSafeMode = safe;
					detail::EvaluationBudget budget(Limits::MaximumEvaluationBytes);
					detail::SourceCommonAdmission admission;
					detail::AllocationReservation charge;
					GroupRenderSession prior, candidate;
					Diagnostic error;
					REQUIRE(
						detail::PrepareSourceCommonCandidate(
							document,
							plan,
							request,
							SourceNodeInitialState::Loaded,
							prior,
							candidate,
							budget,
							charge,
							admission,
							error
						) == Status::Ok
					);
					const auto before = candidate;
					const auto used = budget.Used();
					REQUIRE(
						detail::InvokeSourceCommonCallback(
							document, plan, request, 0, mode, candidate, budget, charge, error, &admission
						) == Status::Ok
					);
					Same(candidate, before);
					CHECK(budget.Used() == used);
					CHECK(candidate.SourceCommonWrites.empty());
				}
}
TEST_CASE(
	"automatic full processing retains zero output annotation constructors", "[source_common][empty_override]"
) {
	for (bool text : {false, true})
		for (bool opaque : {false, true}) {
			auto document = Graph(text, opaque);
			auto session = Cold(document);
			const auto before = session;
			EvaluationRequest request;
			request.SourceSafeMode = false;
			Diagnostic error;
			const auto code = ProcessGroupRender(document, Checked(document), request, {}, session, error);
			INFO(error.Message);
			REQUIRE(code == Status::Ok);
			Same(session, before);
			CHECK(Update(document, session));
			CHECK_FALSE(session.Ready("annotation"));
		}
}
TEST_CASE(
	"empty native and preserved opaque identities are exact and metadata refusal is atomic",
	"[source_common][empty_override]"
) {
	for (bool text : {false, true})
		for (bool opaque : {false, true})
			for (bool group : {false, true}) {
				auto document = Graph(text, opaque, group);
				REQUIRE(SourceCommonEmptyOwnerMatches(
					document.Nodes[0], document.SourceCommonOwners[0].SourceType
				));
				CHECK_FALSE(SourceCommonEmptyOwnerMatches(
					document.Nodes[0], text ? "Node_Frame" : "Node_Display_Text"
				));
				CHECK_FALSE(SourceCommonEmptyOwnerMatches(document.Nodes[0], "Node_Unknown_Annotation"));
				auto session = Cold(document);
				const auto before = session;
				Diagnostic error;
				CHECK(
					NativeSourceStepBounded(document, Checked(document), {}, {}, session, error) ==
					Status::UnsupportedExecution
				);
				Same(session, before);
			}
	auto document = Graph();
	auto session = Cold(document);
	const auto before = session;
	const auto plan = Checked(document);
	document.SourceCommonOwners[0].SourceType = "Node_Unknown_Annotation";
	detail::EvaluationBudget budget(Limits::MaximumEvaluationBytes);
	auto charge = budget.Reserve(RetainedGroupRenderSessionBytes(session));
	REQUIRE(charge);
	Diagnostic error;
	CHECK(
		detail::InvokeSourceCommonCallback(
			document,
			plan,
			{},
			0,
			detail::SourceCommonInvocationMode::DirectUpdate,
			session,
			budget,
			*charge,
			error
		) == Status::UnsupportedExecution
	);
	Same(session, before);
}
