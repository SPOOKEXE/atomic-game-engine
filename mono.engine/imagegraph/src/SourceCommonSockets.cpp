#include "Utf8TextOps.hpp"

#include <engine/core/Profiling.hpp>
#include <engine/imagegraph/SourceCommonSockets.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>

namespace engine::imagegraph {
	namespace {
		constexpr uint64_t WorkLimit = 16'000'000;
		bool SourceCommonSocketsAdd(uint64_t &total, uint64_t bytes) {
			if (bytes > UINT64_MAX - total) return false;
			total += bytes;
			return true;
		}
		uint64_t StringBytes(const std::string &text) {
			return text.capacity() + 1;
		}
		uint64_t CopyTextBytes(std::string_view text) {
			return std::max(text.size(), std::string{}.capacity()) + 1;
		}
		bool Text(std::string_view text, bool identity) {
			if (text.size() > Limits::MaximumTextBytes ||
				(identity && (text.empty() || text.find('\0') != std::string_view::npos)))
				return false;
			size_t count = 0;
			return detail::CountText(text, count) == detail::TextOpStatus::Ok;
		}
		Status SourceCommonSocketsFail(
			Diagnostic &diagnostic, Status status, const char *message, std::string_view owner = {}
		) {
			diagnostic = {status, std::string(owner), {}, message};
			return status;
		}
		bool Cap(uint64_t maximumBytes) {
			return maximumBytes && maximumBytes <= Limits::MaximumEvaluationBytes;
		}
		Status Owners(std::span<const SourceCommonOwner> owners, Diagnostic &diagnostic, uint64_t &inputs) {
			if (owners.size() > Limits::MaximumSourceCommonOwners)
				return SourceCommonSocketsFail(
					diagnostic, Status::LimitExceeded, "common socket owner count exceeds bounds"
				);
			inputs = owners.size_bytes();
			uint64_t textWork = 0, identityBytes = 0;
			for (const auto &owner : owners) {
				if (owner.OwnerId.size() > Limits::MaximumTextBytes ||
					owner.OwnerType.size() > Limits::MaximumTextBytes)
					return SourceCommonSocketsFail(
						diagnostic, Status::LimitExceeded, "common socket owner text exceeds bounds"
					);
				if (!SourceCommonSocketsAdd(inputs, owner.OwnerId.size() + owner.OwnerType.size()) ||
					!SourceCommonSocketsAdd(textWork, owner.OwnerId.size() + owner.OwnerType.size()) ||
					!SourceCommonSocketsAdd(identityBytes, owner.OwnerId.size()))
					return SourceCommonSocketsFail(
						diagnostic, Status::LimitExceeded, "common socket identity bytes overflow"
					);
			}
			if (textWork > WorkLimit ||
				textWork + owners.size() * (identityBytes + owners.size()) > WorkLimit)
				return SourceCommonSocketsFail(
					diagnostic, Status::LimitExceeded, "common socket owner comparison exceeds bounds"
				);
			for (const auto &owner : owners)
				if (!Text(owner.OwnerId, true) || !Text(owner.OwnerType, true))
					return SourceCommonSocketsFail(
						diagnostic, Status::InvalidValue, "common socket owner identity is invalid"
					);
			for (size_t index = 0; index < owners.size(); ++index)
				for (size_t previous = 0; previous < index; ++previous)
					if (owners[index].OwnerId == owners[previous].OwnerId)
						return SourceCommonSocketsFail(
							diagnostic,
							Status::DuplicateId,
							"common socket owner is duplicated",
							owners[index].OwnerId
						);
			return Status::Ok;
		}
		Status Session(const SourceCommonSocketSession &session, Diagnostic &diagnostic) {
			if (session.Owners.size() > Limits::MaximumSourceCommonOwners ||
				session.Owners.capacity() > Limits::MaximumSourceCommonOwners)
				return SourceCommonSocketsFail(
					diagnostic, Status::LimitExceeded, "common socket state count exceeds bounds"
				);
			uint64_t textWork = 0, identityBytes = 0;
			for (const auto &state : session.Owners) {
				if (state.OwnerId.size() > Limits::MaximumTextBytes ||
					state.OwnerType.size() > Limits::MaximumTextBytes ||
					state.Name.size() > Limits::MaximumTextBytes)
					return SourceCommonSocketsFail(
						diagnostic, Status::LimitExceeded, "common socket retained text exceeds bounds"
					);
				if (!SourceCommonSocketsAdd(
						textWork, state.OwnerId.size() + state.OwnerType.size() + state.Name.size()
					) ||
					!SourceCommonSocketsAdd(identityBytes, state.OwnerId.size()))
					return SourceCommonSocketsFail(
						diagnostic, Status::LimitExceeded, "common socket retained text overflows"
					);
			}
			if (textWork > WorkLimit ||
				textWork + session.Owners.size() * (identityBytes + session.Owners.size()) > WorkLimit)
				return SourceCommonSocketsFail(
					diagnostic, Status::LimitExceeded, "common socket retained comparison exceeds bounds"
				);
			for (const auto &state : session.Owners)
				if (!Text(state.OwnerId, true) || !Text(state.OwnerType, true) || !Text(state.Name, false) ||
					!std::isfinite(state.Position.X) || !std::isfinite(state.Position.Y))
					return SourceCommonSocketsFail(
						diagnostic, Status::InvalidValue, "common socket retained state is invalid"
					);
			for (size_t index = 0; index < session.Owners.size(); ++index)
				for (size_t previous = 0; previous < index; ++previous)
					if (session.Owners[index].OwnerId == session.Owners[previous].OwnerId)
						return SourceCommonSocketsFail(
							diagnostic, Status::DuplicateId, "common socket retained owner is duplicated"
						);
			return Status::Ok;
		}
		uint64_t CloneBytes(const SourceCommonSocketSession &session) {
			uint64_t bytes = sizeof(session) + session.Owners.size() * sizeof(SourceCommonSocketState);
			for (const auto &state : session.Owners)
				if (!SourceCommonSocketsAdd(bytes, CopyTextBytes(state.OwnerId)) ||
					!SourceCommonSocketsAdd(bytes, CopyTextBytes(state.OwnerType)) ||
					!SourceCommonSocketsAdd(bytes, CopyTextBytes(state.Name)))
					return UINT64_MAX;
			return bytes;
		}
		bool Fits(uint64_t cap, std::initializer_list<uint64_t> parts) {
			uint64_t total = 0;
			for (auto bytes : parts)
				if (!SourceCommonSocketsAdd(total, bytes) || total > cap) return false;
			return true;
		}
	} // namespace
	uint64_t RetainedSourceCommonSocketBytes(const SourceCommonSocketSession &session) {
		uint64_t bytes = sizeof(session) + session.Owners.capacity() * sizeof(SourceCommonSocketState);
		for (const auto &state : session.Owners)
			if (!SourceCommonSocketsAdd(bytes, StringBytes(state.OwnerId)) ||
				!SourceCommonSocketsAdd(bytes, StringBytes(state.OwnerType)) ||
				!SourceCommonSocketsAdd(bytes, StringBytes(state.Name)))
				return UINT64_MAX;
		return bytes;
	}
	uint64_t RetainedSourceCommonReceiptBytes(const std::vector<SourceCommonStepReceipt> &receipts) {
		uint64_t bytes = sizeof(receipts) + receipts.capacity() * sizeof(SourceCommonStepReceipt);
		for (const auto &receipt : receipts)
			if (!SourceCommonSocketsAdd(bytes, StringBytes(receipt.OwnerId))) return UINT64_MAX;
		return bytes;
	}
	Status InitializeSourceCommonSockets(
		std::span<const SourceCommonOwner> owners,
		SourceCommonSocketSession &session,
		Diagnostic &diagnostic,
		uint64_t maximumBytes
	) try {
		ENGINE_PROFILE("imagegraph.common_sockets.initialize");
		if (!Cap(maximumBytes))
			return SourceCommonSocketsFail(
				diagnostic, Status::LimitExceeded, "common socket byte cap is invalid"
			);
		uint64_t inputs = 0;
		if (Owners(owners, diagnostic, inputs) != Status::Ok || Session(session, diagnostic) != Status::Ok)
			return diagnostic.Code;
		uint64_t candidateBytes = sizeof(session) + owners.size() * sizeof(SourceCommonSocketState);
		for (const auto &owner : owners)
			if (!SourceCommonSocketsAdd(
					candidateBytes,
					CopyTextBytes(owner.OwnerId) + CopyTextBytes(owner.OwnerType) + CopyTextBytes({})
				))
				return SourceCommonSocketsFail(
					diagnostic, Status::LimitExceeded, "common socket constructor bytes overflow"
				);
		if (!Fits(maximumBytes, {inputs, RetainedSourceCommonSocketBytes(session), candidateBytes}))
			return SourceCommonSocketsFail(
				diagnostic, Status::LimitExceeded, "common socket constructor overlap exceeds bounds"
			);
		SourceCommonSocketSession candidate;
		candidate.Owners.reserve(owners.size());
		for (const auto &owner : owners)
			candidate.Owners.push_back(
				{std::string(owner.OwnerId), std::string(owner.OwnerType), false, {}, {}}
			);
		if (!Fits(
				maximumBytes,
				{inputs, RetainedSourceCommonSocketBytes(session), RetainedSourceCommonSocketBytes(candidate)}
			))
			return SourceCommonSocketsFail(
				diagnostic, Status::LimitExceeded, "common socket constructor capacities exceed bounds"
			);
		session = std::move(candidate);
		diagnostic = {};
		return Status::Ok;
	} catch (const std::bad_alloc &) {
		return SourceCommonSocketsFail(diagnostic, Status::LimitExceeded, "common socket allocation failed");
	}
	Status BeginSourceCommonStep(
		std::span<const SourceCommonOwner> owners,
		std::span<const SourceCommonStepCapture> captures,
		SourceCommonSocketSession &session,
		std::vector<SourceCommonStepReceipt> &receipts,
		Diagnostic &diagnostic,
		uint64_t maximumBytes
	) try {
		ENGINE_PROFILE("imagegraph.common_sockets.step");
		if (!Cap(maximumBytes) || captures.size() > Limits::MaximumSourceCommonOwners ||
			receipts.capacity() > Limits::MaximumSourceCommonOwners)
			return SourceCommonSocketsFail(
				diagnostic, Status::LimitExceeded, "common step bounds are invalid"
			);
		uint64_t inputs = 0;
		if (Owners(owners, diagnostic, inputs) != Status::Ok || Session(session, diagnostic) != Status::Ok)
			return diagnostic.Code;
		if (owners.size() != session.Owners.size())
			return SourceCommonSocketsFail(
				diagnostic, Status::InvalidValue, "common step owner membership is stale"
			);
		uint64_t replacementNames = 0, resetBytes = 0, captureTextWork = 0;
		size_t captureIndex = 0, resets = 0;
		for (size_t index = 0; index < owners.size(); ++index) {
			const auto &owner = owners[index];
			const auto &state = session.Owners[index];
			if (owner.OwnerId != state.OwnerId || owner.OwnerType != state.OwnerType)
				return SourceCommonSocketsFail(
					diagnostic,
					Status::InvalidValue,
					"common step owner order or type is stale",
					owner.OwnerId
				);
			if (!owner.Active) continue;
			if (captureIndex == captures.size())
				return SourceCommonSocketsFail(
					diagnostic, Status::InvalidValue, "common step active capture is absent", owner.OwnerId
				);
			const auto &capture = captures[captureIndex++];
			if (capture.OwnerId != owner.OwnerId || capture.OwnerType != owner.OwnerType)
				return SourceCommonSocketsFail(
					diagnostic,
					Status::InvalidValue,
					"common step capture order or type is stale",
					owner.OwnerId
				);
			if (!SourceCommonSocketsAdd(inputs, capture.OwnerId.size() + capture.OwnerType.size()))
				return SourceCommonSocketsFail(
					diagnostic, Status::LimitExceeded, "common step capture bytes overflow"
				);
			if (owner.ShowUpdateTrigger) {
				if (!capture.UpdateRequested)
					return SourceCommonSocketsFail(
						diagnostic,
						Status::UnsupportedExecution,
						"common step Update predicate needs a capture",
						owner.OwnerId
					);
				if (*capture.UpdateRequested) {
					if (!capture.DirectUpdateCompleted || !*capture.DirectUpdateCompleted)
						return SourceCommonSocketsFail(
							diagnostic,
							Status::UnsupportedExecution,
							"common step direct update needs a completed capture",
							owner.OwnerId
						);
					++resets;
					if (!SourceCommonSocketsAdd(resetBytes, CopyTextBytes(owner.OwnerId)))
						return SourceCommonSocketsFail(
							diagnostic, Status::LimitExceeded, "common step reset bytes overflow"
						);
				}
			}
			if (capture.Metadata) {
				if (capture.Metadata->Name.size() > Limits::MaximumTextBytes ||
					!SourceCommonSocketsAdd(captureTextWork, capture.Metadata->Name.size()) ||
					captureTextWork > WorkLimit)
					return SourceCommonSocketsFail(
						diagnostic, Status::LimitExceeded, "common step captured text exceeds bounds"
					);
				if (!Text(capture.Metadata->Name, false) || !std::isfinite(capture.Metadata->Position.X) ||
					!std::isfinite(capture.Metadata->Position.Y))
					return SourceCommonSocketsFail(
						diagnostic,
						Status::InvalidValue,
						"common step metadata capture is invalid",
						owner.OwnerId
					);
				if (!SourceCommonSocketsAdd(inputs, capture.Metadata->Name.size()))
					return SourceCommonSocketsFail(
						diagnostic, Status::LimitExceeded, "common step captured text bytes overflow"
					);
			}
			if (owner.OutMeta) {
				if (!capture.Metadata)
					return SourceCommonSocketsFail(
						diagnostic,
						Status::UnsupportedExecution,
						"common step metadata needs a capture",
						owner.OwnerId
					);
				if (!SourceCommonSocketsAdd(replacementNames, CopyTextBytes(capture.Metadata->Name)))
					return SourceCommonSocketsFail(
						diagnostic, Status::LimitExceeded, "common step metadata bytes overflow"
					);
			}
		}
		if (captureIndex != captures.size())
			return SourceCommonSocketsFail(
				diagnostic, Status::InvalidValue, "common step has surplus captures"
			);
		if (!SourceCommonSocketsAdd(inputs, captures.size_bytes()))
			return SourceCommonSocketsFail(
				diagnostic, Status::LimitExceeded, "common step capture bytes overflow"
			);
		const auto priorReceipts = RetainedSourceCommonReceiptBytes(receipts);
		const auto nextReceipts = sizeof(receipts) + resets * sizeof(SourceCommonStepReceipt) + resetBytes;
		if (!Fits(
				maximumBytes,
				{inputs,
				 RetainedSourceCommonSocketBytes(session),
				 priorReceipts,
				 CloneBytes(session),
				 replacementNames,
				 nextReceipts}
			))
			return SourceCommonSocketsFail(
				diagnostic, Status::LimitExceeded, "common step replacement overlap exceeds bounds"
			);
		SourceCommonSocketSession candidate = session;
		std::vector<SourceCommonStepReceipt> next;
		next.reserve(resets);
		captureIndex = 0;
		for (size_t index = 0; index < owners.size(); ++index) {
			const auto &owner = owners[index];
			if (!owner.Active) continue;
			const auto &capture = captures[captureIndex++];
			auto &state = candidate.Owners[index];
			if (owner.ShowUpdateTrigger) {
				if (*capture.UpdateRequested) next.push_back({state.OwnerId, true});
				state.Updated = false;
			}
			if (owner.OutMeta) {
				std::string replacement(capture.Metadata->Name);
				state.Name.swap(replacement);
				state.Position = capture.Metadata->Position;
			}
		}
		if (!Fits(
				maximumBytes,
				{inputs,
				 RetainedSourceCommonSocketBytes(session),
				 priorReceipts,
				 RetainedSourceCommonSocketBytes(candidate),
				 RetainedSourceCommonReceiptBytes(next)}
			))
			return SourceCommonSocketsFail(
				diagnostic, Status::LimitExceeded, "common step replacement capacities exceed bounds"
			);
		session = std::move(candidate);
		receipts = std::move(next);
		diagnostic = {};
		return Status::Ok;
	} catch (const std::bad_alloc &) {
		return SourceCommonSocketsFail(diagnostic, Status::LimitExceeded, "common step allocation failed");
	}
	Status CompleteSourceCommonFullUpdates(
		std::span<const SourceCommonFullUpdateCapture> captures,
		SourceCommonSocketSession &session,
		Diagnostic &diagnostic,
		uint64_t maximumBytes
	) try {
		ENGINE_PROFILE("imagegraph.common_sockets.complete");
		if (!Cap(maximumBytes) || captures.size() > Limits::MaximumSourceCommonOwners)
			return SourceCommonSocketsFail(
				diagnostic, Status::LimitExceeded, "common full-update bounds are invalid"
			);
		if (Session(session, diagnostic) != Status::Ok) return diagnostic.Code;
		uint64_t inputs = captures.size_bytes(), textWork = 0, identityBytes = 0, retainedIdentityBytes = 0;
		for (const auto &capture : captures) {
			if (capture.OwnerId.size() > Limits::MaximumTextBytes ||
				capture.OwnerType.size() > Limits::MaximumTextBytes)
				return SourceCommonSocketsFail(
					diagnostic, Status::LimitExceeded, "common full-update identity text exceeds bounds"
				);
			if (!SourceCommonSocketsAdd(inputs, capture.OwnerId.size() + capture.OwnerType.size()) ||
				!SourceCommonSocketsAdd(textWork, capture.OwnerId.size() + capture.OwnerType.size()) ||
				!SourceCommonSocketsAdd(identityBytes, capture.OwnerId.size()))
				return SourceCommonSocketsFail(
					diagnostic, Status::LimitExceeded, "common full-update identity bytes overflow"
				);
		}
		for (const auto &state : session.Owners)
			if (!SourceCommonSocketsAdd(retainedIdentityBytes, state.OwnerId.size()))
				return SourceCommonSocketsFail(
					diagnostic, Status::LimitExceeded, "common full-update owner bytes overflow"
				);
		if (textWork > WorkLimit || textWork + identityBytes * (captures.size() + 2 * session.Owners.size()) +
											2 * retainedIdentityBytes * captures.size() +
											captures.size() * captures.size() >
										WorkLimit)
			return SourceCommonSocketsFail(
				diagnostic, Status::LimitExceeded, "common full-update comparison exceeds bounds"
			);
		for (size_t index = 0; index < captures.size(); ++index) {
			const auto &capture = captures[index];
			if (!Text(capture.OwnerId, true) || !Text(capture.OwnerType, true))
				return SourceCommonSocketsFail(
					diagnostic, Status::InvalidValue, "common full-update identity is invalid"
				);
			for (size_t previous = 0; previous < index; ++previous)
				if (capture.OwnerId == captures[previous].OwnerId)
					return SourceCommonSocketsFail(
						diagnostic,
						Status::DuplicateId,
						"common full-update owner is duplicated",
						capture.OwnerId
					);
			const auto found =
				std::find_if(session.Owners.begin(), session.Owners.end(), [&](const auto &state) {
					return state.OwnerId == capture.OwnerId;
				});
			if (found == session.Owners.end() || found->OwnerType != capture.OwnerType)
				return SourceCommonSocketsFail(
					diagnostic,
					Status::InvalidValue,
					"common full-update owner or type is stale",
					capture.OwnerId
				);
			if (!capture.SafeMode && (!capture.Completed || !*capture.Completed))
				return SourceCommonSocketsFail(
					diagnostic,
					Status::UnsupportedExecution,
					"common full-update needs a completed capture",
					capture.OwnerId
				);
		}
		if (!Fits(maximumBytes, {inputs, RetainedSourceCommonSocketBytes(session), CloneBytes(session)}))
			return SourceCommonSocketsFail(
				diagnostic, Status::LimitExceeded, "common full-update replacement overlap exceeds bounds"
			);
		SourceCommonSocketSession candidate = session;
		for (const auto &capture : captures)
			if (!capture.SafeMode) {
				const auto found =
					std::find_if(candidate.Owners.begin(), candidate.Owners.end(), [&](const auto &state) {
						return state.OwnerId == capture.OwnerId;
					});
				found->Updated = true;
			}
		if (!Fits(
				maximumBytes,
				{inputs, RetainedSourceCommonSocketBytes(session), RetainedSourceCommonSocketBytes(candidate)}
			))
			return SourceCommonSocketsFail(
				diagnostic, Status::LimitExceeded, "common full-update capacities exceed bounds"
			);
		session = std::move(candidate);
		diagnostic = {};
		return Status::Ok;
	} catch (const std::bad_alloc &) {
		return SourceCommonSocketsFail(
			diagnostic, Status::LimitExceeded, "common full-update allocation failed"
		);
	}
} // namespace engine::imagegraph
