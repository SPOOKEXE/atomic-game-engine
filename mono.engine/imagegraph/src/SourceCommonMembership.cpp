#include "SourceCommonMembership.hpp"

#include "SourceAnimatorPersistence.hpp"
#include "Utf8TextOps.hpp"

#include <engine/core/Profiling.hpp>

#include <algorithm>
#include <cmath>
#include <memory>
#include <new>

namespace engine::imagegraph::detail {
	namespace {
		constexpr uint64_t MAXIMUM_COMPARISON_WORK = 16'000'000;
		struct Selection {
			const SourceCommonOwnerRecord *Owner = nullptr;
			const SourceCommonSocketState *Socket = nullptr;
			const SourceCommonInputMap *Inputs = nullptr;
			const DetachedSourceAnimator *Animator = nullptr;
			const GroupSubtypeOverlay *Payload = nullptr;
			const SourceCommonWriterState *Effects = nullptr;
			bool WriterUnique = false;
		};
		Status Fail(Diagnostic &diagnostic, Status code, const char *message) {
			diagnostic = {code, {}, "pxcx.update_in_trigger", message};
			return code;
		}
		bool Text(uint64_t &bytes, const std::string &text, bool clone) {
			size_t count = 0;
			return text.find('\0') == std::string::npos && CountText(text, count) == TextOpStatus::Ok &&
				   SourceAnimatorText(bytes, text, clone) && SourceAnimatorAdd(bytes, 1);
		}
		bool SnapshotBytes(uint64_t &bytes, const SourceCommonOwnerSnapshot &row, bool clone) {
			return !row.SourceOwnerId.empty() && !row.SourceType.empty() && !row.NativeOwnerId.empty() &&
				   (row.NativeOwnerKind == SourceCommonNativeOwnerKind::Node ||
					row.NativeOwnerKind == SourceCommonNativeOwnerKind::Group) &&
				   Text(bytes, row.SourceOwnerId, clone) && Text(bytes, row.SourceType, clone) &&
				   Text(bytes, row.NativeOwnerId, clone);
		}
		bool SocketBytes(uint64_t &bytes, const SourceCommonSocketState &row, bool clone) {
			return Text(bytes, row.OwnerId, clone) && Text(bytes, row.OwnerType, clone) &&
				   Text(bytes, row.Name, clone) && std::isfinite(row.Position.X) &&
				   std::isfinite(row.Position.Y);
		}
		bool EffectBytes(uint64_t &bytes, const SourceCommonWriterState &row, bool clone) {
			return Text(bytes, row.OwnerId, clone) && Text(bytes, row.Port, clone);
		}
		bool InputMapBytes(uint64_t &bytes, const SourceCommonInputMap &row, bool clone) {
			if (row.OwnerId.empty() || row.OwnerType.empty() || !Text(bytes, row.OwnerId, clone) ||
				!Text(bytes, row.OwnerType, clone))
				return false;
			if (!row.Inputs) return true;
			// Read owned payload directly. Wrapping this in Value would copy before admission.
			if (!ValidStructPayload(*row.Inputs)) return false;
			return SourceAnimatorAdd(bytes, RetainedPayloadBytes(*row.Inputs));
		}
		bool AnimatorBytes(uint64_t &bytes, const DetachedSourceAnimator &row, bool clone) {
			if (row.OriginalPort != "pxcx.update_in_trigger" || row.Type != ValueType::Boolean ||
				!SourceAnimatorEnum(row.Writer) || !Text(bytes, row.Id, clone) ||
				!Text(bytes, row.OwnerId, clone) || !Text(bytes, row.OriginalPort, clone))
				return false;
			return !row.Track || (Text(bytes, row.Track->NodeId, clone) &&
								  Text(bytes, row.Track->Port, clone) && Text(bytes, row.Track->End, clone));
		}
		bool PayloadBytes(uint64_t &bytes, const GroupSubtypeOverlay &row, bool clone, size_t &keys) {
			if (row.SeparatedVec2 || (row.Fixed && !row.Keys.empty()) || !Text(bytes, row.NodeId, clone) ||
				!Text(bytes, row.Port, clone))
				return false;
			if (row.Fixed && !std::holds_alternative<bool>(*row.Fixed)) return false;
			const size_t slots = clone ? row.Keys.size() : row.Keys.capacity();
			if (slots > Limits::MaximumKeyframes || !SourceAnimatorAdd(bytes, slots * sizeof(Keyframe)))
				return false;
			for (const auto &key : row.Keys)
				if (key.NodeId != row.NodeId || key.Port != row.Port ||
					!std::holds_alternative<bool>(key.Data) || !SourceAnimatorKey(bytes, key, clone, keys))
					return false;
			return true;
		}
		bool SameBinding(const SourceCommonOwnerRecord &owner, const SourceCommonOwnerSnapshot &snapshot) {
			return owner.SourceOwnerId == snapshot.SourceOwnerId && owner.SourceType == snapshot.SourceType &&
				   owner.NativeOwnerKind == snapshot.NativeOwnerKind &&
				   owner.NativeOwnerId == snapshot.NativeOwnerId;
		}
		template <class Rows, class Predicate>
		const typename Rows::value_type *Find(const Rows &rows, Predicate matches) {
			const auto found = std::find_if(rows.begin(), rows.end(), matches);
			return found == rows.end() ? nullptr : &*found;
		}
		bool Identity(const DetachedSourceAnimator &row, std::string_view owner, std::string_view port) {
			return row.OwnerId == owner && row.Id == port;
		}
		bool Identity(const GroupSubtypeOverlay &row, std::string_view owner, std::string_view port) {
			return row.NodeId == owner && row.Port == port;
		}
		bool Identity(const SourceCommonWriterState &row, std::string_view owner, std::string_view port) {
			return row.OwnerId == owner && row.Port == port;
		}
		Status Select(
			const Document &document,
			const GroupRenderSession &candidate,
			Selection *selections,
			std::span<const SourceCommonWriterIdentity> resetWriters,
			Diagnostic &diagnostic
		) {
			for (size_t index = 0; index < document.SourceCommonOwners.size(); ++index) {
				const auto &owner = document.SourceCommonOwners[index];
				auto &selected = selections[index];
				selected.Owner = &owner;
				const auto *snapshot = Find(candidate.SourceCommonBindings, [&](const auto &row) {
					return row.SourceOwnerId == owner.SourceOwnerId;
				});
				const bool survived = snapshot && SameBinding(owner, *snapshot);
				if (survived)
					selected.Socket = Find(candidate.Common.Owners, [&](const auto &row) {
						return row.OwnerId == owner.SourceOwnerId && row.OwnerType == owner.SourceType;
					});
				if (survived && !selected.Socket)
					return Fail(
						diagnostic, Status::InvalidValue, "common retained constructor history is incomplete"
					);
				if (survived)
					selected.Inputs = Find(candidate.SourceCommonInputs, [&](const auto &row) {
						return row.OwnerId == owner.SourceOwnerId && row.OwnerType == owner.SourceType;
					});
				if (survived && !selected.Inputs)
					return Fail(
						diagnostic, Status::InvalidValue, "common retained input history is incomplete"
					);
				if (owner.UpdateAnimatorOwnerId.empty() || owner.UpdateAnimatorPort.empty())
					return Fail(diagnostic, Status::InvalidValue, "common local writer is absent");
				if (!document.SourceAnimators)
					return Fail(diagnostic, Status::InvalidValue, "common authored animators are absent");
				const auto &authored = *document.SourceAnimators;
				const auto *animator = Find(authored.Detached, [&](const auto &row) {
					return Identity(row, owner.UpdateAnimatorOwnerId, owner.UpdateAnimatorPort);
				});
				const auto *payload = Find(authored.DetachedValues, [&](const auto &row) {
					return Identity(row, owner.UpdateAnimatorOwnerId, owner.UpdateAnimatorPort);
				});
				if (!animator || !payload)
					return Fail(
						diagnostic, Status::InvalidValue, "common authored local writer payload is absent"
					);
				selected.Animator = animator;
				selected.Payload = payload;
				bool reset = !survived;
				for (const auto &identity : resetWriters)
					if (identity.OwnerId == owner.UpdateAnimatorOwnerId &&
						identity.Port == owner.UpdateAnimatorPort)
						reset = true;
				if (!reset) {
					const auto *retainedAnimator =
						Find(candidate.CommonAnimators.Detached, [&](const auto &row) {
							return Identity(row, owner.UpdateAnimatorOwnerId, owner.UpdateAnimatorPort);
						});
					const auto *retainedPayload =
						Find(candidate.CommonAnimators.DetachedValues, [&](const auto &row) {
							return Identity(row, owner.UpdateAnimatorOwnerId, owner.UpdateAnimatorPort);
						});
					if (retainedAnimator && retainedPayload) {
						// Mode/track metadata is authored; only the physical setter payload is process
						// history.
						selected.Payload = retainedPayload;
						selected.Effects = Find(candidate.SourceCommonWrites, [&](const auto &row) {
							return Identity(row, owner.UpdateAnimatorOwnerId, owner.UpdateAnimatorPort);
						});
					} else if (retainedAnimator || retainedPayload)
						return Fail(
							diagnostic, Status::InvalidValue, "common retained writer history is incomplete"
						);
				}
				selected.WriterUnique = true;
				for (size_t previous = 0; previous < index; ++previous)
					if (Identity(*selections[previous].Animator, animator->OwnerId, animator->Id)) {
						// A single physical writer cannot simultaneously survive and reconstruct.
						if (selections[previous].Payload != selected.Payload)
							return Fail(
								diagnostic,
								Status::InvalidValue,
								"shared common writer has conflicting lifecycle events"
							);
						selected.WriterUnique = false;
						break;
					}
			}
			for (size_t index = 0; index < resetWriters.size(); ++index) {
				const auto &identity = resetWriters[index];
				bool found = false;
				for (size_t owner = 0; owner < document.SourceCommonOwners.size(); ++owner)
					found |= Identity(*selections[owner].Animator, identity.OwnerId, identity.Port);
				if (!found)
					return Fail(
						diagnostic,
						Status::UnknownPort,
						"explicit common writer edit names no registered writer"
					);
				for (size_t previous = 0; previous < index; ++previous)
					if (identity.OwnerId == resetWriters[previous].OwnerId &&
						identity.Port == resetWriters[previous].Port)
						return Fail(
							diagnostic, Status::DuplicateId, "explicit common writer edit is duplicated"
						);
			}
			return Status::Ok;
		}
		bool ReplacementBytes(uint64_t &bytes, std::span<const Selection> selections) {
			bytes = sizeof(SourceCommonSocketSession) + sizeof(SourceAnimatorState) +
					sizeof(std::vector<SourceCommonOwnerSnapshot>) +
					sizeof(std::vector<SourceCommonWriterState>) + sizeof(std::vector<SourceCommonInputMap>);
			if (!SourceAnimatorAdd(
					bytes,
					selections.size() * (sizeof(SourceCommonSocketState) + sizeof(SourceCommonOwnerSnapshot) +
										 sizeof(SourceCommonInputMap))
				))
				return false;
			size_t aggregateKeys = 0;
			for (const auto &selected : selections) {
				const auto &owner = *selected.Owner;
				if (!Text(bytes, owner.SourceOwnerId, true) || !Text(bytes, owner.SourceType, true) ||
					!Text(bytes, owner.NativeOwnerId, true))
					return false;
				if (selected.Socket) {
					if (!SocketBytes(bytes, *selected.Socket, true)) return false;
				} else if (!Text(bytes, owner.SourceOwnerId, true) || !Text(bytes, owner.SourceType, true) ||
						   !SourceAnimatorAdd(bytes, std::string{}.capacity() + 1))
					return false;
				if (selected.Inputs) {
					if (!InputMapBytes(bytes, *selected.Inputs, true)) return false;
				} else if (!Text(bytes, owner.SourceOwnerId, true) || !Text(bytes, owner.SourceType, true))
					return false;
				if (!selected.WriterUnique) continue;
				if (!SourceAnimatorAdd(bytes, sizeof(DetachedSourceAnimator) + sizeof(GroupSubtypeOverlay)) ||
					!AnimatorBytes(bytes, *selected.Animator, true) ||
					!PayloadBytes(bytes, *selected.Payload, true, aggregateKeys))
					return false;
				if (selected.Effects && (!SourceAnimatorAdd(bytes, sizeof(SourceCommonWriterState)) ||
										 !EffectBytes(bytes, *selected.Effects, true)))
					return false;
			}
			return true;
		}
	}
	uint64_t RetainedSourceCommonMembershipBytes(const GroupRenderSession &session) {
		const auto animatorBytes = SourceAnimatorStateBytes(session.CommonAnimators, false);
		if (!animatorBytes || session.SourceCommonBindings.capacity() > Limits::MaximumSourceCommonOwners ||
			session.SourceCommonInputs.capacity() > Limits::MaximumSourceCommonOwners ||
			session.SourceCommonWrites.capacity() > Limits::MaximumArrayElements)
			return UINT64_MAX;
		uint64_t validationBytes = 0;
		size_t keys = 0;
		for (const auto &socket : session.Common.Owners)
			if (socket.OwnerId.empty() || socket.OwnerType.empty() ||
				!SocketBytes(validationBytes, socket, false))
				return UINT64_MAX;
		for (const auto &animator : session.CommonAnimators.Detached)
			if (!AnimatorBytes(validationBytes, animator, false)) return UINT64_MAX;
		for (const auto &payload : session.CommonAnimators.DetachedValues)
			if (!PayloadBytes(validationBytes, payload, false, keys)) return UINT64_MAX;
		uint64_t bytes = RetainedSourceCommonSocketBytes(session.Common);
		if (!SourceAnimatorAdd(bytes, *animatorBytes) ||
			!SourceAnimatorAdd(
				bytes,
				sizeof(session.SourceCommonBindings) +
					session.SourceCommonBindings.capacity() * sizeof(SourceCommonOwnerSnapshot)
			) ||
			!SourceAnimatorAdd(
				bytes,
				sizeof(session.SourceCommonWrites) +
					session.SourceCommonWrites.capacity() * sizeof(SourceCommonWriterState)
			) ||
			!SourceAnimatorAdd(
				bytes,
				sizeof(session.SourceCommonInputs) +
					session.SourceCommonInputs.capacity() * sizeof(SourceCommonInputMap)
			))
			return UINT64_MAX;
		for (const auto &row : session.SourceCommonBindings)
			if (!SnapshotBytes(bytes, row, false)) return UINT64_MAX;
		for (const auto &row : session.SourceCommonWrites)
			if (!EffectBytes(bytes, row, false)) return UINT64_MAX;
		for (const auto &row : session.SourceCommonInputs)
			if (!InputMapBytes(bytes, row, false)) return UINT64_MAX;
		return bytes;
	}
	Status ReconcileSourceCommonMembership(
		const Document &document,
		SourceNodeInitialState newOwnerState,
		GroupRenderSession &candidate,
		EvaluationBudget &budget,
		AllocationReservation &candidateCharge,
		Diagnostic &diagnostic,
		std::span<const SourceCommonWriterIdentity> resetWriters
	) try {
		ENGINE_PROFILE("imagegraph.common_membership.reconcile");
		if (newOwnerState != SourceNodeInitialState::Loaded &&
			newOwnerState != SourceNodeInitialState::Constructed)
			return Fail(diagnostic, Status::InvalidValue, "common owner constructor mode is invalid");
		const size_t count = document.SourceCommonOwners.size();
		if (count > Limits::MaximumSourceCommonOwners || resetWriters.size() > Limits::MaximumArrayElements ||
			candidate.SourceCommonBindings.capacity() > Limits::MaximumSourceCommonOwners ||
			candidate.Common.Owners.capacity() > Limits::MaximumSourceCommonOwners ||
			candidate.SourceCommonInputs.capacity() > Limits::MaximumSourceCommonOwners ||
			candidate.CommonAnimators.Detached.capacity() > Limits::MaximumArrayElements ||
			candidate.CommonAnimators.DetachedValues.capacity() > Limits::MaximumArrayElements ||
			candidate.SourceCommonWrites.capacity() > Limits::MaximumArrayElements ||
			candidate.SourceCommonBindings.size() != candidate.Common.Owners.size() ||
			candidate.SourceCommonInputs.size() != candidate.Common.Owners.size() ||
			!candidate.CommonAnimators.Bindings.empty())
			return Fail(diagnostic, Status::LimitExceeded, "common membership count exceeds bounds");
		uint64_t identityBytes = count + candidate.SourceCommonBindings.size() + resetWriters.size();
		for (const auto &owner : document.SourceCommonOwners)
			if (!SourceAnimatorAdd(
					identityBytes,
					owner.SourceOwnerId.size() + owner.SourceType.size() + owner.NativeOwnerId.size() +
						owner.UpdateAnimatorOwnerId.size() + owner.UpdateAnimatorPort.size()
				))
				return Fail(diagnostic, Status::LimitExceeded, "common membership identity bytes overflow");
		for (const auto &snapshot : candidate.SourceCommonBindings)
			if (!SourceAnimatorAdd(
					identityBytes,
					snapshot.SourceOwnerId.size() + snapshot.SourceType.size() + snapshot.NativeOwnerId.size()
				))
				return Fail(diagnostic, Status::LimitExceeded, "common retained identity bytes overflow");
		for (const auto &identity : resetWriters)
			if (!SourceAnimatorAdd(identityBytes, identity.OwnerId.size() + identity.Port.size()))
				return Fail(diagnostic, Status::LimitExceeded, "common writer edit identity bytes overflow");
		for (const auto &animator : candidate.CommonAnimators.Detached)
			if (!SourceAnimatorAdd(identityBytes, 1 + animator.OwnerId.size() + animator.Id.size()))
				return Fail(
					diagnostic, Status::LimitExceeded, "common retained writer identity bytes overflow"
				);
		for (const auto &effects : candidate.SourceCommonWrites)
			if (!SourceAnimatorAdd(identityBytes, 1 + effects.OwnerId.size() + effects.Port.size()))
				return Fail(
					diagnostic, Status::LimitExceeded, "common retained effects identity bytes overflow"
				);
		const uint64_t comparisons =
			count + candidate.SourceCommonBindings.size() + candidate.SourceCommonInputs.size() +
			candidate.CommonAnimators.Detached.size() + candidate.CommonAnimators.DetachedValues.size() +
			candidate.SourceCommonWrites.size() + resetWriters.size() +
			(document.SourceAnimators
				 ? document.SourceAnimators->Detached.size() + document.SourceAnimators->DetachedValues.size()
				 : 0);
		if (comparisons > MAXIMUM_COMPARISON_WORK || identityBytes > MAXIMUM_COMPARISON_WORK ||
			(comparisons && identityBytes > MAXIMUM_COMPARISON_WORK / comparisons))
			return Fail(
				diagnostic, Status::LimitExceeded, "common membership comparison work exceeds bounds"
			);
		const auto oldBytes = RetainedSourceCommonMembershipBytes(candidate);
		if (oldBytes == UINT64_MAX || oldBytes > candidateCharge.Bytes())
			return Fail(
				diagnostic, Status::LimitExceeded, "common membership candidate charge is incomplete"
			);
		auto ledgerProbe = budget.Reserve(0);
		if (!ledgerProbe || !candidateCharge.Merge(std::move(*ledgerProbe)))
			return Fail(
				diagnostic, Status::InvalidValue, "common membership candidate uses a different byte ledger"
			);
		for (size_t index = 0; index < candidate.SourceCommonBindings.size(); ++index) {
			const auto &snapshot = candidate.SourceCommonBindings[index];
			const auto &socket = candidate.Common.Owners[index];
			const auto &inputs = candidate.SourceCommonInputs[index];
			if (snapshot.SourceOwnerId != socket.OwnerId || snapshot.SourceType != socket.OwnerType ||
				snapshot.SourceOwnerId != inputs.OwnerId || snapshot.SourceType != inputs.OwnerType)
				return Fail(diagnostic, Status::InvalidValue, "common retained owner binding order is stale");
			for (size_t previous = 0; previous < index; ++previous)
				if (candidate.SourceCommonBindings[previous].SourceOwnerId == snapshot.SourceOwnerId ||
					(candidate.SourceCommonBindings[previous].NativeOwnerKind == snapshot.NativeOwnerKind &&
					 candidate.SourceCommonBindings[previous].NativeOwnerId == snapshot.NativeOwnerId))
					return Fail(
						diagnostic, Status::DuplicateId, "common retained owner binding is duplicated"
					);
		}
		if (candidate.CommonAnimators.Detached.size() != candidate.CommonAnimators.DetachedValues.size())
			return Fail(diagnostic, Status::InvalidValue, "common retained writer count is inconsistent");
		for (size_t index = 0; index < candidate.CommonAnimators.Detached.size(); ++index) {
			const auto &animator = candidate.CommonAnimators.Detached[index];
			const auto &payload = candidate.CommonAnimators.DetachedValues[index];
			if (!Identity(payload, animator.OwnerId, animator.Id))
				return Fail(
					diagnostic, Status::InvalidValue, "common retained writer payload order is inconsistent"
				);
			for (size_t previous = 0; previous < index; ++previous)
				if (Identity(candidate.CommonAnimators.Detached[previous], animator.OwnerId, animator.Id))
					return Fail(
						diagnostic, Status::DuplicateId, "common retained physical writer is duplicated"
					);
		}
		for (size_t index = 0; index < candidate.SourceCommonWrites.size(); ++index) {
			const auto &effects = candidate.SourceCommonWrites[index];
			if (!Find(candidate.CommonAnimators.Detached, [&](const auto &row) {
					return Identity(row, effects.OwnerId, effects.Port);
				}))
				return Fail(
					diagnostic, Status::InvalidValue, "common retained setter effects have no writer"
				);
			for (size_t previous = 0; previous < index; ++previous)
				if (Identity(candidate.SourceCommonWrites[previous], effects.OwnerId, effects.Port))
					return Fail(
						diagnostic, Status::DuplicateId, "common retained setter effects are duplicated"
					);
		}
		uint64_t scratchBytes = count * sizeof(Selection) + resetWriters.size_bytes();
		for (const auto &identity : resetWriters)
			if (identity.OwnerId.size() > Limits::MaximumTextBytes ||
				identity.Port.size() > Limits::MaximumTextBytes ||
				!SourceAnimatorAdd(scratchBytes, identity.OwnerId.size() + identity.Port.size()))
				return Fail(diagnostic, Status::LimitExceeded, "common writer edit identity exceeds bounds");
		auto scratchCharge = budget.Reserve(scratchBytes);
		if (!scratchCharge)
			return Fail(diagnostic, Status::LimitExceeded, "common membership scratch exceeds cap");
		auto selections = std::make_unique<Selection[]>(count);
		if (Select(document, candidate, selections.get(), resetWriters, diagnostic) != Status::Ok)
			return diagnostic.Code;
		uint64_t replacementBytes = 0;
		if (!ReplacementBytes(replacementBytes, {selections.get(), count}))
			return Fail(diagnostic, Status::InvalidValue, "common replacement writer payload is invalid");
		auto replacementCharge = budget.Reserve(replacementBytes);
		if (!replacementCharge)
			return Fail(diagnostic, Status::LimitExceeded, "common membership replacement exceeds cap");
		GroupRenderSession replacement;
		replacement.Common.Owners.reserve(count);
		replacement.SourceCommonInputs.reserve(count);
		replacement.SourceCommonBindings.reserve(count);
		size_t writers = 0, effects = 0;
		for (size_t index = 0; index < count; ++index) {
			writers += selections[index].WriterUnique;
			effects += selections[index].WriterUnique && selections[index].Effects;
		}
		replacement.CommonAnimators.Detached.reserve(writers);
		replacement.CommonAnimators.DetachedValues.reserve(writers);
		replacement.SourceCommonWrites.reserve(effects);
		for (size_t index = 0; index < count; ++index) {
			const auto &selected = selections[index];
			const auto &owner = *selected.Owner;
			if (selected.Socket)
				replacement.Common.Owners.push_back(*selected.Socket);
			else
				replacement.Common.Owners.push_back({owner.SourceOwnerId, owner.SourceType, false, {}, {}});
			if (selected.Inputs)
				replacement.SourceCommonInputs.push_back(*selected.Inputs);
			else
				replacement.SourceCommonInputs.push_back(
					{owner.SourceOwnerId, owner.SourceType, std::nullopt}
				);
			replacement.SourceCommonBindings.push_back(
				{owner.SourceOwnerId, owner.SourceType, owner.NativeOwnerKind, owner.NativeOwnerId}
			);
			if (!selected.WriterUnique) continue;
			replacement.CommonAnimators.Detached.push_back(*selected.Animator);
			replacement.CommonAnimators.DetachedValues.push_back(*selected.Payload);
			if (selected.Effects) replacement.SourceCommonWrites.push_back(*selected.Effects);
		}
		const uint64_t actualBytes = RetainedSourceCommonMembershipBytes(replacement);
		if (actualBytes == UINT64_MAX || !replacementCharge->Resize(actualBytes))
			return Fail(
				diagnostic, Status::LimitExceeded, "common membership allocated capacities exceed cap"
			);
		// Swap leaves old selected storage owned by replacement until it is actually freed.
		std::swap(candidate.Common, replacement.Common);
		std::swap(candidate.CommonAnimators, replacement.CommonAnimators);
		candidate.SourceCommonBindings.swap(replacement.SourceCommonBindings);
		candidate.SourceCommonWrites.swap(replacement.SourceCommonWrites);
		candidate.SourceCommonInputs.swap(replacement.SourceCommonInputs);
		replacement = {};
		const uint64_t retainedOtherBytes = candidateCharge.Bytes() - oldBytes;
		const bool released = candidateCharge.Resize(retainedOtherBytes);
		const bool transferred = candidateCharge.Merge(std::move(*replacementCharge));
		// Both are shrinking or same-ledger transfers, which cannot refuse.
		assert(released && transferred);
		(void)released;
		(void)transferred;
		diagnostic = {};
		return Status::Ok;
	} catch (const std::bad_alloc &) {
		return Fail(diagnostic, Status::LimitExceeded, "common membership allocation failed");
	}
}
