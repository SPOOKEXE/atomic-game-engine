#pragma once

#include <engine/imagegraph/Document.hpp>
#include <engine/imagegraph/FrameTime.hpp>

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
		engine::imagegraph::Diagnostic &error
	) try {
		using namespace engine::imagegraph;
		const auto fail = [&](Status status, const char *message) {
			error = {status, {}, {}, message};
			return false;
		};
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
