#include "EvaluationAllocator.hpp"
#include "GroupReplayInternal.hpp"
#include "SourceAnimatorIdentity.hpp"
#include "SourceAxisStorage.hpp"
#include "SourceSeparatedVec2.hpp"
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
		bool AddTransitionBytes(uint64_t &bytes, uint64_t extra) {
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
	static Status ToggleSourceInputModeImpl(
		const Document &document,
		const GroupReplayState &replay,
		uint64_t authoringRevision,
		const SourceModeTransition &transition,
		Document &result,
		GroupReplayState *replayResult,
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
		const uint64_t priorReplayBytes =
			replayResult && replayResult != &replay ? replayResult->RetainedBytes() : 0;
		uint64_t borrowed = oldBytes.value_or(0);
		if (!oldBytes || !priorBytes || !AddTransitionBytes(borrowed, *priorBytes) ||
			!AddTransitionBytes(borrowed, replay.RetainedBytes()) ||
			!AddTransitionBytes(borrowed, priorReplayBytes))
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
		if (!candidateBytes || !AddTransitionBytes(live, *candidateBytes) || !held->Resize(live))
			return fail(Status::LimitExceeded, "source mode live document overlap exceeds bounds");
		if (wasAnimated == transition.Animated) {
			if (replayResult) {
				auto next = detail::CloneGroupReplay(
					replay,
					0,
					authoringRevision,
					budget.Available() + replay.RetainedBytes() + priorReplayBytes,
					priorReplayBytes,
					diagnostic
				);
				if (!next) return diagnostic.Code;
				detail::GroupReplayAccess::Install(*replayResult, std::move(next));
			}
			result = std::move(candidate);
			diagnostic = {};
			return Status::Ok;
		}
		auto target = std::find_if(candidate.Nodes.begin(), candidate.Nodes.end(), [&](const Node &node) {
			return node.Id == transition.NodeId;
		});
		std::string_view ownerId = target->Id;
		std::string_view animatorPort = transition.Port;
		if (const auto *binding = replay.Binding(transition.NodeId, transition.Port)) {
			ownerId = binding->OwnerId;
			animatorPort = detail::BindingAnimatorPort(*binding);
		} else if (!detail::SourceInputInstanceBase(*target, transition.Port).empty())
			return fail(Status::InvalidValue, "source instance mode requires its original animator binding");
		auto owner = std::find_if(candidate.Nodes.begin(), candidate.Nodes.end(), [&](const Node &node) {
			return node.Id == ownerId;
		});
		if (owner == candidate.Nodes.end())
			return fail(Status::UnknownNode, "source animator owner is absent");
		const auto *detached = replay.DetachedAnimator(ownerId, animatorPort);
		if (detached && !replayResult)
			return fail(
				Status::UnsupportedExecution, "retained source animator mode requires a paired replay result"
			);
		bool ownerAnimated = detached && detached->Writer == GroupSubtypeAnimator::Animated;
		if (!detached && !Mode(*owner, animatorPort, ownerAnimated))
			return fail(Status::InvalidValue, "source animator owner needs explicit provenance");
		const bool ownerWasAnimated = ownerAnimated;
		uint64_t axisWork = 0;
		const auto sourceAxes = detail::ResolveLocalSourceAxes(
			candidate,
			*target,
			transition.Port,
			replay.InstancesBound() ? replay.Bindings() : std::span<const GroupSubtypeBinding>{},
			replay.SharedSubtypes(),
			replay.DetachedAnimators(),
			ownerId,
			animatorPort,
			ownerAnimated,
			axisWork
		);
		if (sourceAxes.Code != Status::Ok) return fail(sourceAxes.Code, std::string(sourceAxes.Message));
		auto &removedModes = transition.Animated ? target->SourceStaticInputs : target->SourceAnimatedInputs;
		auto &addedModes = transition.Animated ? target->SourceAnimatedInputs : target->SourceStaticInputs;
		if (addedModes.size() == Limits::MaximumArrayElements)
			return fail(Status::LimitExceeded, "source mode declaration count exceeds bounds");
		uint64_t modeBytes = TextBytes(transition.Port);
		if (addedModes.size() == addedModes.capacity() &&
			!AddTransitionBytes(modeBytes, (addedModes.size() + 1) * sizeof(std::string)))
			return fail(Status::LimitExceeded, "source mode declaration allocation overflows");
		auto modeCharge = budget.Reserve(modeBytes);
		if (!modeCharge) return fail(Status::LimitExceeded, "source mode declaration exceeds budget");
		if (addedModes.size() == addedModes.capacity()) addedModes.reserve(addedModes.size() + 1);
		addedModes.push_back(std::string(transition.Port));
		std::erase(removedModes, transition.Port);
		const bool combinedWriterChanges = !detached && owner == target && animatorPort == transition.Port;
		if (combinedWriterChanges) ownerAnimated = transition.Animated;
		const auto refreshGetterMode = [&](GroupSubtypeBinding &binding) {
			if ((binding.Axes.Storage == GroupAxisStorage::Local ||
				 binding.Axes.Storage == GroupAxisStorage::Shared) &&
				binding.Axes.OwnerId == target->Id && binding.Axes.Port == transition.Port)
				binding.Axes.Writer =
					transition.Animated ? GroupSubtypeAnimator::Animated : GroupSubtypeAnimator::Static;
			const auto selected =
				std::find_if(candidate.Nodes.begin(), candidate.Nodes.end(), [&](const auto &node) {
					return node.Id == binding.NodeId;
				});
			if (selected == candidate.Nodes.end()) return;
			if (const auto mode =
					detail::SourcePropertyGetterAnimated(candidate, *selected, binding.Port, nullptr))
				binding.Getter = *mode ? GroupSubtypeAnimator::Animated : GroupSubtypeAnimator::Static;
		};

		if (candidate.SourceAnimators)
			for (auto &binding : candidate.SourceAnimators->Bindings) {
				refreshGetterMode(binding);
				if (combinedWriterChanges && binding.OwnerId == ownerId &&
					detail::BindingAnimatorPort(binding) == animatorPort)
					binding.Writer =
						transition.Animated ? GroupSubtypeAnimator::Animated : GroupSubtypeAnimator::Static;
			}

		if (sourceAxes.Separated) {
			if (sourceAxes.Detached)
				return fail(
					Status::UnsupportedExecution, "retained scalar axis mode transition is not represented"
				);
			const auto *separated = sourceAxes.Axes;
			const auto axisOwner =
				std::find_if(candidate.Nodes.begin(), candidate.Nodes.end(), [&](const auto &node) {
					return node.Id == sourceAxes.Owner->Id;
				});
			const auto axisOwnerId = std::string_view(axisOwner->Id);
			const auto axisPort = sourceAxes.Port;
			const bool axisWriterChanges = axisOwner == target && axisPort == transition.Port;
			const bool axisAnimated = axisWriterChanges ? transition.Animated : sourceAxes.WriterAnimated;
			size_t aggregate = candidate.Keyframes.size();
			for (const auto &node : candidate.Nodes)
				if (node.SourceSeparatedVec2Animators)
					for (const auto &input : node.SourceSeparatedVec2Animators->Inputs)
						for (const auto &axis : input.Axes)
							aggregate += axis.Keys.size();
			uint64_t required = sizeof(SourceSeparatedVec2Animator) + TextBytes(axisPort);
			for (const auto &axis : separated->Axes) {
				if (transition.Animated) {
					if (axis.Keys.empty())
						return fail(
							Status::UnsupportedExecution, "source split enable has no first key to retime"
						);
					if (aggregate == Limits::MaximumKeyframes)
						return fail(Status::LimitExceeded, "source split mode keys exceed aggregate bound");
					++aggregate;
					const auto bytes = detail::SeparatedScalarBytes(axis, false);
					if (!bytes || !AddTransitionBytes(required, *bytes))
						return fail(Status::LimitExceeded, "source split mode snapshot exceeds bounds");
				} else
					aggregate = aggregate - axis.Keys.size() + 1;
				if (!AddTransitionBytes(
						required,
						sizeof(Keyframe) + TextBytes(axisOwnerId) + TextBytes(axisPort) +
							TextBytes("source") + 2 * TextBytes("linear")
					))
					return fail(Status::LimitExceeded, "source split mode key allocation overflows");
			}
			if (aggregate > Limits::MaximumKeyframes)
				return fail(Status::LimitExceeded, "source split mode keys exceed aggregate bound");
			auto keysCharge = budget.Reserve(required);
			if (!keysCharge)
				return fail(Status::LimitExceeded, "source split mode replacement exceeds live budget");
			EvaluationRequest at;
			if (!SetFrameTime(at, transition.Time))
				return fail(Status::InvalidValue, "source split mode clock is invalid");

			std::array<double, 2> samples{};
			for (size_t index = 0; index < 2; ++index) {
				status = detail::SampleSeparatedScalar(
					separated->Axes[index],
					sourceAxes.Track,
					candidate.Timeline ? &*candidate.Timeline : nullptr,
					at,
					true,
					axisAnimated,
					budget,
					samples[index],
					diagnostic
				);
				if (status != Status::Ok) return status;
			}
			SourceSeparatedVec2Animator replacement;
			replacement.Port = std::string(axisPort);
			replacement.Separated = separated->Separated;
			for (size_t index = 0; index < 2; ++index) {
				auto &keys = replacement.Axes[index].Keys;
				const auto &old = separated->Axes[index].Keys;
				keys.reserve(transition.Animated ? old.size() + 1 : 1);
				if (transition.Animated)
					for (const auto &key : old)
						keys.push_back(key);
				Keyframe added{
					std::string(axisOwnerId),
					std::string(axisPort),
					0,
					samples[index],
					"source",
					KeyframeEase{}
				};
				if (transition.Animated && !SetFrameTime(added, transition.Time))
					return fail(Status::InvalidValue, "source split append clock is invalid");
				keys.push_back(std::move(added));
				if (transition.Animated && !SetFrameTime(keys.front(), transition.Time))
					return fail(Status::InvalidValue, "source split retime clock is invalid");
			}
			for (auto &input : axisOwner->SourceSeparatedVec2Animators->Inputs)
				if (input.Port == axisPort) {
					input = std::move(replacement);
					break;
				}
			candidate.FormatVersion = std::max(candidate.FormatVersion, uint32_t{9});
			if (!DocumentRetainedPayloadBytes(candidate))
				return fail(Status::LimitExceeded, "source split mode retained candidate exceeds bounds");
			if (replayResult) {
				auto next = detail::CloneGroupReplay(
					replay,
					0,
					authoringRevision,
					budget.Available() + replay.RetainedBytes() + priorReplayBytes,
					priorReplayBytes,
					diagnostic
				);
				if (!next) return diagnostic.Code;
				uint64_t retired = 0;
				for (auto it = next->SharedSubtypes.begin(); it != next->SharedSubtypes.end();) {
					if (it->NodeId != axisOwnerId || it->Port != axisPort) {
						++it;
						continue;
					}
					retired += TextBytes(it->NodeId) + TextBytes(it->Port) +
							   it->Keys.capacity() * sizeof(Keyframe) + detail::SeparatedOverlayBytes(*it);
					if (it->Fixed) retired += detail::RetainedPayloadBytes(*it->Fixed);
					for (const auto &key : it->Keys)
						retired += *KeyframePayloadBytes(key) - sizeof(Keyframe);
					it = next->SharedSubtypes.erase(it);
				}
				if (!next->Charge.Resize(next->Charge.Bytes() - retired)) std::terminate();
				for (auto &binding : next->Bindings) {
					refreshGetterMode(binding);
					if (combinedWriterChanges && binding.OwnerId == ownerId &&
						detail::BindingAnimatorPort(binding) == animatorPort)
						binding.Writer = transition.Animated ? GroupSubtypeAnimator::Animated
															 : GroupSubtypeAnimator::Static;
					if (axisWriterChanges && detail::BindingReferencesAxes(binding, axisOwnerId, axisPort))
						binding.Axes.Writer = transition.Animated ? GroupSubtypeAnimator::Animated
																  : GroupSubtypeAnimator::Static;
				}
				detail::GroupReplayAccess::Install(*replayResult, std::move(next));
			}
			result = std::move(candidate);
			diagnostic = {};
			return Status::Ok;
		}

		const auto *detachedEffect = detached ? replay.SharedSubtype(ownerId, animatorPort) : nullptr;
		if (detached && !detachedEffect)
			return fail(Status::InvalidValue, "retained source animator effect is absent");
		const auto &originalKeys = detachedEffect ? detachedEffect->Keys : candidate.Keyframes;
		size_t count = 0;
		uint64_t sourceBytes = 0;
		for (const auto &key : originalKeys)
			if (key.NodeId == ownerId && key.Port == animatorPort) {
				if (key.Interpolation != "source")
					return fail(
						Status::UnsupportedExecution, "source mode transition requires source animator keys"
					);
				const auto bytes = KeyframePayloadBytes(key);
				if (!bytes || !AddTransitionBytes(sourceBytes, *bytes))
					return fail(Status::LimitExceeded, "source animator key payload exceeds bounds");
				++count;
			}
		auto sourceCharge = budget.Reserve(sourceBytes);
		if (!sourceCharge) return fail(Status::LimitExceeded, "source animator snapshot exceeds budget");
		std::vector<Keyframe> keys;
		keys.reserve(count);
		for (const auto &key : originalKeys)
			if (key.NodeId == ownerId && key.Port == animatorPort) keys.push_back(key);
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
				const bool configured =
					(detached && detached->Track) ||
					std::any_of(
						candidate.Tracks.begin(), candidate.Tracks.end(), [&](const AnimationTrack &track) {
							return track.NodeId == ownerId && track.Port == animatorPort;
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
					ownerCatalogue ? FindCatalogueInput(*ownerCatalogue, animatorPort) : nullptr;
				if (!ownerInput && ownerCatalogue)
					ownerInput = FindDynamicTemplate(*ownerCatalogue, animatorPort, ownerGroup);
				const auto classified = detached ? detached->ArrayClassification
										: ownerInput
											? ArrayClassification(*owner, animatorPort, *ownerInput, replay)
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
					if (!detached && ownerInput->Type != ValueType::Array &&
						!(owner->Type == "pc.group_input" && animatorPort == "parent_value") &&
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
				if (detached) clock.GroupReplay = &replay;
				if (!SetFrameTime(clock, transition.Time))
					return fail(Status::InvalidValue, "source sampler clock is invalid");
				detail::TimelineOverrides values;
				status = detail::ResolveTimelineOverrides(
					candidate,
					needed,
					clock,
					budget,
					values,
					diagnostic,
					animatorPort,
					true,
					{},
					{},
					nullptr,
					true
				);
				if (status != Status::Ok) return status;
				const Node &resolved = values.Find(size_t(owner - candidate.Nodes.begin()), *owner);
				const auto *raw = Authored(resolved, animatorPort);
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
			replacementBytes = sizeof(Keyframe) + TextBytes(ownerId) + TextBytes(animatorPort) +
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
			key.Port = std::string(animatorPort);
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
		if (detached) {
			auto next = detail::CloneGroupReplay(
				replay,
				0,
				authoringRevision,
				budget.Available() + replay.RetainedBytes() + priorReplayBytes,
				priorReplayBytes,
				diagnostic
			);
			if (!next) return diagnostic.Code;
			auto effect =
				std::find_if(next->SharedSubtypes.begin(), next->SharedSubtypes.end(), [&](const auto &item) {
					return item.NodeId == ownerId && item.Port == animatorPort;
				});
			uint64_t oldPayload = effect->Keys.capacity() * sizeof(Keyframe);
			for (const auto &key : effect->Keys)
				oldPayload += *KeyframePayloadBytes(key) - sizeof(Keyframe);
			if (effect->Fixed) oldPayload += detail::RetainedPayloadBytes(*effect->Fixed);
			uint64_t newPayload = replacement.capacity() * sizeof(Keyframe);
			for (const auto &key : replacement)
				newPayload += *KeyframePayloadBytes(key) - sizeof(Keyframe);
			auto replayKeys = next->Budget.Reserve(newPayload);
			if (!replayKeys)
				return fail(Status::LimitExceeded, "retained source animator mode keys exceed budget");
			{
				auto old = std::move(effect->Keys);
				effect->Keys = std::move(replacement);
				effect->Fixed.reset();
			}
			if (!next->Charge.Merge(std::move(*replayKeys)) ||
				!next->Charge.Resize(next->Charge.Bytes() - oldPayload))
				std::terminate();
			for (auto &binding : next->Bindings)
				refreshGetterMode(binding);
			GroupReplayState replayCandidate;
			detail::GroupReplayAccess::Install(replayCandidate, std::move(next));
			auto stateCharge = budget.Reserve(replayCandidate.RetainedBytes());
			if (!stateCharge)
				return fail(Status::LimitExceeded, "retained source mode candidate exceeds budget");
			Document projected;
			status = ProjectGroupReplay(
				candidate,
				replayCandidate,
				authoringRevision,
				projected,
				diagnostic,
				budget.Available() + *candidateBytes + replayCandidate.RetainedBytes()
			);
			if (status != Status::Ok) return status;
			result = std::move(projected);
			*replayResult = std::move(replayCandidate);
			diagnostic = {};
			return Status::Ok;
		}
		if (emptyGroupVector)
			for (auto &value : owner->Values)
				if (value.Port == animatorPort) value.Data = ArrayValue{ValueType::Scalar, {}};
		const size_t finalCount = candidate.Keyframes.size() - count + replacement.size();
		if (finalCount > Limits::MaximumKeyframes)
			return fail(Status::LimitExceeded, "source mode replacement key count exceeds bounds");
		uint64_t finalBytes = finalCount * sizeof(Keyframe);
		for (const auto &key : candidate.Keyframes)
			if (key.NodeId != ownerId || key.Port != animatorPort) {
				const auto bytes = KeyframePayloadBytes(key);
				if (!bytes || !AddTransitionBytes(finalBytes, *bytes - sizeof(Keyframe)))
					return fail(Status::LimitExceeded, "source mode retained keys exceed bounds");
			}
		auto finalCharge = budget.Reserve(finalBytes);
		if (!finalCharge) return fail(Status::LimitExceeded, "source mode final key vector exceeds budget");
		std::vector<Keyframe> finalKeys;
		finalKeys.reserve(finalCount);
		for (const auto &key : candidate.Keyframes)
			if (key.NodeId != ownerId || key.Port != animatorPort) finalKeys.push_back(key);
		for (auto &key : replacement)
			finalKeys.push_back(std::move(key));
		if (std::none_of(candidate.Tracks.begin(), candidate.Tracks.end(), [&](const AnimationTrack &track) {
				return track.NodeId == ownerId && track.Port == animatorPort;
			})) {
			if (candidate.Tracks.size() == Limits::MaximumTracks)
				return fail(Status::LimitExceeded, "source mode track count exceeds bounds");
			uint64_t trackBytes = TextBytes(ownerId) + TextBytes(animatorPort) + TextBytes("hold");
			if (candidate.Tracks.size() == candidate.Tracks.capacity() &&
				!AddTransitionBytes(trackBytes, (candidate.Tracks.size() + 1) * sizeof(AnimationTrack)))
				return fail(Status::LimitExceeded, "source mode track allocation overflows");
			auto trackCharge = budget.Reserve(trackBytes);
			if (!trackCharge) return fail(Status::LimitExceeded, "source mode track exceeds budget");
			if (candidate.Tracks.size() == candidate.Tracks.capacity())
				candidate.Tracks.reserve(candidate.Tracks.size() + 1);
			candidate.Tracks.push_back({std::string(ownerId), std::string(animatorPort), "hold", -1});
			// This admission lives until the transaction ends, alongside the candidate.
			if (!held->Merge(std::move(*trackCharge))) std::terminate();
		}
		candidate.Keyframes.swap(finalKeys);
		candidate.FormatVersion = std::max(candidate.FormatVersion, uint32_t{9});
		if (replayResult) {
			auto next = detail::CloneGroupReplay(
				replay,
				0,
				authoringRevision,
				budget.Available() + replay.RetainedBytes() + priorReplayBytes,
				priorReplayBytes,
				diagnostic
			);
			if (!next) return diagnostic.Code;
			uint64_t retiredBytes = 0;
			for (auto it = next->SharedSubtypes.begin(); it != next->SharedSubtypes.end();) {
				if (it->NodeId != ownerId || it->Port != animatorPort) {
					++it;
					continue;
				}
				retiredBytes +=
					TextBytes(it->NodeId) + TextBytes(it->Port) + it->Keys.capacity() * sizeof(Keyframe);
				if (it->Fixed) retiredBytes += detail::RetainedPayloadBytes(*it->Fixed);
				retiredBytes += detail::SeparatedOverlayBytes(*it);
				for (const auto &key : it->Keys)
					retiredBytes += *KeyframePayloadBytes(key) - sizeof(Keyframe);
				it = next->SharedSubtypes.erase(it);
			}
			if (!next->Charge.Resize(next->Charge.Bytes() - retiredBytes)) std::terminate();
			for (auto &binding : next->Bindings) {
				refreshGetterMode(binding);
				if (combinedWriterChanges && binding.OwnerId == ownerId &&
					detail::BindingAnimatorPort(binding) == animatorPort)
					binding.Writer =
						transition.Animated ? GroupSubtypeAnimator::Animated : GroupSubtypeAnimator::Static;
			}
			detail::GroupReplayAccess::Install(*replayResult, std::move(next));
		}
		result = std::move(candidate);
		diagnostic = {};
		return Status::Ok;
	} catch (const std::bad_alloc &) {
		diagnostic = {Status::LimitExceeded, {}, {}, "source mode transaction allocation failed"};
		return diagnostic.Code;
	}
	Status ToggleSourceInputMode(
		const Document &document,
		const GroupReplayState &replay,
		uint64_t revision,
		const SourceModeTransition &transition,
		Document &result,
		Diagnostic &diagnostic,
		uint64_t maximumBytes
	) {
		return ToggleSourceInputModeImpl(
			document, replay, revision, transition, result, nullptr, diagnostic, maximumBytes
		);
	}
	Status ToggleSourceInputMode(
		const Document &document,
		const GroupReplayState &replay,
		uint64_t revision,
		const SourceModeTransition &transition,
		Document &result,
		GroupReplayState &replayResult,
		Diagnostic &diagnostic,
		uint64_t maximumBytes
	) {
		return ToggleSourceInputModeImpl(
			document, replay, revision, transition, result, &replayResult, diagnostic, maximumBytes
		);
	}
}
