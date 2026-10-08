#include "SourceCommonMembership.hpp"

#include <engine/imagegraph/SourceCommonAnimatorReset.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

TEST_SUITE_ID("engine.imagegraph.source_common_membership")
using namespace engine::imagegraph;
namespace {
	void AddOwner(Document &document, std::string id, std::string type = "Node_Number") {
		SourceCommonOwnerRecord owner;
		owner.SourceOwnerId = id;
		owner.SourceType = std::move(type);
		owner.NativeOwnerId = "native-" + id;
		owner.UpdateAnimatorOwnerId = id;
		owner.UpdateAnimatorPort = "update-" + id;
		document.SourceCommonOwners.push_back(owner);
		if (!document.SourceAnimators) document.SourceAnimators.emplace();
		DetachedSourceAnimator animator;
		animator.OwnerId = id;
		animator.Id = owner.UpdateAnimatorPort;
		animator.OriginalPort = "pxcx.update_in_trigger";
		animator.Type = ValueType::Boolean;
		document.SourceAnimators->Detached.push_back(animator);
		GroupSubtypeOverlay payload;
		payload.NodeId = id;
		payload.Port = animator.Id;
		payload.Fixed = true;
		document.SourceAnimators->DetachedValues.push_back(std::move(payload));
	}
	Status Reconcile(
		const Document &document,
		GroupRenderSession &session,
		Diagnostic &diagnostic,
		std::span<const SourceCommonWriterIdentity> resets = {},
		uint64_t cap = Limits::MaximumEvaluationBytes
	) {
		detail::EvaluationBudget budget(cap);
		auto charge = budget.Reserve(RetainedGroupRenderSessionBytes(session));
		if (!charge) return Status::LimitExceeded;
		const uint64_t originalCharge = charge->Bytes();
		const auto status = detail::ReconcileSourceCommonMembership(
			document, SourceNodeInitialState::Constructed, session, budget, *charge, diagnostic, resets
		);
		if (status != Status::Ok)
			CHECK(charge->Bytes() == originalCharge);
		else
			CHECK(charge->Bytes() == RetainedGroupRenderSessionBytes(session));
		return status;
	}
	void Warm(GroupRenderSession &session, size_t index) {
		auto &socket = session.Common.Owners[index];
		socket.Updated = true;
		socket.Name = "held-" + socket.OwnerId;
		socket.Position = {11, 19};
		auto &payload = session.CommonAnimators.DetachedValues[index];
		payload.Fixed = false;
		session.SourceCommonWrites.push_back({payload.NodeId, payload.Port, true, true, true});
	}
}
TEST_CASE("Common structural edit keeps survivors warm and adopts source order", "[source_common]") {
	Document document;
	AddOwner(document, "first");
	AddOwner(document, "second");
	AddOwner(document, "retired");
	GroupRenderSession session;
	Diagnostic diagnostic;
	REQUIRE(Reconcile(document, session, diagnostic) == Status::Ok);
	Warm(session, 0);
	Warm(session, 1);
	Warm(session, 2);
	const auto heldFirst = session.Common.Owners[0];
	const auto heldSecond = session.Common.Owners[1];
	document.SourceCommonOwners.pop_back();
	std::swap(document.SourceCommonOwners[0], document.SourceCommonOwners[1]);
	AddOwner(document, "new");
	REQUIRE(Reconcile(document, session, diagnostic) == Status::Ok);
	REQUIRE(session.Common.Owners.size() == 3);
	CHECK(session.Common.Owners[0] == heldSecond);
	CHECK(session.Common.Owners[1] == heldFirst);
	CHECK(session.Common.Owners[2] == SourceCommonSocketState{"new", "Node_Number", false, {}, {}});
	CHECK(session.SourceCommonWrites.size() == 2);
	REQUIRE(session.CommonAnimators.DetachedValues.size() == 3);
	CHECK(std::get<bool>(*session.CommonAnimators.DetachedValues[0].Fixed) == false);
	CHECK(std::get<bool>(*session.CommonAnimators.DetachedValues[1].Fixed) == false);
	CHECK(std::get<bool>(*session.CommonAnimators.DetachedValues[2].Fixed) == true);
}
TEST_CASE("Common authored flag edits leave held metadata and pending setter effects", "[source_common]") {
	Document document;
	AddOwner(document, "owner");
	GroupRenderSession session;
	Diagnostic diagnostic;
	REQUIRE(Reconcile(document, session, diagnostic) == Status::Ok);
	Warm(session, 0);
	const auto held = session.Common;
	const auto effects = session.SourceCommonWrites;
	auto &owner = document.SourceCommonOwners.front();
	owner.Active = false;
	owner.ShowUpdateTrigger = true;
	owner.OutMeta = true;
	owner.DisplayNamePresent = true;
	owner.UpdateOverrideInstance = true;
	owner.UpdateGraph = false;
	document.SourceAnimators->Detached.front().Writer = GroupSubtypeAnimator::Animated;
	REQUIRE(Reconcile(document, session, diagnostic) == Status::Ok);
	CHECK(session.Common == held);
	CHECK(session.SourceCommonWrites == effects);
	CHECK(session.CommonAnimators.Detached.front().Writer == GroupSubtypeAnimator::Animated);
}
TEST_CASE(
	"Explicit common writer edit replaces only that physical payload with all authored keys",
	"[source_common]"
) {
	Document document;
	AddOwner(document, "edited");
	AddOwner(document, "untouched");
	GroupRenderSession session;
	Diagnostic diagnostic;
	REQUIRE(Reconcile(document, session, diagnostic) == Status::Ok);
	Warm(session, 0);
	Warm(session, 1);
	const auto common = session.Common;
	auto &canonical = document.SourceAnimators->DetachedValues.front();
	canonical.Fixed.reset();
	for (uint64_t tick : {2, 8, 21}) {
		Keyframe key;
		key.NodeId = canonical.NodeId;
		key.Port = canonical.Port;
		key.Tick = tick;
		key.Data = true;
		key.SourceKeyId = "key-" + std::to_string(tick);
		canonical.Keys.push_back(std::move(key));
	}
	document.SourceAnimators->Detached.front().Writer = GroupSubtypeAnimator::Animated;
	const SourceCommonWriterIdentity reset{"edited", "update-edited"};
	REQUIRE(Reconcile(document, session, diagnostic, {&reset, 1}) == Status::Ok);
	CHECK(session.Common == common);
	CHECK(session.CommonAnimators.DetachedValues.front() == canonical);
	CHECK(std::get<bool>(*session.CommonAnimators.DetachedValues.back().Fixed) == false);
	REQUIRE(session.SourceCommonWrites.size() == 1);
	CHECK(session.SourceCommonWrites.front().OwnerId == "untouched");
}
TEST_CASE("Common membership keeps actual animated reset keys on untouched writers", "[source_common]") {
	Document document;
	AddOwner(document, "animated");
	auto &animator = document.SourceAnimators->Detached.front();
	animator.Writer = GroupSubtypeAnimator::Animated;
	AnimationTrack track;
	track.NodeId = animator.OwnerId;
	track.Port = animator.Id;
	animator.Track = track;
	auto &authoredPayload = document.SourceAnimators->DetachedValues.front();
	authoredPayload.Fixed.reset();
	for (uint64_t tick : {2, 9}) {
		Keyframe key;
		key.NodeId = authoredPayload.NodeId;
		key.Port = authoredPayload.Port;
		key.Tick = tick;
		key.Data = true;
		authoredPayload.Keys.push_back(std::move(key));
	}
	GroupRenderSession session;
	Diagnostic diagnostic;
	REQUIRE(Reconcile(document, session, diagnostic) == Status::Ok);
	SourceCommonAnimatorResetReceipt receipt;
	auto &retainedPayload = session.CommonAnimators.DetachedValues.front();
	REQUIRE(
		ResetSourceCommonAnimator(animator, retainedPayload, {{5}}, retainedPayload, receipt, diagnostic) ==
		Status::Ok
	);
	REQUIRE(retainedPayload.Keys.size() == 3);
	CHECK(std::get<bool>(retainedPayload.Keys[1].Data) == false);
	session.SourceCommonWrites.push_back(
		{animator.OwnerId, animator.Id, receipt.Modified, receipt.ForceDynamic, receipt.Edited}
	);
	const auto retained = retainedPayload;
	const auto effects = session.SourceCommonWrites;
	document.SourceCommonOwners.front().OutMeta = true;
	AddOwner(document, "new");
	std::swap(document.SourceCommonOwners[0], document.SourceCommonOwners[1]);
	REQUIRE(Reconcile(document, session, diagnostic) == Status::Ok);
	CHECK(session.CommonAnimators.DetachedValues[1] == retained);
	CHECK(session.CommonAnimators.Detached[1].Track == document.SourceAnimators->Detached.front().Track);
	CHECK(session.SourceCommonWrites == effects);
	CHECK(document.SourceAnimators->DetachedValues.front().Keys.size() == 2);
}
TEST_CASE("Explicit type or native binding replacement constructs cold common outputs", "[source_common]") {
	Document document;
	AddOwner(document, "type");
	AddOwner(document, "binding");
	AddOwner(document, "survivor");
	GroupRenderSession session;
	Diagnostic diagnostic;
	REQUIRE(Reconcile(document, session, diagnostic) == Status::Ok);
	Warm(session, 0);
	Warm(session, 1);
	Warm(session, 2);
	const auto held = session.Common.Owners.back();
	document.SourceCommonOwners[0].SourceType = "Node_Boolean";
	document.SourceCommonOwners[1].NativeOwnerKind = SourceCommonNativeOwnerKind::Group;
	document.SourceCommonOwners[1].NativeOwnerId = "new-group";
	REQUIRE(Reconcile(document, session, diagnostic) == Status::Ok);
	CHECK(session.Common.Owners[0] == SourceCommonSocketState{"type", "Node_Boolean", false, {}, {}});
	CHECK(session.Common.Owners[1] == SourceCommonSocketState{"binding", "Node_Number", false, {}, {}});
	CHECK(session.Common.Owners[2] == held);
	REQUIRE(session.SourceCommonWrites.size() == 1);
	CHECK(session.SourceCommonWrites.front().OwnerId == "survivor");
}
TEST_CASE(
	"Common reconciliation refusal preserves all selected state and unrelated readiness", "[source_common]"
) {
	Document document;
	AddOwner(document, "owner");
	GroupRenderSession session;
	Diagnostic diagnostic;
	REQUIRE(Reconcile(document, session, diagnostic) == Status::Ok);
	Warm(session, 0);
	session.Nodes.push_back({"native-owner", true, SourceFrameActivity::Static});
	session.SourceCommonInputs.front().Inputs.emplace().Data.emplace().Fields = {
		{"previous", double{5}}, {"text", std::string(300, 'h')}
	};
	const auto common = session.Common;
	const auto writers = session.CommonAnimators;
	const auto bindings = session.SourceCommonBindings;
	const auto effects = session.SourceCommonWrites;
	const auto readiness = session.Nodes;
	const auto inputs = session.SourceCommonInputs;
	AddOwner(document, "new");
	const uint64_t cap = RetainedGroupRenderSessionBytes(session);
	CHECK(Reconcile(document, session, diagnostic, {}, cap) == Status::LimitExceeded);
	CHECK(session.Common == common);
	CHECK(session.CommonAnimators == writers);
	CHECK(session.SourceCommonBindings == bindings);
	CHECK(session.SourceCommonWrites == effects);
	CHECK(session.Nodes == readiness);
	CHECK(session.SourceCommonInputs == inputs);
	const SourceCommonWriterIdentity unknown{"absent", "missing"};
	CHECK(Reconcile(document, session, diagnostic, {&unknown, 1}) == Status::UnknownPort);
	CHECK(session.Common == common);
	CHECK(session.CommonAnimators == writers);
	CHECK(session.SourceCommonBindings == bindings);
	CHECK(session.SourceCommonWrites == effects);
	CHECK(session.Nodes == readiness);
	CHECK(session.SourceCommonInputs == inputs);
}
TEST_CASE(
	"Common membership preserves actual held input maps and constructs unavailable maps", "[source_common]"
) {
	Document document;
	AddOwner(document, "warm");
	AddOwner(document, "removed");
	AddOwner(document, "retyped");
	AddOwner(document, "rebound");
	AddOwner(document, "unavailable");
	GroupRenderSession session;
	Diagnostic diagnostic;
	REQUIRE(Reconcile(document, session, diagnostic) == Status::Ok);
	REQUIRE(session.SourceCommonInputs.size() == 5);
	for (const auto &row : session.SourceCommonInputs)
		CHECK_FALSE(row.Inputs.has_value());
	StructValue actual;
	actual.Data.emplace().Fields = {{"previous", double{7}}, {"label", std::string(400, 'a')}};
	session.SourceCommonInputs[0].Inputs = actual;
	session.SourceCommonInputs[1].Inputs = actual;
	session.SourceCommonInputs[2].Inputs = actual;
	session.SourceCommonInputs[3].Inputs = actual;
	const auto heldWarm = session.SourceCommonInputs[0];
	document.SourceCommonOwners.erase(document.SourceCommonOwners.begin() + 1);
	document.SourceCommonOwners[1].SourceType = "Node_Boolean";
	document.SourceCommonOwners[2].NativeOwnerId = "new-native-binding";
	document.SourceCommonOwners.front().ShowUpdateTrigger = true;
	AddOwner(document, "new");
	std::reverse(document.SourceCommonOwners.begin(), document.SourceCommonOwners.end());
	const SourceCommonWriterIdentity edited{"warm", "update-warm"};
	REQUIRE(Reconcile(document, session, diagnostic, {&edited, 1}) == Status::Ok);
	REQUIRE(session.SourceCommonInputs.size() == 5);
	CHECK(session.SourceCommonInputs.back() == heldWarm);
	for (size_t index = 0; index + 1 < session.SourceCommonInputs.size(); ++index) {
		CHECK_FALSE(session.SourceCommonInputs[index].Inputs.has_value());
		CHECK(session.SourceCommonInputs[index].OwnerId == document.SourceCommonOwners[index].SourceOwnerId);
		CHECK(session.SourceCommonInputs[index].OwnerType == document.SourceCommonOwners[index].SourceType);
	}
	CHECK(
		std::none_of(
			session.SourceCommonInputs.begin(), session.SourceCommonInputs.end(), [](const auto &row) {
				return row.OwnerId == "removed";
			}
		)
	);
}
