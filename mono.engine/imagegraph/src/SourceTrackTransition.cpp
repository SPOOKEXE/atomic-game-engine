#include "EvaluationAllocator.hpp"
#include "SourceAnimatorPersistence.hpp"

#include <engine/core/Profiling.hpp>
#include <engine/imagegraph/SourceTrackTransition.hpp>

namespace engine::imagegraph {
	namespace {
		struct TrackTransitionWorkExceeded {};
	}
	Status ApplySourceTrackTransition(
		const Document &document,
		const SourceTrackTransition &transition,
		Document &result,
		Diagnostic &diagnostic,
		uint64_t maximumBytes
	) try {
		ENGINE_PROFILE("imagegraph.source_track_transition");
		const auto fail = [&](Status code, const char *message) {
			diagnostic = {code, {}, {}, message};
			return code;
		};
		if (!maximumBytes || maximumBytes > Limits::MaximumEvaluationBytes ||
			transition.NodeId.size() > Limits::MaximumTextBytes ||
			transition.Port.size() > Limits::MaximumTextBytes)
			return fail(Status::LimitExceeded, "source track target or byte bound is invalid");
		const auto *replacement = transition.Replacement;
		if (transition.SourceInterpolation && !replacement)
			return fail(Status::InvalidValue, "source normalization requires a track override");
		if (replacement &&
			(replacement->NodeId != transition.NodeId || replacement->Port != transition.Port ||
			 (replacement->End != "hold" && replacement->End != "loop" && replacement->End != "ping" &&
			  replacement->End != "wrap") ||
			 replacement->LoopRange < -1 ||
			 (replacement->QuaternionMode && *replacement->QuaternionMode != 0 &&
			  *replacement->QuaternionMode != 1)))
			return fail(Status::InvalidValue, "source track replacement is invalid");
		const auto oldBytes = DocumentRetainedPayloadBytes(document);
		const auto priorBytes =
			&document == &result ? std::optional<uint64_t>{0} : DocumentRetainedPayloadBytes(result);
		uint64_t borrowed = sizeof(SourceTrackTransition) + transition.NodeId.size() + transition.Port.size();
		if (!oldBytes || !priorBytes || !detail::SourceAnimatorAdd(borrowed, *oldBytes) ||
			!detail::SourceAnimatorAdd(borrowed, *priorBytes))
			return fail(Status::LimitExceeded, "source track document overlap exceeds bounds");
		if (replacement && (!detail::SourceAnimatorAdd(borrowed, sizeof(AnimationTrack)) ||
							!detail::SourceAnimatorText(borrowed, replacement->NodeId, false) ||
							!detail::SourceAnimatorText(borrowed, replacement->Port, false) ||
							!detail::SourceAnimatorText(borrowed, replacement->End, false)))
			return fail(Status::LimitExceeded, "source track borrowed payload exceeds bounds");
		detail::EvaluationBudget budget(maximumBytes);
		auto held = budget.Reserve(borrowed);
		if (!held) return fail(Status::LimitExceeded, "source track borrowed overlap exceeds bounds");
		if (detail::ValidateSourceAnimatorState(document, diagnostic) != Status::Ok) return diagnostic.Code;
		uint64_t work = 0;
		const auto admit = [&](uint64_t amount) {
			if (amount > 64'000'000 - work) throw TrackTransitionWorkExceeded{};
			work += amount;
		};
		const auto same = [&](std::string_view a, std::string_view b) {
			admit(1 + std::min(a.size(), b.size()));
			return a == b;
		};
		bool targetExists = false;
		for (const auto &node : document.Nodes)
			if (same(node.Id, transition.NodeId)) targetExists = true;
		if (!targetExists) return fail(Status::UnknownNode, "source track target does not exist");
		std::string_view owner = transition.NodeId, port = transition.Port;
		if (document.SourceAnimators)
			for (const auto &binding : document.SourceAnimators->Bindings)
				if (same(binding.NodeId, owner) && same(binding.Port, port)) {
					owner = binding.OwnerId;
					port = binding.AnimatorPort.empty() ? binding.Port : binding.AnimatorPort;
					break;
				}
		size_t detached = SIZE_MAX;
		if (document.SourceAnimators)
			for (size_t index = 0; index < document.SourceAnimators->Detached.size(); ++index) {
				const auto &record = document.SourceAnimators->Detached[index];
				if (same(record.OwnerId, owner) && same(record.Id, port)) {
					detached = index;
					break;
				}
			}
		using Target = std::pair<std::string_view, std::string_view>;
		detail::EvaluationVector<Target> targets{detail::EvaluationAllocator<Target>(budget)};
		if (detached == SIZE_MAX) targets.emplace_back(owner, port);
		if (document.SourceAnimators)
			for (const auto &binding : document.SourceAnimators->Bindings)
				if (same(binding.OwnerId, owner) &&
					same(binding.AnimatorPort.empty() ? binding.Port : binding.AnimatorPort, port))
					targets.emplace_back(binding.NodeId, binding.Port);
		const auto selected = [&](std::string_view node, std::string_view input) {
			for (const auto &[targetNode, targetPort] : targets)
				if (same(node, targetNode) && same(input, targetPort)) return true;
			return false;
		};
		bool hasTrack =
			detached != SIZE_MAX && document.SourceAnimators->Detached[detached].Track.has_value();
		for (const auto &track : document.Tracks)
			if (selected(track.NodeId, track.Port)) hasTrack = true;
		if (!replacement && !hasTrack)
			return fail(Status::InvalidValue, "source track override does not exist");
		uint64_t ownedBytes = 0;
		if (*oldBytes > UINT64_MAX / 2 || !detail::SourceAnimatorAdd(ownedBytes, 2 * *oldBytes))
			return fail(Status::LimitExceeded, "source track clone overlap overflows");
		size_t finalCount = 0;
		for (const auto &track : document.Tracks)
			if (!selected(track.NodeId, track.Port)) ++finalCount;
		if (replacement) {
			if (targets.size() > Limits::MaximumTracks - finalCount)
				return fail(Status::LimitExceeded, "source track fanout exceeds count bounds");
			finalCount += targets.size();
			for (const auto &[node, input] : targets)
				if (!detail::SourceAnimatorAdd(
						ownedBytes,
						2 * (sizeof(AnimationTrack) + node.size() + input.size() + replacement->End.size() +
							 64)
					))
					throw std::bad_alloc{};
		}
		if (!detail::SourceAnimatorAdd(ownedBytes, finalCount * sizeof(AnimationTrack)))
			throw std::bad_alloc{};
		if (transition.SourceInterpolation) {
			uint64_t keys = document.Keyframes.size();
			if (document.SourceAnimators)
				for (const auto &record : document.SourceAnimators->DetachedValues)
					keys += record.Keys.size();
			if (!detail::SourceAnimatorAdd(ownedBytes, keys * 64)) throw std::bad_alloc{};
		}
		auto owned = budget.Reserve(ownedBytes);
		if (!owned) return fail(Status::LimitExceeded, "source track clone and fanout exceed byte bounds");
		Document candidate = document;
		std::vector<AnimationTrack> tracks;
		tracks.reserve(finalCount);
		for (const auto &track : document.Tracks)
			if (!selected(track.NodeId, track.Port)) tracks.push_back(track);
		if (replacement)
			for (const auto &[node, input] : targets) {
				auto track = *replacement;
				track.NodeId = node;
				track.Port = input;
				tracks.push_back(std::move(track));
			}
		candidate.Tracks = std::move(tracks);
		if (detached != SIZE_MAX) {
			auto &metadata = candidate.SourceAnimators->Detached[detached];
			if (replacement) {
				metadata.Track = *replacement;
				metadata.Track->NodeId = owner;
				metadata.Track->Port = port;
			} else
				metadata.Track.reset();
		}
		if (transition.SourceInterpolation) {
			const auto normalize = [](Keyframe &key) {
				key.Interpolation = "source";
				if (!key.Ease) key.Ease = KeyframeEase{};
			};
			for (auto &key : candidate.Keyframes)
				if (selected(key.NodeId, key.Port)) normalize(key);
			if (detached != SIZE_MAX)
				for (auto &record : candidate.SourceAnimators->DetachedValues)
					if (same(record.NodeId, owner) && same(record.Port, port))
						for (auto &key : record.Keys) {
							admit(1);
							normalize(key);
						}
		}
		candidate.FormatVersion = std::max(candidate.FormatVersion, uint32_t{4});
		if (replacement && replacement->QuaternionMode)
			candidate.FormatVersion = std::max(candidate.FormatVersion, uint32_t{8});
		Plan plan;
		if (Compile(candidate, plan, diagnostic, budget.Available()) != Status::Ok) return diagnostic.Code;
		result = std::move(candidate);
		diagnostic = {};
		return Status::Ok;
	} catch (const TrackTransitionWorkExceeded &) {
		diagnostic = {Status::LimitExceeded, {}, {}, "source track comparison work exceeds bounds"};
		return diagnostic.Code;
	} catch (const std::bad_alloc &) {
		diagnostic = {Status::LimitExceeded, {}, {}, "source track allocation exceeds bounds"};
		return diagnostic.Code;
	}
}
