#include "SourceAnimatorPersistence.hpp"
#include "SourceCommonAnimatorResetInternal.hpp"

#include <engine/core/Profiling.hpp>

#include <algorithm>
#include <new>

namespace engine::imagegraph {
	namespace {
		Status Fail(Diagnostic &diagnostic, Status status, const char *message) {
			diagnostic = {status, {}, {}, message};
			return status;
		}
		std::optional<uint64_t> Bytes(const GroupSubtypeOverlay &payload, bool clone) {
			if (payload.SeparatedVec2 || (payload.Fixed && !payload.Keys.empty()) ||
				payload.Keys.size() > Limits::MaximumKeyframes ||
				payload.Keys.capacity() > Limits::MaximumKeyframes)
				return std::nullopt;
			uint64_t bytes =
				sizeof(payload) + (clone ? payload.Keys.size() : payload.Keys.capacity()) * sizeof(Keyframe);
			// Retained string capacities conservatively admit their copies, including short-string storage.
			if (!detail::SourceAnimatorText(bytes, payload.NodeId, false) ||
				!detail::SourceAnimatorText(bytes, payload.Port, false))
				return std::nullopt;
			if (payload.Fixed &&
				(!std::holds_alternative<bool>(*payload.Fixed) ||
				 !detail::SourceAnimatorAdd(bytes, detail::RetainedPayloadBytes(*payload.Fixed))))
				return std::nullopt;
			size_t count = 0;
			for (const auto &key : payload.Keys)
				if (key.NodeId != payload.NodeId || key.Port != payload.Port ||
					!std::holds_alternative<bool>(key.Data) ||
					!detail::SourceAnimatorKey(bytes, key, false, count) || GetFrameTime(key).NegativeFrame ||
					GetFrameTime(key).Subframe != 0)
					return std::nullopt;
			return bytes;
		}
		Keyframe Fresh(const GroupSubtypeOverlay &payload, FrameTime time) {
			Keyframe key;
			key.NodeId = std::string(payload.NodeId);
			key.Port = std::string(payload.Port);
			key.Data = false;
			key.Interpolation = "source";
			key.Ease = KeyframeEase{};
			(void)SetFrameTime(key, time);
			return key;
		}
		std::optional<uint64_t> FreshOwnedBytes(const GroupSubtypeOverlay &payload) {
			// All prototype strings stay in short-string storage; borrowed identities are charged before
			// copy.
			Keyframe prototype;
			prototype.Data = false;
			prototype.Interpolation = "source";
			prototype.Ease = KeyframeEase{};
			uint64_t bytes = 0;
			size_t count = 0;
			if (!detail::SourceAnimatorKey(bytes, prototype, false, count) ||
				!detail::SourceAnimatorAdd(
					bytes,
					std::max(payload.NodeId.size(), prototype.NodeId.capacity()) - prototype.NodeId.capacity()
				) ||
				!detail::SourceAnimatorAdd(
					bytes,
					std::max(payload.Port.size(), prototype.Port.capacity()) - prototype.Port.capacity()
				))
				return std::nullopt;
			return bytes;
		}
		Status Validate(
			const DetachedSourceAnimator &animator,
			const GroupSubtypeOverlay &payload,
			const SourceCommonAnimatorResetOptions &options,
			Diagnostic &diagnostic
		) {
			if (animator.OriginalPort != "pxcx.update_in_trigger" || animator.Type != ValueType::Boolean ||
				animator.OwnerId.empty() || animator.Id.empty() || animator.OwnerId != payload.NodeId ||
				animator.Id != payload.Port || !detail::SourceAnimatorEnum(animator.Writer))
				return Fail(
					diagnostic, Status::InvalidValue, "common reset local animator identity is invalid"
				);
			if (!ValidFrameTime(options.Time) || (animator.Writer == GroupSubtypeAnimator::Animated &&
												  (options.Time.NegativeFrame || options.Time.Subframe != 0)))
				return Fail(
					diagnostic,
					Status::UnsupportedExecution,
					"common Trigger setter needs a nonnegative integer frame"
				);
			return Status::Ok;
		}
		Status Build(
			const DetachedSourceAnimator &animator,
			const GroupSubtypeOverlay &payload,
			const SourceCommonAnimatorResetOptions &options,
			GroupSubtypeOverlay &replacement,
			SourceCommonAnimatorResetReceipt &receipt,
			Diagnostic &diagnostic,
			detail::EvaluationBudget &budget,
			detail::AllocationReservation &replacementCharge
		) {
			if (Validate(animator, payload, options, diagnostic) != Status::Ok) return diagnostic.Code;
			const auto clone = Bytes(payload, true), fresh = FreshOwnedBytes(payload);
			if (!clone || !fresh)
				return Fail(diagnostic, Status::InvalidValue, "common animator payload is invalid");
			const bool animated = animator.Writer == GroupSubtypeAnimator::Animated;
			const auto position =
				std::find_if(payload.Keys.begin(), payload.Keys.end(), [&](const auto &key) {
					return key.Tick >= options.Time.Tick;
				});
			const bool exact = payload.Fixed
								   ? options.Time.Tick == 0
								   : (position != payload.Keys.end() && position->Tick == options.Time.Tick);
			const size_t seeds = payload.Fixed ? 1 : 0;
			const size_t inserts = animated ? !exact : payload.Keys.empty();
			const size_t freshCount = animated ? seeds + inserts : 1;
			const size_t slots =
				animated ? payload.Keys.size() + seeds + inserts : std::max(size_t{1}, payload.Keys.size());
			if (slots > Limits::MaximumKeyframes)
				return Fail(diagnostic, Status::LimitExceeded, "common Trigger reset exceeds key count cap");
			uint64_t charge = *clone;
			if (!detail::SourceAnimatorAdd(charge, (slots - payload.Keys.size()) * sizeof(Keyframe)) ||
				!detail::SourceAnimatorAdd(charge, freshCount * *fresh))
				return Fail(diagnostic, Status::LimitExceeded, "common animator replacement bytes overflow");
			auto reserved = budget.Reserve(charge);
			if (!reserved)
				return Fail(diagnostic, Status::LimitExceeded, "common animator reset overlap exceeds cap");
			GroupSubtypeOverlay candidate{payload.NodeId, std::nullopt, {}, payload.Port, {}};
			candidate.Keys.reserve(slots);
			candidate.Keys.assign(payload.Keys.begin(), payload.Keys.end());
			if (payload.Fixed && animated) {
				auto seed = Fresh(payload, {});
				seed.Data = *payload.Fixed;
				candidate.Keys.push_back(std::move(seed));
			}
			SourceCommonAnimatorResetReceipt next;
			if (!animated) {
				if (candidate.Keys.empty())
					candidate.Keys.push_back(Fresh(payload, {}));
				else
					candidate.Keys.front() = Fresh(payload, {});
				next.AnimatorReturnedTrue = true;
			} else {
				const auto at =
					std::find_if(candidate.Keys.begin(), candidate.Keys.end(), [&](const auto &key) {
						return key.Tick >= options.Time.Tick;
					});
				if (exact) {
					if (options.ReplaceExistingKey) at->Data = false;
					// Source returns false even when an existing Trigger key changed its stored value.
				} else {
					candidate.Keys.insert(at, Fresh(payload, options.Time));
					next.AnimatorReturnedTrue = true;
				}
			}
			next.Edited = next.AnimatorReturnedTrue || options.UpdateOnSet;
			const auto after = Bytes(candidate, false);
			if (!after || *after > reserved->Bytes())
				return Fail(
					diagnostic, Status::LimitExceeded, "common replacement capacities exceed admission"
				);
			if (!reserved->Resize(*after)) std::terminate();
			replacement = std::move(candidate);
			replacementCharge = std::move(*reserved);
			receipt = next;
			return Status::Ok;
		}
	}
	Status ResetSourceCommonAnimator(
		const DetachedSourceAnimator &animator,
		const GroupSubtypeOverlay &payload,
		const SourceCommonAnimatorResetOptions &options,
		GroupSubtypeOverlay &result,
		SourceCommonAnimatorResetReceipt &receipt,
		Diagnostic &diagnostic,
		uint64_t maximumBytes
	) try {
		ENGINE_PROFILE("imagegraph.common_animator_reset");
		if (!maximumBytes || maximumBytes > Limits::MaximumEvaluationBytes)
			return Fail(diagnostic, Status::LimitExceeded, "common animator reset cap is invalid");
		const auto old = Bytes(payload, false), prior = Bytes(result, false);
		if (!old || !prior)
			return Fail(diagnostic, Status::InvalidValue, "common animator payload is invalid");
		uint64_t charge = sizeof(animator) + *old;
		if (!detail::SourceAnimatorText(charge, animator.OwnerId, false) ||
			!detail::SourceAnimatorText(charge, animator.Id, false) ||
			!detail::SourceAnimatorText(charge, animator.OriginalPort, false) ||
			(animator.Track && (!detail::SourceAnimatorText(charge, animator.Track->NodeId, false) ||
								!detail::SourceAnimatorText(charge, animator.Track->Port, false) ||
								!detail::SourceAnimatorText(charge, animator.Track->End, false))) ||
			!detail::SourceAnimatorAdd(charge, &payload == &result ? 0 : *prior))
			return Fail(diagnostic, Status::LimitExceeded, "common animator reset borrowed bytes overflow");
		detail::EvaluationBudget budget(maximumBytes);
		auto borrowed = budget.Reserve(charge);
		if (!borrowed)
			return Fail(
				diagnostic, Status::LimitExceeded, "common animator reset borrowed payload exceeds cap"
			);
		detail::AllocationReservation replacementCharge;
		GroupSubtypeOverlay replacement;
		SourceCommonAnimatorResetReceipt next;
		if (Build(animator, payload, options, replacement, next, diagnostic, budget, replacementCharge) !=
			Status::Ok)
			return diagnostic.Code;
		result = std::move(replacement);
		receipt = next;
		diagnostic = {};
		return Status::Ok;
	} catch (const std::bad_alloc &) {
		return Fail(diagnostic, Status::LimitExceeded, "common animator reset allocation failed");
	}
	namespace detail {
		Status ResetSourceCommonAnimatorBudgeted(
			const DetachedSourceAnimator &animator,
			GroupSubtypeOverlay &candidatePayload,
			const SourceCommonAnimatorResetOptions &options,
			SourceCommonAnimatorResetReceipt &receipt,
			Diagnostic &diagnostic,
			EvaluationBudget &budget,
			AllocationReservation &candidateCharge
		) try {
			ENGINE_PROFILE("imagegraph.common_animator_reset");
			const auto old = Bytes(candidatePayload, false);
			if (!old || candidateCharge.Bytes() < *old)
				return Fail(diagnostic, Status::InvalidValue, "common reset candidate charge is incomplete");
			AllocationReservation replacementCharge;
			GroupSubtypeOverlay replacement;
			SourceCommonAnimatorResetReceipt next;
			if (Build(
					animator,
					candidatePayload,
					options,
					replacement,
					next,
					diagnostic,
					budget,
					replacementCharge
				) != Status::Ok)
				return diagnostic.Code;
			candidatePayload = std::move(replacement);
			if (!candidateCharge.Resize(candidateCharge.Bytes() - *old) ||
				!candidateCharge.Merge(std::move(replacementCharge)))
				std::terminate();
			receipt = next;
			diagnostic = {};
			return Status::Ok;
		} catch (const std::bad_alloc &) {
			return Fail(diagnostic, Status::LimitExceeded, "common animator reset allocation failed");
		}
	}
}
