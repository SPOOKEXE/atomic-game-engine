#include "GroupReplayInternal.hpp"
#include "SourceAxisStorage.hpp"
#include "ValuePayload.hpp"

#include <algorithm>
#include <array>
#include <charconv>

namespace engine::imagegraph {
	namespace {
		uint64_t TextBytes(std::string_view text) {
			return std::max(text.size(), std::string{}.capacity()) + 1;
		}
		struct RecaptureEdit {
			detail::GroupReplayAccess::Owner &Owner;
			Diagnostic &Error;
			uint64_t Work = 0;
			uint64_t ComparisonBytes = 1;

			bool Admit(uint64_t visits) {
				if (visits <= (64'000'000 - Work) / ComparisonBytes &&
					detail::AdmitSourceAxisWork(Work, visits * ComparisonBytes))
					return true;
				Error = {Status::LimitExceeded, {}, {}, "instance recapture exceeds work bounds"};
				return false;
			}
			bool Rename(std::string &text, std::string_view replacement) {
				auto charge = Owner.Budget.Reserve(TextBytes(replacement));
				if (!charge) return Refuse();
				const auto released = TextBytes(text);
				std::string fresh(replacement);
				text.swap(fresh);
				if (!Owner.Charge.Merge(std::move(*charge)) ||
					!Owner.Charge.Resize(Owner.Charge.Bytes() - released))
					std::terminate();
				return true;
			}
			bool Refuse() {
				Error = {Status::LimitExceeded, {}, {}, "instance recapture payload exceeds overlap bounds"};
				return false;
			}
			// retain only the scalar generation; combined aliases keep their own writer identity.
			bool RetainLocalAxes(const Document &document, const Node &node, std::string_view port) {
				if (!Admit(
						Owner.SharedSubtypes.size() + Owner.Bindings.size() +
						(node.SourceSeparatedVec2Animators ? node.SourceSeparatedVec2Animators->Inputs.size()
														   : 0)
					))
					return false;
				auto local = std::find_if(
					Owner.SharedSubtypes.begin(), Owner.SharedSubtypes.end(), [&](const auto &item) {
						return item.NodeId == node.Id && item.Port == port;
					}
				);
				const bool added = local == Owner.SharedSubtypes.end();
				const auto *axes = !added && local->SeparatedVec2 ? &*local->SeparatedVec2
																  : detail::FindSeparatedVec2(node, port);
				if (!axes) return true;
				if (!Admit(axes->Axes[0].Keys.size() + axes->Axes[1].Keys.size())) return false;
				const auto payload = detail::SeparatedAnimatorBytes(*axes, true);
				if (!payload) return Refuse();
				const bool referenced =
					axes->Initialized &&
					std::any_of(Owner.Bindings.begin(), Owner.Bindings.end(), [&](const auto &binding) {
						return binding.NodeId != node.Id && binding.Axes.Storage != GroupAxisStorage::None &&
							   binding.Axes.Storage != GroupAxisStorage::Uninitialized &&
							   binding.Axes.OwnerId == node.Id && binding.Axes.Port == port;
					});
				if (referenced) {
					if (Owner.NextAnimatorId == UINT64_MAX) return Refuse();
					std::array<char, 64> identity{};
					constexpr std::string_view prefix = "native:animator:";
					std::copy(prefix.begin(), prefix.end(), identity.begin());
					const auto digits = std::to_chars(
						identity.data() + prefix.size(),
						identity.data() + identity.size(),
						Owner.NextAnimatorId
					);
					if (digits.ec != std::errc{}) return Refuse();
					const std::string_view id(identity.data(), size_t(digits.ptr - identity.data()));
					if (!Admit(document.Tracks.size())) return false;
					const auto track =
						std::find_if(document.Tracks.begin(), document.Tracks.end(), [&](const auto &item) {
							return item.NodeId == node.Id && item.Port == port;
						});
					uint64_t bytes = TextBytes(id) + TextBytes(node.Id) + TextBytes(port) +
									 TextBytes(node.Id) + TextBytes(id) + *payload;
					if (track != document.Tracks.end())
						bytes += TextBytes(track->NodeId) + TextBytes(id) + TextBytes(track->End);
					auto charge = Owner.Budget.Reserve(bytes);
					if (!charge) return Refuse();
					DetachedSourceAnimator metadata;
					metadata.Id = std::string(id);
					metadata.OwnerId = std::string(node.Id);
					metadata.OriginalPort = std::string(port);
					metadata.Type = ValueType::Vector2;
					const auto reader =
						std::find_if(Owner.Bindings.begin(), Owner.Bindings.end(), [&](const auto &binding) {
							return binding.NodeId != node.Id &&
								   binding.Axes.Storage != GroupAxisStorage::None &&
								   binding.Axes.Storage != GroupAxisStorage::Uninitialized &&
								   binding.Axes.OwnerId == node.Id && binding.Axes.Port == port;
						});
					metadata.Writer = reader->Axes.Writer;
					if (track != document.Tracks.end()) {
						metadata.Track = AnimationTrack{
							track->NodeId,
							std::string(id),
							track->End,
							track->LoopRange,
							track->QuaternionMode
						};
					}
					GroupSubtypeOverlay retained;
					retained.NodeId = std::string(node.Id);
					retained.Port = std::string(id);
					retained.SeparatedVec2.emplace() = *axes;
					Owner.DetachedAnimators.push_back(std::move(metadata));
					Owner.SharedSubtypes.push_back(std::move(retained));
					++Owner.NextAnimatorId;
					if (!Owner.Charge.Merge(std::move(*charge))) std::terminate();
					auto &stored = Owner.SharedSubtypes.back();
					if (!Rename(stored.SeparatedVec2->Port, id)) return false;
					for (auto &axis : stored.SeparatedVec2->Axes)
						for (auto &key : axis.Keys)
							if (!Rename(key.Port, id)) return false;
					for (auto &binding : Owner.Bindings)
						if (binding.NodeId != node.Id && binding.Axes.Storage != GroupAxisStorage::None &&
							binding.Axes.Storage != GroupAxisStorage::Uninitialized &&
							binding.Axes.OwnerId == node.Id && binding.Axes.Port == port)
							if (!Rename(binding.Axes.Port, id)) return false;
				}
				// a cold carrier hides authored old rows until the next constructor read.
				const bool separated = axes->Separated;
				uint64_t bytes = sizeof(SourceSeparatedVec2Animator) + TextBytes(port);
				if (added) bytes += TextBytes(node.Id) + TextBytes(port);
				auto charge = Owner.Budget.Reserve(bytes);
				if (!charge) return Refuse();
				const uint64_t released =
					!added && local->SeparatedVec2 ? detail::SeparatedOverlayBytes(*local) : 0;
				if (added) {
					GroupSubtypeOverlay fresh;
					fresh.NodeId = std::string(node.Id);
					fresh.Port = std::string(port);
					Owner.SharedSubtypes.push_back(std::move(fresh));
					local = Owner.SharedSubtypes.end() - 1;
				}
				SourceSeparatedVec2Animator cold;
				cold.Port = std::string(port);
				cold.Separated = separated;
				cold.Initialized = false;
				local->SeparatedVec2.emplace() = std::move(cold);
				if (!Owner.Charge.Merge(std::move(*charge)) ||
					!Owner.Charge.Resize(Owner.Charge.Bytes() - released))
					std::terminate();
				return true;
			}
		};
	}

	Status RecaptureGroupInstances(
		const Document &document,
		std::span<const GroupInstanceRecapture> targets,
		const GroupReplayState &previous,
		uint64_t revision,
		GroupReplayState &result,
		Diagnostic &diagnostic,
		uint64_t maximumBytes
	) try {
		ENGINE_PROFILE("imagegraph.group_instance_recapture");
		const auto fail = [&](Status status,
							  std::string_view message,
							  std::string_view node = {},
							  std::string_view port = {}) {
			diagnostic = {status, std::string(node), std::string(port), std::string(message)};
			return status;
		};
		if (!maximumBytes || maximumBytes > Limits::MaximumEvaluationBytes ||
			targets.size() > Limits::MaximumArrayElements || document.Nodes.size() > Limits::MaximumNodes)
			return fail(Status::LimitExceeded, "instance recapture cap or counts exceed bounds");
		if (!previous.InstancesBound() || previous.AuthoringRevision() != revision)
			return fail(Status::InvalidValue, "instance recapture requires bound owners at this revision");
		// weight comparisons by the longest name before repeated lookup or mutation.
		// fresh native identities have at most 20 decimal digits after this prefix.
		uint64_t comparisonBytes = std::string_view("native:animator:").size() + 20 + 1;
		const auto name = [&](std::string_view text) {
			if (text.size() > Limits::MaximumTextBytes) return false;
			comparisonBytes = std::max(comparisonBytes, uint64_t(text.size()) + 1);
			return true;
		};
		if (document.Groups.size() > Limits::MaximumGroups ||
			document.Junctions.size() > Limits::MaximumJunctions ||
			document.Links.size() > Limits::MaximumLinks ||
			document.Keyframes.size() > Limits::MaximumKeyframes ||
			document.Tracks.size() > Limits::MaximumTracks ||
			document.Outputs.size() > Limits::MaximumOutputs)
			return fail(Status::LimitExceeded, "instance recapture document counts exceed bounds");
		for (const auto &node : document.Nodes) {
			if (!name(node.Id) || !name(node.InstanceBase) || !name(node.SourceParentInputBase) ||
				node.DynamicInputs.size() > MaximumDynamicInputsForNode(node) ||
				node.Values.size() > Limits::MaximumArrayElements ||
				node.SourceProperties.size() > Limits::MaximumPropertiesPerNode ||
				node.InstanceOverrides.size() > Limits::MaximumArrayElements ||
				node.SourceAnimatedInputs.size() > Limits::MaximumArrayElements ||
				node.SourceStaticInputs.size() > Limits::MaximumArrayElements ||
				(node.SourceSeparatedVec2Animators &&
				 node.SourceSeparatedVec2Animators->Inputs.size() > Limits::MaximumArrayElements))
				return fail(Status::LimitExceeded, "instance recapture node records exceed bounds");
			for (const auto &input : node.DynamicInputs)
				if (!name(input.Id))
					return fail(Status::LimitExceeded, "instance recapture input names exceed bounds");
			for (const auto *ports : {&node.SourceAnimatedInputs, &node.SourceStaticInputs})
				for (const auto &port : *ports)
					if (!name(port))
						return fail(Status::LimitExceeded, "instance recapture mode names exceed bounds");
			if (node.SourceSeparatedVec2Animators)
				for (const auto &axes : node.SourceSeparatedVec2Animators->Inputs)
					if (!name(axes.Port))
						return fail(Status::LimitExceeded, "instance recapture axis names exceed bounds");
		}
		for (const auto &binding : previous.Bindings())
			for (const auto text : std::array<std::string_view, 7>{
					 binding.NodeId,
					 binding.OwnerId,
					 binding.Port,
					 binding.AnimatorPort,
					 binding.Axes.OwnerId,
					 binding.Axes.Port,
					 binding.Axes.InstanceBase
				 })
				if (!name(text))
					return fail(Status::LimitExceeded, "instance recapture binding names exceed bounds");
		for (const auto &overlay : previous.SharedSubtypes())
			if (!name(overlay.NodeId) || !name(overlay.Port))
				return fail(Status::LimitExceeded, "instance recapture overlay names exceed bounds");
		for (const auto &target : targets)
			if (!name(target.NodeId) || !name(target.Port))
				return fail(Status::LimitExceeded, "instance recapture target names exceed bounds");
		const auto documentBytes = DocumentRetainedPayloadBytes(document);
		const uint64_t oldBytes =
			previous.RetainedBytes() + (&previous == &result ? 0 : result.RetainedBytes());
		const uint64_t scratchBytes = targets.size() * sizeof(uint8_t);
		if (!documentBytes || *documentBytes > maximumBytes || oldBytes > maximumBytes - *documentBytes ||
			scratchBytes > maximumBytes - *documentBytes - oldBytes)
			return fail(Status::LimitExceeded, "instance recapture owners exceed overlap bounds");
		uint64_t work = 0, targetBytes = targets.size() * sizeof(GroupInstanceRecapture);
		for (size_t index = 0; index < targets.size(); ++index) {
			const auto &target = targets[index];
			if (target.NodeId.empty() || target.Port.empty() ||
				target.NodeId.size() > Limits::MaximumTextBytes ||
				target.Port.size() > Limits::MaximumTextBytes)
				return fail(Status::LimitExceeded, "instance recapture target names exceed bounds");
			targetBytes += TextBytes(target.NodeId) + TextBytes(target.Port);
			if (const uint64_t visits =
					previous.Bindings().size() + document.Nodes.size() + index + Limits::MaximumArrayElements;
				visits > (64'000'000 - work) / comparisonBytes ||
				!detail::AdmitSourceAxisWork(work, visits * comparisonBytes))
				return fail(Status::LimitExceeded, "instance recapture target lookup exceeds work bounds");
			const auto node =
				std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const auto &item) {
					return item.Id == target.NodeId;
				});
			if (node == document.Nodes.end() || detail::SourceInputInstanceBase(*node, target.Port).empty() ||
				!detail::AliasedSourceInput(*node, target.Port) ||
				!previous.Binding(target.NodeId, target.Port))
				return fail(
					Status::InvalidGroup,
					"instance recapture target has no installed input alias",
					target.NodeId,
					target.Port
				);
			for (size_t prior = 0; prior < index; ++prior)
				if (targets[prior].NodeId == target.NodeId && targets[prior].Port == target.Port)
					return fail(
						Status::InvalidValue,
						"instance recapture target is duplicated",
						target.NodeId,
						target.Port
					);
		}
		const uint64_t overhead = *documentBytes + scratchBytes + targetBytes;
		if (overhead > maximumBytes || oldBytes > maximumBytes - overhead)
			return fail(Status::LimitExceeded, "instance recapture borrowed targets exceed overlap bounds");
		auto candidate = detail::CloneGroupReplay(
			previous,
			targets.size() * 2,
			revision,
			maximumBytes - overhead,
			&previous == &result ? 0 : result.RetainedBytes(),
			diagnostic,
			targets.size()
		);
		if (!candidate) return diagnostic.Code;
		RecaptureEdit edit{*candidate, diagnostic, work, comparisonBytes};
		for (const auto &target : targets) {
			if (!edit.Admit(document.Nodes.size() + Limits::MaximumArrayElements)) return diagnostic.Code;
			const auto node =
				std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const auto &item) {
					return item.Id == target.NodeId;
				});
			if (detail::SourceSeparatedVec2Input(*node, target.Port) &&
				!edit.RetainLocalAxes(document, *node, target.Port))
				return diagnostic.Code;
		}
		std::vector<uint8_t> ready(targets.size(), 0);
		size_t unresolved = targets.size();
		while (unresolved) {
			const size_t before = unresolved;
			if (!edit.Admit(targets.size())) return diagnostic.Code;
			for (size_t index = 0; index < targets.size(); ++index) {
				if (ready[index]) continue;
				const auto &target = targets[index];
				if (!edit.Admit(
						document.Nodes.size() * 2 + candidate->Bindings.size() * 2 + targets.size() +
						Limits::MaximumArrayElements
					))
					return diagnostic.Code;
				const auto node =
					std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const auto &item) {
						return item.Id == target.NodeId;
					});
				const auto base =
					std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const auto &item) {
						return item.Id == detail::SourceInputInstanceBase(*node, target.Port);
					});
				if (base == document.Nodes.end())
					return fail(
						Status::InvalidGroup,
						"instance recapture immediate base is absent",
						target.NodeId,
						target.Port
					);
				const auto dependency = std::find_if(targets.begin(), targets.end(), [&](const auto &item) {
					return item.NodeId == base->Id && item.Port == target.Port;
				});
				if (dependency != targets.end() && !ready[size_t(dependency - targets.begin())]) continue;
				auto binding = std::find_if(
					candidate->Bindings.begin(), candidate->Bindings.end(), [&](const auto &item) {
						return item.NodeId == target.NodeId && item.Port == target.Port;
					}
				);
				const auto source = std::find_if(
					candidate->Bindings.begin(), candidate->Bindings.end(), [&](const auto &item) {
						return item.NodeId == base->Id && item.Port == target.Port;
					}
				);
				if (!detail::SourceInputInstanceBase(*base, target.Port).empty() &&
					source == candidate->Bindings.end())
					return fail(
						Status::InvalidGroup,
						"instance recapture immediate base has no input alias",
						target.NodeId,
						target.Port
					);
				const auto sourceId = source != candidate->Bindings.end() ? std::string_view(source->OwnerId)
																		  : std::string_view(base->Id);
				const auto sourcePort =
					source != candidate->Bindings.end() ? detail::BindingAnimatorPort(*source) : target.Port;
				if (!edit.Rename(binding->OwnerId, sourceId) ||
					!edit.Rename(
						binding->AnimatorPort, sourcePort == binding->Port ? std::string_view{} : sourcePort
					))
					return diagnostic.Code;
				if (!edit.Admit(
						base->SourceAnimatedInputs.size() + base->SourceStaticInputs.size() +
						node->SourceAnimatedInputs.size()
					))
					return diagnostic.Code;
				binding->Writer =
					source != candidate->Bindings.end() ? source->Writer
					: std::find(
						  base->SourceAnimatedInputs.begin(), base->SourceAnimatedInputs.end(), target.Port
					  ) != base->SourceAnimatedInputs.end()
						? GroupSubtypeAnimator::Animated
						: GroupSubtypeAnimator::Static;
				if (detail::SourceSeparatedVec2Input(*node, target.Port)) {
					GroupAxisStorage storage = GroupAxisStorage::Uninitialized;
					std::string_view axisId = node->Id, axisPort = target.Port;
					auto writer =
						std::find(
							node->SourceAnimatedInputs.begin(), node->SourceAnimatedInputs.end(), target.Port
						) != node->SourceAnimatedInputs.end()
							? GroupSubtypeAnimator::Animated
							: GroupSubtypeAnimator::Static;
					if (source != candidate->Bindings.end() &&
						source->Axes.Storage != GroupAxisStorage::None &&
						source->Axes.Storage != GroupAxisStorage::Uninitialized) {
						storage = GroupAxisStorage::Shared;
						axisId = source->Axes.OwnerId;
						axisPort = source->Axes.Port;
						writer = source->Axes.Writer;
					} else if (source == candidate->Bindings.end()) {
						if (!edit.Admit(
								candidate->SharedSubtypes.size() +
								(base->SourceSeparatedVec2Animators
									 ? base->SourceSeparatedVec2Animators->Inputs.size()
									 : 0)
							))
							return diagnostic.Code;
						const auto overlay = std::find_if(
							candidate->SharedSubtypes.begin(),
							candidate->SharedSubtypes.end(),
							[&](const auto &item) {
								return item.NodeId == base->Id && item.Port == target.Port;
							}
						);
						const auto *axes =
							overlay != candidate->SharedSubtypes.end() && overlay->SeparatedVec2
								? &*overlay->SeparatedVec2
								: detail::FindSeparatedVec2(*base, target.Port);
						if (axes && axes->Initialized) {
							storage = GroupAxisStorage::Shared;
							axisId = base->Id;
							writer = binding->Writer;
						}
					}
					if (!edit.Rename(binding->Axes.OwnerId, axisId) ||
						!edit.Rename(binding->Axes.Port, axisPort) ||
						!edit.Rename(binding->Axes.InstanceBase, node->InstanceBase))
						return diagnostic.Code;
					binding->Axes.Storage = storage;
					binding->Axes.Writer = writer;
				}
				ready[index] = 1;
				--unresolved;
			}
			if (unresolved == before)
				return fail(Status::InvalidGroup, "instance recapture contains an unresolved dependency");
		}
		GroupReplayState staged, validated;
		detail::GroupReplayAccess::Install(staged, std::move(candidate));
		const auto validationOverhead = oldBytes + scratchBytes + targetBytes;
		const auto status = BindGroupReplay(
			document,
			staged.Bindings(),
			staged,
			revision,
			validated,
			diagnostic,
			maximumBytes - validationOverhead
		);
		if (status != Status::Ok) return status;
		result = std::move(validated);
		diagnostic = {};
		return Status::Ok;
	} catch (const std::bad_alloc &) {
		diagnostic = {Status::LimitExceeded, {}, {}, "instance recapture allocation failed"};
		return diagnostic.Code;
	}
}
