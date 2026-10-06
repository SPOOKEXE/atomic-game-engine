#pragma once

#include <engine/imagegraph/Document.hpp>
#include <engine/imagegraph/FrameTime.hpp>
#include <engine/imagegraph/SourceKeyframeTransition.hpp>

#include <algorithm>
#include <new>

namespace studio {
	// grug map only ids cleared by a newly captured alias. every payload field still matches.
	template <class Edit>
	bool WithImageGraphProjectedKeyPins(
		const engine::imagegraph::Document &authored,
		engine::imagegraph::Document &projected,
		std::span<const engine::imagegraph::Keyframe> originals,
		uint64_t availableBytes,
		const Edit &edit,
		engine::imagegraph::Diagnostic &error,
		std::span<const int8_t> axes = {}
	) try {
		using namespace engine::imagegraph;
		const auto fail = [&](Status status, const char *message) {
			error = {status, {}, {}, message};
			return false;
		};
		if (!axes.empty() && axes.size() != originals.size())
			return fail(Status::InvalidValue, "projected key selectors do not match their pins");
		if (std::any_of(axes.begin(), axes.end(), [](int8_t axis) { return axis != -1; })) {
			if (originals.size() > Limits::MaximumKeyframes)
				return fail(Status::LimitExceeded, "projected scalar key selection exceeds bounds");
			const auto authoredBytes = DocumentRetainedPayloadBytes(authored),
					   projectedBytes = DocumentRetainedPayloadBytes(projected);
			uint64_t remaining = std::min(availableBytes, Limits::MaximumEvaluationBytes);
			const uint64_t scratch = originals.size() * sizeof(SourceKeyframeIdentity) + axes.size();
			if (!authoredBytes || !projectedBytes || scratch > remaining)
				return fail(Status::LimitExceeded, "projected scalar key residency exceeds bounds");
			remaining -= scratch;
			uint64_t workRemaining = 64'000'000;
			const uint64_t records =
				(authored.SourceAnimators ? authored.SourceAnimators->Bindings.size() : 0) +
				(projected.SourceAnimators ? projected.SourceAnimators->Bindings.size() : 0);
			for (const auto &pin : originals) {
				const auto bytes = KeyframePayloadBytes(pin);
				const uint64_t names = 1 + pin.NodeId.size() + pin.Port.size();
				if (!bytes || *bytes > remaining || *bytes > workRemaining / 4 ||
					(records && names > (workRemaining - 4 * *bytes) / records))
					return fail(Status::LimitExceeded, "projected scalar key comparison exceeds bounds");
				remaining -= *bytes;
				workRemaining -= 4 * *bytes + records * names;
			}
			if (*projectedBytes > remaining)
				return fail(Status::LimitExceeded, "projected scalar key document overlap exceeds bounds");
			std::vector<SourceKeyframeIdentity> identities;
			identities.reserve(originals.size());
			const uint64_t spareIdentities =
				(identities.capacity() - originals.size()) * sizeof(SourceKeyframeIdentity);
			if (spareIdentities > remaining - *projectedBytes)
				return fail(Status::LimitExceeded, "projected scalar identity capacity exceeds bounds");
			remaining -= spareIdentities;
			for (size_t index = 0; index < originals.size(); ++index)
				identities.push_back(
					{originals[index].NodeId,
					 originals[index].Port,
					 GetFrameTime(originals[index]),
					 axes[index]}
				);
			std::vector<Keyframe> beforePins, pins;
			if (CaptureSourceKeyframes(
					authored, identities, beforePins, error, remaining - *projectedBytes
				) != Status::Ok)
				return false;
			uint64_t beforeBytes = (beforePins.capacity() - beforePins.size()) * sizeof(Keyframe);
			if (beforeBytes > remaining)
				return fail(Status::LimitExceeded, "projected scalar snapshot capacity exceeds bounds");
			for (size_t index = 0; index < beforePins.size(); ++index) {
				if (beforePins[index] != originals[index])
					return fail(Status::InvalidValue, "scalar key changed before source writer capture");
				const auto bytes = KeyframePayloadBytes(beforePins[index]);
				if (!bytes || *bytes > remaining - beforeBytes)
					return fail(Status::LimitExceeded, "projected scalar key snapshot exceeds bounds");
				beforeBytes += *bytes;
			}
			if (beforeBytes > remaining || *authoredBytes > remaining - beforeBytes)
				return fail(Status::LimitExceeded, "projected scalar key snapshot overlap exceeds bounds");
			if (CaptureSourceKeyframes(
					projected, identities, pins, error, remaining - beforeBytes - *authoredBytes
				) != Status::Ok)
				return false;
			for (size_t index = 0; index < pins.size(); ++index) {
				auto &before = beforePins[index];
				if (before != pins[index] && projected.SourceAnimators) {
					const auto owns = [&](const auto &binding) {
						return binding.NodeId == before.NodeId && binding.Port == before.Port;
					};
					const bool previouslyBound =
						authored.SourceAnimators && std::any_of(
														authored.SourceAnimators->Bindings.begin(),
														authored.SourceAnimators->Bindings.end(),
														owns
													);
					if (!previouslyBound) {
						const auto alias = std::find_if(
							projected.SourceAnimators->Bindings.begin(),
							projected.SourceAnimators->Bindings.end(),
							owns
						);
						if (alias != projected.SourceAnimators->Bindings.end() &&
							(axes[index] < 0
								 ? (alias->OwnerId != before.NodeId || !alias->AnimatorPort.empty())
								 : (alias->Axes.OwnerId != before.NodeId || alias->Axes.Port != before.Port)))
							before.SourceKeyId.clear();
					}
				}
				if (before != pins[index])
					return fail(Status::InvalidValue, "scalar key changed during source writer capture");
			}
			uint64_t pinBytes = (pins.capacity() - pins.size()) * sizeof(Keyframe);
			if (pinBytes > remaining)
				return fail(Status::LimitExceeded, "projected scalar pin capacity exceeds bounds");
			for (const auto &pin : pins) {
				const auto bytes = KeyframePayloadBytes(pin);
				if (!bytes || *bytes > remaining - pinBytes)
					return fail(Status::LimitExceeded, "projected scalar key pins exceed bounds");
				pinBytes += *bytes;
			}
			if (beforeBytes > remaining || pinBytes > remaining - beforeBytes)
				return fail(Status::LimitExceeded, "projected scalar pin overlap exceeds bounds");
			return edit(std::span<const Keyframe>{pins}, remaining - beforeBytes - pinBytes);
		}

		if (originals.size() > Limits::MaximumKeyframes ||
			authored.Keyframes.size() > Limits::MaximumKeyframes ||
			projected.Keyframes.size() > Limits::MaximumKeyframes ||
			uint64_t(originals.size()) *
					(authored.Keyframes.size() + 3 * projected.Keyframes.size() +
					 (authored.SourceAnimators ? authored.SourceAnimators->Bindings.size() : 0) +
					 (projected.SourceAnimators ? projected.SourceAnimators->Bindings.size() : 0)) >
				64'000'000)
			return fail(Status::LimitExceeded, "projected key selection exceeds work bounds");
		const uint64_t scanRecords =
			authored.Keyframes.size() + 3 * projected.Keyframes.size() +
			(authored.SourceAnimators ? authored.SourceAnimators->Bindings.size() : 0) +
			(projected.SourceAnimators ? projected.SourceAnimators->Bindings.size() : 0);
		uint64_t workRemaining = 64'000'000;
		const auto findPinned = [](const Document &document, const Keyframe &pin) {
			return std::find_if(document.Keyframes.begin(), document.Keyframes.end(), [&](const auto &key) {
				return key.NodeId == pin.NodeId && key.Port == pin.Port &&
					   GetFrameTime(key) == GetFrameTime(pin) && key == pin;
			});
		};
		uint64_t remaining = std::min(availableBytes, Limits::MaximumEvaluationBytes);
		for (const auto &key : originals) {
			const auto bytes = KeyframePayloadBytes(key);
			const uint64_t identityWork = 1 + key.NodeId.size() + key.Port.size();
			if (!bytes || identityWork > workRemaining / std::max(uint64_t{1}, scanRecords))
				return fail(Status::LimitExceeded, "projected key comparison exceeds work bounds");
			workRemaining -= identityWork * scanRecords;
			if (*bytes > workRemaining / 4)
				return fail(Status::LimitExceeded, "projected key payload comparison exceeds work bounds");
			workRemaining -= 4 * *bytes;
			if (!bytes || *bytes > remaining)
				return fail(Status::LimitExceeded, "projected key pins exceed payload bounds");
			remaining -= *bytes;
		}
		std::vector<Keyframe> pins;
		pins.reserve(originals.size());
		for (const auto &original : originals) {
			if (findPinned(authored, original) == authored.Keyframes.end())
				return fail(Status::InvalidValue, "key changed before source writer capture");
			pins.push_back(original);
			auto &pin = pins.back();
			if (findPinned(projected, pin) == projected.Keyframes.end() && projected.SourceAnimators &&
				(!authored.SourceAnimators || std::none_of(
												  authored.SourceAnimators->Bindings.begin(),
												  authored.SourceAnimators->Bindings.end(),
												  [&](const auto &binding) {
													  return binding.NodeId == pin.NodeId &&
															 binding.Port == pin.Port;
												  }
											  ))) {
				const auto alias = std::find_if(
					projected.SourceAnimators->Bindings.begin(),
					projected.SourceAnimators->Bindings.end(),
					[&](const auto &binding) {
						return binding.NodeId == pin.NodeId && binding.Port == pin.Port &&
							   (binding.OwnerId != pin.NodeId || !binding.AnimatorPort.empty());
					}
				);
				if (alias != projected.SourceAnimators->Bindings.end()) pin.SourceKeyId.clear();
			}
			if (findPinned(projected, pin) == projected.Keyframes.end())
				return fail(Status::InvalidValue, "key changed during source writer capture");
		}
		const uint64_t spare = (pins.capacity() - pins.size()) * sizeof(Keyframe);
		if (spare >= remaining)
			return fail(Status::LimitExceeded, "projected key pin allocation exceeds bounds");
		return edit(std::span<const Keyframe>{pins}, remaining - spare);
	} catch (const std::bad_alloc &) {
		error = {engine::imagegraph::Status::LimitExceeded, {}, {}, "projected key pin allocation failed"};
		return false;
	}
}
