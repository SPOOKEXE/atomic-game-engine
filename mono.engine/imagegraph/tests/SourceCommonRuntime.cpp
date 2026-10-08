#include <engine/imagegraph/SourceCommonRuntime.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>

TEST_SUITE_ID("engine.imagegraph.source_common_runtime")
TEST_DEPENDS("engine.imagegraph.source_common_sockets")
TEST_DEPENDS("engine.imagegraph.group_render_session")

using namespace engine::imagegraph;
namespace {
	std::string WriterId(std::string_view owner) {
		const std::array<std::string_view, 4> owners{"first", "second", "new", "collection"};
		const auto found = std::find(owners.begin(), owners.end(), owner);
		REQUIRE(found != owners.end());
		return "native:animator:" + std::to_string(std::distance(owners.begin(), found) + 1);
	}
	SourceCommonOwnerRecord Owner(std::string id, bool requested = false, bool metadata = false) {
		SourceCommonOwnerRecord owner;
		owner.SourceOwnerId = id;
		owner.SourceType = "Node_Boolean";
		owner.NativeOwnerId = id;
		owner.ShowUpdateTrigger = requested;
		owner.OutMeta = metadata;
		owner.UpdateAnimatorOwnerId = id;
		owner.UpdateAnimatorPort = WriterId(id);
		return owner;
	}
	void Animator(
		Document &document,
		std::string_view id,
		bool requested,
		GroupSubtypeAnimator mode = GroupSubtypeAnimator::Static
	) {
		if (!document.SourceAnimators) document.SourceAnimators.emplace();
		DetachedSourceAnimator writer;
		writer.OwnerId = id;
		writer.Id = WriterId(id);
		writer.OriginalPort = "pxcx.update_in_trigger";
		writer.Type = ValueType::Boolean;
		writer.Writer = mode;
		document.SourceAnimators->Detached.push_back(std::move(writer));
		GroupSubtypeOverlay payload;
		payload.NodeId = id;
		payload.Port = WriterId(id);
		payload.Fixed = requested;
		document.SourceAnimators->DetachedValues.push_back(std::move(payload));
	}
	void RequestAtZero(Document &document, std::string_view id) {
		const auto writer = std::find_if(
			document.SourceAnimators->Detached.begin(),
			document.SourceAnimators->Detached.end(),
			[&](const auto &row) { return row.OwnerId == id; }
		);
		const auto payload = std::find_if(
			document.SourceAnimators->DetachedValues.begin(),
			document.SourceAnimators->DetachedValues.end(),
			[&](const auto &row) { return row.NodeId == id; }
		);
		REQUIRE(writer != document.SourceAnimators->Detached.end());
		REQUIRE(payload != document.SourceAnimators->DetachedValues.end());
		writer->Writer = GroupSubtypeAnimator::Animated;
		payload->Fixed.reset();
		Keyframe key;
		key.NodeId = id;
		key.Port = WriterId(id);
		key.Tick = 0;
		key.Data = false;
		payload->Keys = {std::move(key)};
	}

	Document TwoOwners() {
		Document document;
		document.FormatVersion = 11;
		document.Nodes = {
			{"first", "pc.boolean", {}, {}, {{"value", true}}},
			{"second", "pc.boolean", {}, {}, {{"value", false}}}
		};
		document.Outputs = {{"first-value", "first", "boolean"}, {"second-value", "second", "boolean"}};
		document.SourceCommonOwners = {Owner("first", true), Owner("second", true)};
		Animator(document, "first", false);
		Animator(document, "second", false);
		return document;
	}
	EvaluationRequest NativeRequest() {
		EvaluationRequest request;
		request.SourceSafeMode = false;
		return request;
	}
	Plan Checked(const Document &document) {
		Plan plan;
		Diagnostic diagnostic;
		const auto status = Compile(document, plan, diagnostic);
		INFO(diagnostic.Message << " node=" << diagnostic.NodeId << " port=" << diagnostic.Port);
		REQUIRE(status == Status::Ok);
		return plan;
	}
	GroupRenderSession Cold(const Document &document) {
		GroupRenderSession session;
		Diagnostic diagnostic;
		const auto status = InitializeNativeSourceCommonRuntime(
			document, Checked(document), {}, SourceNodeInitialState::Loaded, session, diagnostic
		);
		INFO(diagnostic.Message);
		REQUIRE(status == Status::Ok);
		return session;
	}
	void Step(
		const Document &document,
		GroupRenderSession &session,
		const SourceCommonRuntimeObservations &observations = {}
	) {
		Diagnostic diagnostic;
		const auto status =
			NativeSourceStepBounded(document, Checked(document), {}, observations, session, diagnostic);
		INFO(diagnostic.Message << " node=" << diagnostic.NodeId << " port=" << diagnostic.Port);
		REQUIRE(status == Status::Ok);
	}
	bool Held(const GroupRenderSession &session, std::string_view id) {
		const auto node =
			std::find_if(session.Outputs.Nodes.begin(), session.Outputs.Nodes.end(), [&](const auto &row) {
				return row.NodeId == id;
			});
		REQUIRE(node != session.Outputs.Nodes.end());
		const auto output = std::find_if(node->Outputs.begin(), node->Outputs.end(), [](const auto &row) {
			return row.Port == "boolean";
		});
		REQUIRE(output != node->Outputs.end());
		REQUIRE(output->Data);
		REQUIRE(std::holds_alternative<bool>(*output->Data));
		return std::get<bool>(*output->Data);
	}
	bool NamedBoolean(const GroupRenderSession &session, std::string_view id) {
		const auto output =
			std::find_if(session.Replay.Outputs.begin(), session.Replay.Outputs.end(), [&](const auto &row) {
				return row.Id == id;
			});
		REQUIRE(output != session.Replay.Outputs.end());
		const auto *value = std::get_if<EvaluatedValue>(&output->Output);
		REQUIRE(value);
		REQUIRE(std::holds_alternative<bool>(value->Data));
		return std::get<bool>(value->Data);
	}
	void Same(const GroupRenderSession &actual, const GroupRenderSession &before) {
		CHECK(actual.Outputs == before.Outputs);
		CHECK(actual.Nodes == before.Nodes);
		CHECK(actual.Purities == before.Purities);
		CHECK(actual.Common == before.Common);
		CHECK(actual.CommonAnimators == before.CommonAnimators);
		CHECK(actual.SourceCommonWrites == before.SourceCommonWrites);
		CHECK(actual.SourceCommonBindings == before.SourceCommonBindings);
		CHECK(actual.SourceCommonInputs == before.SourceCommonInputs);
		CHECK(actual.Replay.Simulation == before.Replay.Simulation);
		CHECK(actual.Replay.Surfaces == before.Replay.Surfaces);
		CHECK(actual.Replay.Random == before.Replay.Random);
		CHECK(actual.Replay.Data == before.Replay.Data);
		CHECK(actual.Replay.Rigid == before.Replay.Rigid);
	}
}

TEST_CASE("Native common step uses durable owner order and current pulses", "[source_common]") {
	for (const bool reverse : {false, true}) {
		auto document = TwoOwners();
		document.Links = {{"first", "pxcx.updated_out_trigger", "second", "pxcx.update_in_trigger"}};
		if (reverse) std::reverse(document.SourceCommonOwners.begin(), document.SourceCommonOwners.end());
		auto session = Cold(document);
		Diagnostic diagnostic;
		REQUIRE(
			ProcessGroupRender(document, Checked(document), NativeRequest(), {}, session, diagnostic) ==
			Status::Ok
		);
		for (const auto &owner : session.Common.Owners)
			REQUIRE(owner.Updated);
		document.Nodes[1].Values[0].Data = true;
		Step(document, session);
		CHECK(Held(session, "second") == reverse);
		for (const auto &owner : session.Common.Owners)
			CHECK_FALSE(owner.Updated);
	}
}

TEST_CASE("Linked high Update retriggers after resetting only its local animator", "[source_common]") {
	auto document = TwoOwners();
	document.SourceCommonOwners[0].ShowUpdateTrigger = false;
	document.Links = {{"first", "boolean", "second", "pxcx.update_in_trigger"}};
	auto session = Cold(document);
	Diagnostic diagnostic;
	REQUIRE(
		ProcessGroupRender(document, Checked(document), NativeRequest(), {}, session, diagnostic) ==
		Status::Ok
	);
	const auto ready = session.Nodes;
	document.Nodes[1].Values[0].Data = true;
	Step(document, session);
	CHECK(Held(session, "second"));
	CHECK(NamedBoolean(session, "second-value"));
	document.Nodes[1].Values[0].Data = false;
	Step(document, session);
	CHECK_FALSE(Held(session, "second"));
	CHECK_FALSE(NamedBoolean(session, "second-value"));
	CHECK(Held(session, "first"));
	CHECK(session.Nodes == ready);
	REQUIRE(session.SourceCommonWrites.size() == 1);
	CHECK(session.SourceCommonWrites.front().OwnerId == "second");
	CHECK(session.SourceCommonWrites.front().Modified);
	CHECK(session.SourceCommonWrites.front().ForceDynamic);
}

TEST_CASE(
	"Metadata refusal after an earlier callback rolls back the whole common session", "[source_common]"
) {
	auto document = TwoOwners();
	document.SourceCommonOwners[1].ShowUpdateTrigger = false;
	document.SourceCommonOwners[1].OutMeta = true;
	RequestAtZero(document, "first");
	auto session = Cold(document);
	const auto before = session;
	Diagnostic diagnostic;
	CHECK(
		NativeSourceStepBounded(document, Checked(document), {}, {}, session, diagnostic) ==
		Status::UnsupportedExecution
	);
	Same(session, before);
	const std::array names{
		SourceCommonRuntimeMetadataObservation{"second", "Node_Boolean", "actual runtime name"}
	};
	Step(document, session, {names, {}});
	CHECK(Held(session, "first"));
	CHECK(session.Common.Owners[1].Name == "actual runtime name");
	const auto warm = session;
	CHECK(
		NativeSourceStepBounded(document, Checked(document), {}, {names, {}}, session, diagnostic, 1) ==
		Status::LimitExceeded
	);
	Same(session, warm);
}

TEST_CASE("Saved empty metadata names are proven and explicit reopen starts cold", "[source_common]") {
	auto document = TwoOwners();
	document.SourceCommonOwners[0].ShowUpdateTrigger = false;
	document.SourceCommonOwners[0].OutMeta = true;
	document.SourceCommonOwners[0].DisplayNamePresent = true;
	document.Nodes[0].SourceDisplayName = "";
	document.Nodes[0].Position = {11, 17};
	auto session = Cold(document);
	Step(document, session);
	CHECK(session.Common.Owners[0].Name.empty());
	CHECK(session.Common.Owners[0].Position == (Vector2{11, 17}));
	Document reopened;
	Diagnostic diagnostic;
	REQUIRE(Read(Write(document), reopened, diagnostic) == Status::Ok);
	REQUIRE(
		InitializeNativeSourceCommonRuntime(
			reopened, Checked(reopened), {}, SourceNodeInitialState::Loaded, session, diagnostic
		) == Status::Ok
	);
	for (const auto &owner : session.Common.Owners) {
		CHECK_FALSE(owner.Updated);
		CHECK(owner.Name.empty());
		CHECK(owner.Position == Vector2{});
	}
	Step(reopened, session);
	CHECK(session.Common.Owners[0].Position == (Vector2{11, 17}));
}

TEST_CASE("Disabled render policy does not suppress an active direct common callback", "[source_common]") {
	auto document = TwoOwners();
	document.Nodes[0].GroupId = "disabled";
	document.Groups = {{"disabled", "Disabled"}};
	document.Groups[0].RenderActive = false;
	RequestAtZero(document, "first");
	auto session = Cold(document);
	CHECK_FALSE(session.Ready("first"));
	CHECK_FALSE(Held(session, "first"));
	Step(document, session);
	CHECK(Held(session, "first"));
	CHECK_FALSE(session.Ready("first"));
	CHECK_FALSE(session.Common.Owners[0].Updated);
}

TEST_CASE(
	"Explicit common reconciliation preserves survivors while constructing and retiring owners",
	"[source_common]"
) {
	auto document = TwoOwners();
	document.SourceCommonOwners[0].OutMeta = true;
	document.SourceCommonOwners[0].DisplayNamePresent = true;
	document.Nodes[0].SourceDisplayName = "kept name";
	RequestAtZero(document, "first");
	auto session = Cold(document);
	Step(document, session);
	REQUIRE(Held(session, "first"));
	REQUIRE(session.Common.Owners[0].Name == "kept name");
	const auto oldAnimator = session.CommonAnimators.DetachedValues[0];

	document.Nodes.erase(document.Nodes.begin() + 1);
	document.Outputs.erase(document.Outputs.begin() + 1);
	document.SourceCommonOwners.erase(document.SourceCommonOwners.begin() + 1);
	document.SourceAnimators->Detached.erase(document.SourceAnimators->Detached.begin() + 1);
	document.SourceAnimators->DetachedValues.erase(document.SourceAnimators->DetachedValues.begin() + 1);
	document.Nodes.push_back({"new", "pc.boolean", {}, {}, {{"value", true}}});
	document.Outputs.push_back({"new-value", "new", "boolean"});
	document.SourceCommonOwners[0].ShowUpdateTrigger = false;
	document.SourceCommonOwners[0].OutMeta = false;
	document.SourceCommonOwners.insert(document.SourceCommonOwners.begin(), Owner("new", true));
	Animator(document, "new", false);
	Diagnostic diagnostic;
	REQUIRE(
		ReconcileNativeSourceCommonRuntime(
			document, Checked(document), {}, {SourceNodeInitialState::Constructed}, session, diagnostic
		) == Status::Ok
	);
	REQUIRE(session.Common.Owners.size() == 2);
	CHECK(session.Common.Owners[0].OwnerId == "new");
	CHECK(session.Common.Owners[0].Name.empty());
	CHECK_FALSE(session.Common.Owners[0].Updated);
	CHECK_FALSE(Held(session, "new"));
	CHECK(session.Common.Owners[1].OwnerId == "first");
	CHECK(session.Common.Owners[1].Name == "kept name");
	CHECK(Held(session, "first"));
	const auto kept = std::find_if(
		session.CommonAnimators.DetachedValues.begin(),
		session.CommonAnimators.DetachedValues.end(),
		[](const auto &row) { return row.NodeId == "first"; }
	);
	REQUIRE(kept != session.CommonAnimators.DetachedValues.end());
	CHECK(*kept == oldAnimator);
	CHECK(std::none_of(session.Outputs.Nodes.begin(), session.Outputs.Nodes.end(), [](const auto &row) {
		return row.NodeId == "second";
	}));

	document.Nodes[0].Type = "pc.number_simple";
	document.Nodes[0].Values = {{"value", 42.0}};
	document.Outputs[0].Port = "number";
	document.SourceCommonOwners[1].SourceType = "Node_Number_Simple";
	REQUIRE(
		ReconcileNativeSourceCommonRuntime(
			document, Checked(document), {}, {SourceNodeInitialState::Constructed}, session, diagnostic
		) == Status::Ok
	);
	CHECK(session.Common.Owners[1].Name.empty());
	CHECK_FALSE(session.Common.Owners[1].Updated);
	CHECK_FALSE(session.Ready("first"));
}

TEST_CASE("Animated false key presence still invokes the common direct callback", "[source_common]") {
	auto document = TwoOwners();
	auto &writer = document.SourceAnimators->Detached[0];
	writer.Writer = GroupSubtypeAnimator::Animated;
	auto &payload = document.SourceAnimators->DetachedValues[0];
	payload.Fixed.reset();
	Keyframe key;
	key.NodeId = "first";
	key.Port = WriterId("first");
	key.Tick = 0;
	key.Data = false;
	payload.Keys = {key};
	auto session = Cold(document);
	CHECK_FALSE(Held(session, "first"));
	Step(document, session);
	CHECK(Held(session, "first"));
	document.Nodes[0].Values[0].Data = false;
	Step(document, session);
	CHECK_FALSE(Held(session, "first"));
	REQUIRE(session.CommonAnimators.DetachedValues[0].Keys.size() == 1);
	CHECK_FALSE(std::get<bool>(session.CommonAnimators.DetachedValues[0].Keys[0].Data));
}

TEST_CASE(
	"Collection pending flags are explicit and its override preserves common sockets", "[source_common]"
) {
	auto document = TwoOwners();
	document.Groups = {{"collection", "Collection metadata is not base metadata"}};
	document.Nodes[0].GroupId = "collection";
	auto collection = Owner("collection", true, true);
	collection.SourceType = "Node_Collection";
	collection.NativeOwnerKind = SourceCommonNativeOwnerKind::Group;
	collection.DisplayNamePresent = true;
	document.SourceCommonOwners.insert(document.SourceCommonOwners.begin(), collection);
	Animator(document, "collection", true);
	auto session = Cold(document);
	const auto before = session;
	Diagnostic diagnostic;
	CHECK(
		NativeSourceStepBounded(document, Checked(document), {}, {}, session, diagnostic) ==
		Status::UnsupportedExecution
	);
	Same(session, before);
	const std::array observed{SourceCommonRuntimeCollectionStepObservation{"collection", "Node_Collection"}};
	Step(document, session, {{}, observed});
	CHECK(session.Common.Owners[0].Name.empty());
	CHECK_FALSE(session.Common.Owners[0].Updated);
	CHECK(
		session.CommonAnimators.DetachedValues.back().Fixed ==
		before.CommonAnimators.DetachedValues.back().Fixed
	);

	const std::array pending{SourceCommonRuntimeCollectionStepObservation{
		"collection",
		"Node_Collection",
		true,
		false,
		false,
		SourcePurityRefresh{SourcePurityRefreshEvent::LoadTopology}
	}};
	Step(document, session, {{}, pending});
	CHECK(session.Common.Owners[0].Name.empty());
	CHECK_FALSE(session.Common.Owners[0].Updated);
	const auto purity = std::find_if(session.Purities.begin(), session.Purities.end(), [](const auto &row) {
		return row.GroupId == "collection";
	});
	REQUIRE(purity != session.Purities.end());
	CHECK(purity->State == SourceGroupPurity::Pure);
}

TEST_CASE("Native common getter observations do not execute or mutate their owner", "[source_common]") {
	auto document = TwoOwners();
	document.SourceCommonOwners[0].OutMeta = true;
	document.SourceCommonOwners[0].DisplayNamePresent = true;
	document.Nodes[0].SourceDisplayName = "observed";
	auto session = Cold(document);
	Step(document, session);
	const auto before = session;
	EvaluatedValue result;
	Diagnostic diagnostic;
	REQUIRE(
		ReadNativeSourceCommonGetter(
			document, Checked(document), "first", SourceCommonSelector::Name, {}, session, result, diagnostic
		) == Status::Ok
	);
	CHECK(std::get<std::string>(result.Data) == "observed");
	REQUIRE(result.Domain);
	CHECK(result.Domain->Type == ValueType::Text);
	Same(session, before);
	const auto oldResult = result;
	CHECK(
		ReadNativeSourceCommonGetter(
			document,
			Checked(document),
			"first",
			SourceCommonSelector::Name,
			{},
			session,
			result,
			diagnostic,
			1
		) == Status::LimitExceeded
	);
	CHECK(result == oldResult);
	Same(session, before);
}

TEST_CASE("Static Trigger storage does not manufacture an Update request", "[source_common]") {
	auto document = TwoOwners();
	document.SourceAnimators->DetachedValues[0].Fixed = true;
	auto session = Cold(document);
	Step(document, session);
	CHECK_FALSE(Held(session, "first"));
	CHECK(session.SourceCommonWrites.empty());
	CHECK_FALSE(session.Ready("first"));
	EvaluatedValue getter;
	Diagnostic diagnostic;
	REQUIRE(
		ReadNativeSourceCommonGetter(
			document,
			Checked(document),
			"first",
			SourceCommonSelector::Update,
			{},
			session,
			getter,
			diagnostic
		) == Status::Ok
	);
	CHECK_FALSE(std::get<bool>(getter.Data));
}

TEST_CASE(
	"Common expression reads the previous actual getInputs map before direct refresh", "[source_common]"
) {
	auto document = TwoOwners();
	document.SourceCommonOwners[1].ShowUpdateTrigger = false;
	document.SourceCommonOwners[0].UpdateExpression =
		SourceInputExpression{"pxcx.update_in_trigger", "self.value", true};
	auto session = Cold(document);
	const auto cold = session;
	Diagnostic diagnostic;
	CHECK(
		NativeSourceStepBounded(document, Checked(document), {}, {}, session, diagnostic) ==
		Status::UnsupportedExecution
	);
	CHECK(diagnostic.Message.find("input_value_map") != std::string::npos);
	Same(session, cold);

	document.SourceCommonOwners[0].ShowUpdateTrigger = false;
	REQUIRE(
		ProcessGroupRender(document, Checked(document), NativeRequest(), {}, session, diagnostic) ==
		Status::Ok
	);
	REQUIRE(Held(session, "first"));
	document.SourceCommonOwners[0].ShowUpdateTrigger = true;
	document.Nodes[0].Values[0].Data = false;
	Step(document, session);
	CHECK_FALSE(Held(session, "first"));
	CHECK_FALSE(NamedBoolean(session, "first-value"));
	const auto captured = session.SourceCommonInputs;
	document.Nodes[0].Values[0].Data = true;
	Step(document, session);
	CHECK_FALSE(Held(session, "first"));
	CHECK_FALSE(NamedBoolean(session, "first-value"));
	CHECK(session.SourceCommonInputs == captured);
}

TEST_CASE(
	"Source common instance getter inheritance yields to local override and grouped incoming route",
	"[source_common]"
) {
	for (int mode = 0; mode != 3; ++mode) {
		INFO(mode);
		auto document = TwoOwners();
		document.SourceCommonOwners[0].ShowUpdateTrigger = false;
		document.SourceCommonOwners[1].InstanceBase = "first";
		document.SourceCommonOwners[1].UpdateOverrideInstance = mode == 1;
		RequestAtZero(document, "first");
		document.Nodes[1].Values[0].Data = true;
		if (mode == 2) {
			document.Groups = {{"scope", "Scope"}};
			for (auto &node : document.Nodes)
				node.GroupId = "scope";
			document.Junctions = {{"update-wire", "scope", ValueType::Boolean, std::nullopt}};
			document.Links = {
				{"first", "boolean", "update-wire", "value"},
				{"update-wire", "value", "second", "pxcx.update_in_trigger"}
			};
		}
		auto session = Cold(document);
		const auto base = session.CommonAnimators.DetachedValues.front();
		EvaluatedValue observed;
		Diagnostic diagnostic;
		REQUIRE(
			ReadNativeSourceCommonGetter(
				document,
				Checked(document),
				"second",
				SourceCommonSelector::Update,
				{},
				session,
				observed,
				diagnostic
			) == Status::Ok
		);
		CHECK(std::get<bool>(observed.Data) == (mode == 0));
		Step(document, session);
		CHECK(Held(session, "second") == (mode == 0));
		CHECK(NamedBoolean(session, "second-value") == (mode == 0));
		CHECK(session.CommonAnimators.DetachedValues.front() == base);
	}
}

TEST_CASE(
	"Retiring the last common owner preserves canonical journals and starts readded IDs cold",
	"[source_common]"
) {
	for (const bool keepOrdinary : {false, true}) {
		INFO(keepOrdinary);
		auto document = TwoOwners();
		RequestAtZero(document, "first");
		if (keepOrdinary) {
			document.Nodes.push_back({"ordinary", "pc.boolean", {}, {}, {{"value", true}}});
			document.Outputs.push_back({"ordinary-value", "ordinary", "boolean"});
		}
		const auto authored = document;
		auto session = Cold(document);
		Diagnostic diagnostic;
		REQUIRE(
			ProcessGroupRender(document, Checked(document), NativeRequest(), {}, session, diagnostic) ==
			Status::Ok
		);
		Step(document, session);
		REQUIRE(Held(session, "first"));
		REQUIRE_FALSE(session.SourceCommonWrites.empty());
		REQUIRE(session.SourceCommonInputs.front().Inputs);

		SimulationReplayState simulation;
		simulation.Entries.emplace_back();
		simulation.Entries.back().NodeId = "canonical-simulation";
		simulation.Entries.back().State.Initialized = true;
		simulation.Entries.back().State.Tick = 7;
		SurfaceFrameReplayState surfaces;
		surfaces.Initialized = true;
		surfaces.Tick = 7;
		surfaces.Entries.push_back({"canonical-surface", 7, 0, Image{1, 1, {1, 2, 3, 255}}});
		RandomReplayState random;
		random.Entries.emplace_back();
		random.Entries.back().NodeId = "canonical-random";
		random.Entries.back().Initialized = true;
		random.Entries.back().StoredSeed = 19;
		DataReplayState data;
		data.Entries.emplace_back();
		data.Entries.back().NodeId = "canonical-data";
		data.Entries.back().Initialized = true;
		data.Entries.back().Values.push_back({7, 23.0});
		RigidReplayState rigid;
		rigid.Owners.emplace_back();
		rigid.Owners.back().History.OwnerId = "canonical-rigid";
		rigid.Owners.back().History.Frames.emplace_back();
		REQUIRE(
			ValidateSimulationReplay(simulation, Limits::MaximumEvaluationBytes, diagnostic) == Status::Ok
		);
		REQUIRE(
			ValidateSurfaceFrameReplay(surfaces, Limits::MaximumEvaluationBytes, diagnostic) == Status::Ok
		);
		REQUIRE(ValidateRandomReplay(random, Limits::MaximumEvaluationBytes, diagnostic) == Status::Ok);
		REQUIRE(ValidateDataReplay(data, Limits::MaximumEvaluationBytes, diagnostic) == Status::Ok);
		REQUIRE(ValidateRigidReplay(rigid, Limits::MaximumEvaluationBytes, diagnostic) == Status::Ok);
		auto request = NativeRequest();
		request.SimulationReplay = &simulation;
		request.SurfaceReplay = &surfaces;
		request.RandomReplay = &random;
		request.DataReplay = &data;
		request.RigidReplay = &rigid;
		const auto preserved = [&] {
			CHECK(session.Replay.Simulation == simulation);
			CHECK(session.Replay.Surfaces == surfaces);
			CHECK(session.Replay.Random == random);
			CHECK(session.Replay.Data == data);
			CHECK(session.Replay.Rigid == rigid);
		};

		document.Nodes.erase(document.Nodes.begin(), document.Nodes.begin() + 2);
		document.Outputs.erase(document.Outputs.begin(), document.Outputs.begin() + 2);
		document.SourceCommonOwners.clear();
		document.SourceAnimators = OwnedPayload3D<SourceAnimatorState>{};
		Plan runtime;
		REQUIRE(CompileSourceCommonRuntime(document, runtime, diagnostic) == Status::Ok);
		Plan ordinary;
		CHECK(Compile(document, ordinary, diagnostic) == (keepOrdinary ? Status::Ok : Status::InvalidOutput));
		REQUIRE(
			ReconcileNativeSourceCommonRuntime(
				document, runtime, request, {SourceNodeInitialState::Constructed}, session, diagnostic
			) == Status::Ok
		);
		CHECK(session.Common.Owners.empty());
		CHECK(session.CommonAnimators.Detached.empty());
		CHECK(session.CommonAnimators.DetachedValues.empty());
		CHECK(session.SourceCommonWrites.empty());
		CHECK(session.SourceCommonBindings.empty());
		CHECK(session.SourceCommonInputs.empty());
		CHECK(session.Outputs.Nodes.size() == size_t(keepOrdinary));
		CHECK(session.Nodes.size() == size_t(keepOrdinary));
		CHECK(session.Replay.Outputs.size() == size_t(keepOrdinary));
		if (keepOrdinary) {
			CHECK(Held(session, "ordinary"));
			CHECK(NamedBoolean(session, "ordinary-value"));
		}
		preserved();

		auto malformed = document;
		malformed.Nodes.push_back({"invalid", "unknown.native"});
		CHECK(CompileSourceCommonRuntime(malformed, runtime, diagnostic) == Status::UnknownNode);
		document.Nodes.push_back(authored.Nodes[0]);
		document.Outputs.push_back(authored.Outputs[0]);
		document.SourceCommonOwners.push_back(authored.SourceCommonOwners[0]);
		document.SourceAnimators = authored.SourceAnimators;
		document.SourceAnimators->Detached.resize(1);
		document.SourceAnimators->DetachedValues.resize(1);
		REQUIRE(CompileSourceCommonRuntime(document, runtime, diagnostic) == Status::Ok);
		REQUIRE(
			ReconcileNativeSourceCommonRuntime(
				document, runtime, request, {SourceNodeInitialState::Constructed}, session, diagnostic
			) == Status::Ok
		);
		CHECK_FALSE(Held(session, "first"));
		CHECK_FALSE(session.Ready("first"));
		CHECK_FALSE(NamedBoolean(session, "first-value"));
		REQUIRE(session.Common.Owners.size() == 1);
		CHECK_FALSE(session.Common.Owners.front().Updated);
		CHECK(session.Common.Owners.front().Name.empty());
		REQUIRE(session.SourceCommonInputs.size() == 1);
		CHECK_FALSE(session.SourceCommonInputs.front().Inputs);
		CHECK(session.SourceCommonWrites.empty());
		preserved();
	}
}
