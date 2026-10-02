#include "EvaluationAllocator.hpp"
#include "GroupReplayInternal.hpp"
#include "TimelineOverrides.hpp"
#include "ValuePayload.hpp"

#include <engine/core/Profiling.hpp>
#include <engine/imagegraph/SourceAnimatorCapture.hpp>
#include <engine/imagegraph/SourceModeTransition.hpp>

#include <algorithm>
#include <new>

namespace engine::imagegraph {
	namespace {
		uint64_t TextBytes(std::string_view text) {
			return std::max(text.size(), std::string{}.capacity()) + 1;
		}
		bool Add(uint64_t &bytes, uint64_t extra) {
			if (extra > UINT64_MAX - bytes) return false;
			bytes += extra;
			return true;
		}
		bool BoundedCounts(const Document &document) {
			if (document.Nodes.size() > Limits::MaximumNodes ||
				document.Groups.size() > Limits::MaximumGroups ||
				document.Junctions.size() > Limits::MaximumJunctions ||
				document.Links.size() > Limits::MaximumLinks ||
				document.Keyframes.size() > Limits::MaximumKeyframes ||
				document.Tracks.size() > Limits::MaximumTracks ||
				document.Outputs.size() > Limits::MaximumOutputs)
				return false;
			for (const auto &node : document.Nodes)
				if (node.Values.size() > Limits::MaximumArrayElements ||
					node.DynamicInputs.size() > MaximumDynamicInputsForNode(node) ||
					node.InstanceOverrides.size() > Limits::MaximumArrayElements ||
					node.SourceAnimatedInputs.size() > Limits::MaximumArrayElements ||
					node.SourceStaticInputs.size() > Limits::MaximumArrayElements)
					return false;
			for (const auto &group : document.Groups)
				if (group.Ports.size() > Limits::MaximumGroupPorts) return false;
			return true;
		}
		bool Mode(const Node &node, std::string_view port, bool &animated) {
			const bool on =
				std::find(node.SourceAnimatedInputs.begin(), node.SourceAnimatedInputs.end(), port) !=
				node.SourceAnimatedInputs.end();
			const bool off =
				std::find(node.SourceStaticInputs.begin(), node.SourceStaticInputs.end(), port) !=
				node.SourceStaticInputs.end();
			if (on == off) return false;
			animated = on;
			return true;
		}
		std::optional<bool> ArrayClassification(
			const Node &owner,
			std::string_view port,
			const CatalogueInput &input,
			const GroupReplayState &replay
		) {
			if (owner.Type != "pc.group_input" || port != "parent_value")
				return input.SourceArrayClassification;
			const auto *entry = replay.Find(owner.Id);
			if (!entry || !entry->Domain.Kind || !entry->Domain.Display) return std::nullopt;
			if (entry->Domain.Kind == SourceSocketKind::Curve) return true;
			switch (*entry->Domain.Display) {
			case SourceValueDisplay::Range:
			case SourceValueDisplay::RotationRange:
			case SourceValueDisplay::SliderRange:
			case SourceValueDisplay::Padding:
			case SourceValueDisplay::Vector:
			case SourceValueDisplay::VectorRange:
			case SourceValueDisplay::Area:
			case SourceValueDisplay::Palette:
			case SourceValueDisplay::Curve:
				return true;
			default:
				return false;
			}
		}
		const Value *Authored(const Node &node, std::string_view port) {
			for (const auto &value : node.Values)
				if (value.Port == port) return &value.Data;
			for (const auto &input : node.DynamicInputs)
				if (input.Id == port && input.Default) return &*input.Default;
			return nullptr;
		}
	}
	Status ToggleSourceInputMode(
		const Document &document,
		const GroupReplayState &replay,
		uint64_t authoringRevision,
		const SourceModeTransition &transition,
		Document &result,
		Diagnostic &diagnostic,
		uint64_t maximumBytes
	) try {
		ENGINE_PROFILE("imagegraph.source_mode_transition");
		const auto fail = [&](Status code, std::string message) {
			diagnostic = {
				code,
				transition.NodeId.size() <= Limits::MaximumTextBytes ? std::string(transition.NodeId)
																	 : std::string{},
				transition.Port.size() <= Limits::MaximumTextBytes ? std::string(transition.Port)
																   : std::string{},
				std::move(message)
			};
			return code;
		};
		if (!maximumBytes || maximumBytes > Limits::MaximumEvaluationBytes)
			return fail(Status::LimitExceeded, "source mode transaction byte bound is invalid");
		if (!ValidFrameTime(transition.Time) || transition.NodeId.size() > Limits::MaximumTextBytes ||
			transition.Port.size() > Limits::MaximumTextBytes)
			return fail(Status::InvalidValue, "source mode transaction target or clock is invalid");
		if (!BoundedCounts(document) || (&document != &result && !BoundedCounts(result)))
			return fail(Status::LimitExceeded, "source mode public document counts exceed bounds");
		const auto oldBytes = DocumentRetainedPayloadBytes(document);
		const auto priorBytes =
			&document == &result ? std::optional<uint64_t>{0} : DocumentRetainedPayloadBytes(result);
		uint64_t borrowed = oldBytes.value_or(0);
		if (!oldBytes || !priorBytes || !Add(borrowed, *priorBytes) || !Add(borrowed, replay.RetainedBytes()))
			return fail(Status::LimitExceeded, "source mode document payload exceeds bounds");
		detail::EvaluationBudget budget(maximumBytes);
		auto held = budget.Reserve(borrowed);
		if (!held) return fail(Status::LimitExceeded, "source mode borrowed document overlap exceeds bounds");
		FrameTime exact;
		if (!SplitFrameTime(double(FrameTimeToReal(transition.Time)), exact) || exact != transition.Time)
			return fail(Status::UnsupportedExecution, "source mode clock has no exact source real");
		const auto targetOriginal =
			std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const Node &node) {
				return node.Id == transition.NodeId;
			});
		if (targetOriginal == document.Nodes.end())
			return fail(Status::UnknownNode, "source mode target is absent");
		const auto *entry = FindCatalogueEntry(targetOriginal->Type);
		size_t dynamicGroup = 0;
		const auto *input = entry ? FindCatalogueInput(*entry, transition.Port) : nullptr;
		if (!input && entry) input = FindDynamicTemplate(*entry, transition.Port, dynamicGroup);
		const bool trigger = (input && input->SourceKind == "Trigger") ||
							 (transition.Port == "parent_value" && replay.Find(transition.NodeId) &&
							  replay.Find(transition.NodeId)->Domain.Kind == SourceSocketKind::Trigger);
		if (trigger && (transition.Time.Subframe != 0 || transition.Time.NegativeFrame))
			return fail(
				Status::UnsupportedExecution, "source Trigger map requires a nonnegative integer frame"
			);
		if (!input || (!detail::AliasedSourceInput(*targetOriginal, transition.Port) &&
					   !(targetOriginal->Type == "pc.group_input" && transition.Port == "parent_value")))
			return fail(Status::UnknownPort, "source mode target is not a supported source input");
		if (transition.Port == "parent_value" && targetOriginal->Type == "pc.group_input" &&
			!replay.Find(transition.NodeId))
			return fail(Status::InvalidValue, "source parent mode requires restored boundary declarations");

		if (!IsAuthoredValueType(input->Type) &&
			!(targetOriginal->Type == "pc.group_input" && transition.Port == "parent_value"))
			return fail(
				Status::UnsupportedExecution, "source mode input has no represented authored animator carrier"
			);
		bool wasAnimated;
		if (!Mode(*targetOriginal, transition.Port, wasAnimated))
			return fail(Status::InvalidValue, "source mode target needs explicit disjoint provenance");
		Document candidate;
		// Projection admits document/replay/candidate/scratch; the separate prior output remains live.
		auto status = ProjectGroupReplay(
			document, replay, authoringRevision, candidate, diagnostic, maximumBytes - *priorBytes
		);
		if (status != Status::Ok) return status;
		const auto candidateBytes = DocumentRetainedPayloadBytes(candidate);
		uint64_t live = borrowed;
		if (!candidateBytes || !Add(live, *candidateBytes) || !held->Resize(live))
			return fail(Status::LimitExceeded, "source mode live document overlap exceeds bounds");
		if (wasAnimated == transition.Animated) {
			result = std::move(candidate);
			diagnostic = {};
			return Status::Ok;
		}
		auto target = std::find_if(candidate.Nodes.begin(), candidate.Nodes.end(), [&](const Node &node) {
			return node.Id == transition.NodeId;
		});
		std::string_view ownerId = target->Id;
		if (const auto *binding = replay.Binding(transition.NodeId, transition.Port))
			ownerId = binding->OwnerId;
		else if (!target->InstanceBase.empty() && transition.Port != "parent_value")
			return fail(Status::InvalidValue, "source instance mode requires its original animator binding");
		auto owner = std::find_if(candidate.Nodes.begin(), candidate.Nodes.end(), [&](const Node &node) {
			return node.Id == ownerId;
		});
		if (owner == candidate.Nodes.end())
			return fail(Status::UnknownNode, "source animator owner is absent");
		bool ownerAnimated;
		if (!Mode(*owner, transition.Port, ownerAnimated))
			return fail(Status::InvalidValue, "source animator owner needs explicit provenance");
		const bool ownerWasAnimated = ownerAnimated;
		auto &removedModes = transition.Animated ? target->SourceStaticInputs : target->SourceAnimatedInputs;
		auto &addedModes = transition.Animated ? target->SourceAnimatedInputs : target->SourceStaticInputs;
		if (addedModes.size() == Limits::MaximumArrayElements)
			return fail(Status::LimitExceeded, "source mode declaration count exceeds bounds");
		uint64_t modeBytes = TextBytes(transition.Port);
		if (addedModes.size() == addedModes.capacity() &&
			!Add(modeBytes, (addedModes.size() + 1) * sizeof(std::string)))
			return fail(Status::LimitExceeded, "source mode declaration allocation overflows");
		auto modeCharge = budget.Reserve(modeBytes);
		if (!modeCharge) return fail(Status::LimitExceeded, "source mode declaration exceeds budget");
		if (addedModes.size() == addedModes.capacity()) addedModes.reserve(addedModes.size() + 1);
		addedModes.push_back(std::string(transition.Port));
		std::erase(removedModes, transition.Port);
		if (owner == target) ownerAnimated = transition.Animated;
		size_t count = 0;
		uint64_t sourceBytes = 0;
		for (const auto &key : candidate.Keyframes)
			if (key.NodeId == ownerId && key.Port == transition.Port) {
				if (key.Interpolation != "source")
					return fail(
						Status::UnsupportedExecution, "source mode transition requires source animator keys"
					);
				const auto bytes = KeyframePayloadBytes(key);
				if (!bytes || !Add(sourceBytes, *bytes))
					return fail(Status::LimitExceeded, "source animator key payload exceeds bounds");
				++count;
			}
		auto sourceCharge = budget.Reserve(sourceBytes);
		if (!sourceCharge) return fail(Status::LimitExceeded, "source animator snapshot exceeds budget");
		std::vector<Keyframe> keys;
		keys.reserve(count);
		for (const auto &key : candidate.Keyframes)
			if (key.NodeId == ownerId && key.Port == transition.Port) keys.push_back(key);
		for (size_t index = 1; index < keys.size(); ++index)
			if (CompareFrameTime(GetFrameTime(keys[index - 1]), GetFrameTime(keys[index])) >= 0)
				return fail(
					Status::UnsupportedExecution,
					"source animator order is not represented by the native sampler"
				);
		Value sampled;
		bool emptyGroupVector = false;
		detail::AllocationReservation sampledCharge;
		if (trigger) {
			// setAnim sets the mode before capturing; disabled Trigger getters return false.
			sampled = false;
		} else if (!transition.Animated || keys.empty()) {
			if (keys.empty()) {
				const bool configured = std::any_of(
					candidate.Tracks.begin(), candidate.Tracks.end(), [&](const AnimationTrack &track) {
						return track.NodeId == ownerId && track.Port == transition.Port;
					}
				);
				if (!ownerWasAnimated && !configured)
					return fail(
						Status::UnsupportedExecution,
						"source static fixed storage needs a represented original animator key"
					);
				const auto *ownerCatalogue = FindCatalogueEntry(owner->Type);
				size_t ownerGroup = 0;
				const auto *ownerInput =
					ownerCatalogue ? FindCatalogueInput(*ownerCatalogue, transition.Port) : nullptr;
				if (!ownerInput && ownerCatalogue)
					ownerInput = FindDynamicTemplate(*ownerCatalogue, transition.Port, ownerGroup);
				const auto classified =
					ownerInput ? ArrayClassification(*owner, transition.Port, *ownerInput, replay)
							   : std::nullopt;
				if (!classified)
					return fail(
						Status::UnsupportedExecution,
						"empty source animator typeArray classification is unresolved"
					);
				if (*classified) {
					ArrayValue empty{ValueType::Scalar, {}};
					const bool groupVector =
						(owner->Type == "pc.group_input" || owner->Type == "pc.group_output") &&
						ownerInput->Type == ValueType::Vector2 &&
						(ownerInput->SourceKind == "Range" || ownerInput->SourceKind == "Vec2");
					emptyGroupVector = groupVector;
					if (ownerInput->Type != ValueType::Array &&
						!(owner->Type == "pc.group_input" && transition.Port == "parent_value") &&
						!groupVector && !CatalogueAuthoredArray(*ownerCatalogue, *ownerInput, empty))
						return fail(
							Status::UnsupportedExecution,
							"empty source array animator has no native property carrier"
						);
					sampled = std::move(empty);
				} else
					sampled = 0.0;
			} else if (!ownerAnimated && std::holds_alternative<Quaternion>(keys.front().Data) &&
					   (keys.size() > 1 || !keys.front().SourceDriver ||
						CompareFrameTime(transition.Time, GetFrameTime(keys.front())) < 0)) {
				// The disabled animator returns its first raw quaternion before the typed getter.
				sampled = keys.front().Data;
			} else if (!ownerAnimated) {
				if (keys.size() == 1 && keys.front().SourceDriver &&
					CompareFrameTime(transition.Time, GetFrameTime(keys.front())) >= 0) {
					const auto &driver = *keys.front().SourceDriver;
					if (!std::holds_alternative<KeyframeLinearDriver>(driver) &&
						!std::holds_alternative<KeyframeSineDriver>(driver) &&
						!std::holds_alternative<KeyframeSnapDriver>(driver))
						return fail(
							Status::UnsupportedExecution, "source raw lone-driver capture is not represented"
						);
					if (const auto *array = std::get_if<ArrayValue>(&keys.front().Data);
						array && !array->Items.empty())
						return fail(
							Status::UnsupportedExecution,
							"source raw driver capture of recursive carriers is not represented"
						);
				}
				SourceAnimatorCapture capture;
				SourceAnimatorCaptureOptions options;
				options.Time = transition.Time;
				options.TotalFrames = candidate.Timeline ? candidate.Timeline->Frames : keys.back().Tick + 1;
				options.AvailableOwnedBytes = budget.Available();
				status = CaptureSourceDisabledAnimatorValue(keys, options, capture, diagnostic);
				if (status != Status::Ok) return status;
				auto charge = budget.Reserve(capture.ResidentOwnedBytes);
				if (!charge) return fail(Status::LimitExceeded, "source disabled capture exceeds budget");
				sampledCharge = std::move(*charge);
				sampled = std::move(capture.Data);
			} else {
				auto coneCharge = budget.Reserve(candidate.Nodes.size());
				if (!coneCharge)
					return fail(Status::LimitExceeded, "source sampler selection exceeds budget");
				std::vector<uint8_t> needed(candidate.Nodes.size(), 0);
				needed[size_t(owner - candidate.Nodes.begin())] = 1;
				EvaluationRequest clock;
				if (!SetFrameTime(clock, transition.Time))
					return fail(Status::InvalidValue, "source sampler clock is invalid");
				detail::TimelineOverrides values;
				status = detail::ResolveTimelineOverrides(
					candidate, needed, clock, budget, values, diagnostic, transition.Port, true
				);
				if (status != Status::Ok) return status;
				const Node &resolved = values.Find(size_t(owner - candidate.Nodes.begin()), *owner);
				const auto *raw = Authored(resolved, transition.Port);
				if (!raw) return fail(Status::UnsupportedExecution, "source animator raw sample is absent");
				const auto bytes = ValueClonePayloadBytes(*raw);
				auto charge = bytes ? budget.Reserve(*bytes) : std::nullopt;
				if (!charge)
					return fail(Status::LimitExceeded, "source animator sample clone exceeds budget");
				sampledCharge = std::move(*charge);
				sampled = Value(*raw);
			}
		}
		std::vector<Keyframe> replacement;
		uint64_t replacementBytes = 0;
		const bool fresh = !transition.Animated || keys.empty();
		if (fresh) {
			replacementBytes = sizeof(Keyframe) + TextBytes(ownerId) + TextBytes(transition.Port) +
							   TextBytes("source") + 2 * TextBytes("linear");
		} else
			replacementBytes = sourceBytes;
		auto replacementCharge = budget.Reserve(replacementBytes);
		if (!replacementCharge)
			return fail(Status::LimitExceeded, "source animator replacement exceeds budget");
		replacement.reserve(fresh ? 1 : keys.size());
		if (fresh) {
			Keyframe key;
			key.NodeId = std::string(ownerId);
			key.Port = std::string(transition.Port);
			key.Interpolation = "source";
			key.Ease = KeyframeEase{};
			key.Data = std::move(sampled);
			replacement.push_back(std::move(key));
		} else
			for (const auto &key : keys)
				replacement.push_back(key);
		if (transition.Animated) {
			if (replacement.size() > 1 &&
				CompareFrameTime(transition.Time, GetFrameTime(replacement[1])) >= 0)
				return fail(
					Status::UnsupportedExecution,
					"retimed source first key would require unrepresented key order"
				);
			if (!SetFrameTime(replacement.front(), transition.Time))
				return fail(Status::InvalidValue, "source first key time is invalid");
		}
		if (emptyGroupVector)
			for (auto &value : owner->Values)
				if (value.Port == transition.Port) value.Data = ArrayValue{ValueType::Scalar, {}};
		const size_t finalCount = candidate.Keyframes.size() - count + replacement.size();
		if (finalCount > Limits::MaximumKeyframes)
			return fail(Status::LimitExceeded, "source mode replacement key count exceeds bounds");
		uint64_t finalBytes = finalCount * sizeof(Keyframe);
		for (const auto &key : candidate.Keyframes)
			if (key.NodeId != ownerId || key.Port != transition.Port) {
				const auto bytes = KeyframePayloadBytes(key);
				if (!bytes || !Add(finalBytes, *bytes - sizeof(Keyframe)))
					return fail(Status::LimitExceeded, "source mode retained keys exceed bounds");
			}
		auto finalCharge = budget.Reserve(finalBytes);
		if (!finalCharge) return fail(Status::LimitExceeded, "source mode final key vector exceeds budget");
		std::vector<Keyframe> finalKeys;
		finalKeys.reserve(finalCount);
		for (const auto &key : candidate.Keyframes)
			if (key.NodeId != ownerId || key.Port != transition.Port) finalKeys.push_back(key);
		for (auto &key : replacement)
			finalKeys.push_back(std::move(key));
		if (std::none_of(candidate.Tracks.begin(), candidate.Tracks.end(), [&](const AnimationTrack &track) {
				return track.NodeId == ownerId && track.Port == transition.Port;
			})) {
			if (candidate.Tracks.size() == Limits::MaximumTracks)
				return fail(Status::LimitExceeded, "source mode track count exceeds bounds");
			uint64_t trackBytes = TextBytes(ownerId) + TextBytes(transition.Port) + TextBytes("hold");
			if (candidate.Tracks.size() == candidate.Tracks.capacity() &&
				!Add(trackBytes, (candidate.Tracks.size() + 1) * sizeof(AnimationTrack)))
				return fail(Status::LimitExceeded, "source mode track allocation overflows");
			auto trackCharge = budget.Reserve(trackBytes);
			if (!trackCharge) return fail(Status::LimitExceeded, "source mode track exceeds budget");
			if (candidate.Tracks.size() == candidate.Tracks.capacity())
				candidate.Tracks.reserve(candidate.Tracks.size() + 1);
			candidate.Tracks.push_back({std::string(ownerId), std::string(transition.Port), "hold", -1});
			// This admission lives until the transaction ends, alongside the candidate.
			if (!held->Merge(std::move(*trackCharge))) std::terminate();
		}
		candidate.Keyframes.swap(finalKeys);
		candidate.FormatVersion = 9;
		result = std::move(candidate);
		diagnostic = {};
		return Status::Ok;
	} catch (const std::bad_alloc &) {
		diagnostic = {Status::LimitExceeded, {}, {}, "source mode transaction allocation failed"};
		return diagnostic.Code;
	}
}
