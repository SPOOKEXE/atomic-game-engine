#include "SourceAnimatorPersistence.hpp"
#include "SourceCommonAnimatorResetInternal.hpp"

#include <engine/imagegraph/SourceCommonAnimatorReset.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.imagegraph.source_common_animator_reset")
using namespace engine::imagegraph;
namespace {
	DetachedSourceAnimator Animator(GroupSubtypeAnimator writer) {
		DetachedSourceAnimator animator;
		animator.OwnerId = "local";
		animator.Id = "update-storage";
		animator.OriginalPort = "pxcx.update_in_trigger";
		animator.Type = ValueType::Boolean;
		animator.Writer = writer;
		return animator;
	}
	GroupSubtypeOverlay Payload() {
		GroupSubtypeOverlay payload;
		payload.NodeId = "local";
		payload.Port = "update-storage";
		return payload;
	}
	Keyframe Key(uint64_t tick, bool value = true) {
		Keyframe key;
		key.NodeId = "local";
		key.Port = "update-storage";
		key.Tick = tick;
		key.Data = value;
		return key;
	}
}
TEST_CASE("Common static reset replaces physical slot zero and preserves later keys", "[source_common]") {
	auto payload = Payload();
	payload.Keys = {Key(7), Key(13)};
	payload.Keys.front().SourceKeyId = "retired";
	const auto later = payload.Keys.back();
	SourceCommonAnimatorResetReceipt receipt;
	Diagnostic diagnostic;
	REQUIRE(
		ResetSourceCommonAnimator(
			Animator(GroupSubtypeAnimator::Static), payload, {{9}}, payload, receipt, diagnostic
		) == Status::Ok
	);
	REQUIRE(payload.Keys.size() == 2);
	CHECK(payload.Keys.front().Tick == 0);
	CHECK(std::get<bool>(payload.Keys.front().Data) == false);
	CHECK(payload.Keys.front().SourceKeyId.empty());
	CHECK(payload.Keys.back() == later);
	CHECK(receipt.Modified);
	CHECK(receipt.ForceDynamic);
	CHECK(receipt.Edited);
	CHECK(receipt.AnimatorReturnedTrue);
}
TEST_CASE(
	"Common animated reset preserves key override semantics and false key presence", "[source_common]"
) {
	auto payload = Payload();
	payload.Keys = {Key(2), Key(8)};
	const auto animator = Animator(GroupSubtypeAnimator::Animated);
	SourceCommonAnimatorResetReceipt receipt;
	Diagnostic diagnostic;
	REQUIRE(ResetSourceCommonAnimator(animator, payload, {{5}}, payload, receipt, diagnostic) == Status::Ok);
	REQUIRE(payload.Keys.size() == 3);
	CHECK(payload.Keys[1].Tick == 5);
	CHECK_FALSE(std::get<bool>(payload.Keys[1].Data));
	CHECK(receipt.AnimatorReturnedTrue);
	payload.Keys[1].Data = true;
	payload.Keys[1].SourceKeyId = "retained";
	REQUIRE(
		ResetSourceCommonAnimator(animator, payload, {{5}, false}, payload, receipt, diagnostic) == Status::Ok
	);
	CHECK(std::get<bool>(payload.Keys[1].Data));
	CHECK_FALSE(receipt.Edited);
	REQUIRE(
		ResetSourceCommonAnimator(animator, payload, {{5}, true}, payload, receipt, diagnostic) == Status::Ok
	);
	CHECK_FALSE(std::get<bool>(payload.Keys[1].Data));
	CHECK(payload.Keys[1].SourceKeyId == "retained");
	CHECK_FALSE(receipt.AnimatorReturnedTrue);
	CHECK_FALSE(receipt.Edited);
	REQUIRE(
		ResetSourceCommonAnimator(animator, payload, {{5}, false, true}, payload, receipt, diagnostic) ==
		Status::Ok
	);
	CHECK(receipt.Edited);
}
TEST_CASE("Common local reset refuses bad identity clock and budget without publication", "[source_common]") {
	auto payload = Payload();
	payload.Fixed = true;
	const auto before = payload;
	SourceCommonAnimatorResetReceipt receipt{false, false, false, false};
	const auto priorReceipt = receipt;
	Diagnostic diagnostic;
	auto animator = Animator(GroupSubtypeAnimator::Animated);
	animator.OwnerId = "delegated-getter";
	CHECK(
		ResetSourceCommonAnimator(animator, payload, {}, payload, receipt, diagnostic) == Status::InvalidValue
	);
	animator = Animator(GroupSubtypeAnimator::Animated);
	CHECK(
		ResetSourceCommonAnimator(animator, payload, {{1, 0.5}}, payload, receipt, diagnostic) ==
		Status::UnsupportedExecution
	);
	CHECK(
		ResetSourceCommonAnimator(animator, payload, {}, payload, receipt, diagnostic, 1) ==
		Status::LimitExceeded
	);
	CHECK(payload == before);
	CHECK(receipt == priorReceipt);
}

TEST_CASE(
	"Common Fixed storage becomes physical Trigger keys under bounded long identities", "[source_common]"
) {
	auto animator = Animator(GroupSubtypeAnimator::Animated);
	animator.OwnerId = std::string(2048, 'o');
	animator.Id = std::string(2048, 'p');
	auto payload = Payload();
	payload.NodeId = animator.OwnerId;
	payload.Port = animator.Id;
	payload.Fixed = true;
	SourceAnimatorState state;
	state.Detached = {animator};
	state.DetachedValues = {payload};
	const auto measured = detail::SourceAnimatorStateBytes(state, false);
	REQUIRE(measured);
	const auto borrowed = *measured - sizeof(state);
	const auto tight = borrowed + sizeof(payload) + payload.NodeId.capacity() + payload.Port.capacity() +
					   2 * sizeof(Keyframe) + payload.NodeId.size() + payload.Port.size() + 128;
	const auto before = payload;
	SourceCommonAnimatorResetReceipt receipt{false, false, false, false};
	const auto prior = receipt;
	Diagnostic diagnostic;
	CHECK(
		ResetSourceCommonAnimator(animator, payload, {{9}}, payload, receipt, diagnostic, tight) ==
		Status::LimitExceeded
	);
	CHECK(payload == before);
	CHECK(receipt == prior);
	REQUIRE(
		ResetSourceCommonAnimator(
			animator,
			payload,
			{{9}},
			payload,
			receipt,
			diagnostic,
			tight + 2 * (payload.NodeId.size() + payload.Port.size()) + 4096
		) == Status::Ok
	);
	CHECK_FALSE(payload.Fixed);
	REQUIRE(payload.Keys.size() == 2);
	CHECK(payload.Keys[0].Tick == 0);
	CHECK(std::get<bool>(payload.Keys[0].Data));
	CHECK(payload.Keys[1].Tick == 9);
	CHECK_FALSE(std::get<bool>(payload.Keys[1].Data));
	payload = before;
	animator.Writer = GroupSubtypeAnimator::Static;
	REQUIRE(ResetSourceCommonAnimator(animator, payload, {{9}}, payload, receipt, diagnostic) == Status::Ok);
	CHECK_FALSE(payload.Fixed);
	REQUIRE(payload.Keys.size() == 1);
	CHECK(payload.Keys.front().Tick == 0);
	CHECK_FALSE(std::get<bool>(payload.Keys.front().Data));
}
TEST_CASE("Common budgeted reset updates only its selected candidate charge", "[source_common]") {
	SourceAnimatorState candidate;
	candidate.Detached = {Animator(GroupSubtypeAnimator::Animated)};
	candidate.DetachedValues = {Payload()};
	candidate.DetachedValues.front().Fixed = true;
	const auto initial = detail::SourceAnimatorStateBytes(candidate, false);
	REQUIRE(initial);
	detail::EvaluationBudget budget(Limits::MaximumEvaluationBytes);
	auto charge = budget.Reserve(*initial);
	REQUIRE(charge);
	SourceCommonAnimatorResetReceipt receipt;
	Diagnostic diagnostic;
	REQUIRE(
		detail::ResetSourceCommonAnimatorBudgeted(
			candidate.Detached.front(),
			candidate.DetachedValues.front(),
			{{7}},
			receipt,
			diagnostic,
			budget,
			*charge
		) == Status::Ok
	);
	const auto final = detail::SourceAnimatorStateBytes(candidate, false);
	REQUIRE(final);
	CHECK(charge->Bytes() == *final);
	CHECK(budget.Used() == *final);
}
