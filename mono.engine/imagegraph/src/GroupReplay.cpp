#include "EvaluationAllocator.hpp"
#include "GroupBoundary.hpp"
#include "GroupReplayInternal.hpp"
#include "SourceAxisStorage.hpp"
#include "SourceLuaSockets.hpp"
#include "ValuePayload.hpp"

#include <engine/imagegraph/FrameTime.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <memory>
#include <new>
#include <tuple>

namespace engine::imagegraph {
	GroupReplayState::GroupReplayState() = default;
	GroupReplayState::~GroupReplayState() = default;
	GroupReplayState::GroupReplayState(GroupReplayState &&) noexcept = default;
	GroupReplayState &GroupReplayState::operator=(GroupReplayState &&) noexcept = default;
	const GroupReplayEntry *GroupReplayState::Find(std::string_view nodeId) const noexcept {
		if (!Data) return nullptr;
		const auto found = std::find_if(Data->Entries.begin(), Data->Entries.end(), [&](const auto &entry) {
			return entry.NodeId == nodeId;
		});
		return found == Data->Entries.end() ? nullptr : &*found;
	}
	std::span<const GroupReplayEntry> GroupReplayState::Entries() const noexcept {
		return Data ? std::span<const GroupReplayEntry>{Data->Entries} : std::span<const GroupReplayEntry>{};
	}
	std::span<const GroupSubtypeBinding> GroupReplayState::Bindings() const noexcept {
		return Data ? std::span<const GroupSubtypeBinding>{Data->Bindings}
					: std::span<const GroupSubtypeBinding>{};
	}
	std::span<const GroupSubtypeOverlay> GroupReplayState::SharedSubtypes() const noexcept {
		return Data ? std::span<const GroupSubtypeOverlay>{Data->SharedSubtypes}
					: std::span<const GroupSubtypeOverlay>{};
	}
	std::span<const DetachedSourceAnimator> GroupReplayState::DetachedAnimators() const noexcept {
		return Data ? std::span<const DetachedSourceAnimator>{Data->DetachedAnimators}
					: std::span<const DetachedSourceAnimator>{};
	}
	const DetachedSourceAnimator *
	GroupReplayState::DetachedAnimator(std::string_view ownerId, std::string_view id) const noexcept {
		for (const auto &animator : DetachedAnimators())
			if (animator.OwnerId == ownerId && animator.Id == id) return &animator;
		return nullptr;
	}
	const GroupSubtypeBinding *
	GroupReplayState::Binding(std::string_view id, std::string_view port) const noexcept {
		for (const auto &binding : Bindings())
			if (binding.NodeId == id && binding.Port == port) return &binding;
		return nullptr;
	}
	const GroupSubtypeOverlay *
	GroupReplayState::SharedSubtype(std::string_view id, std::string_view port) const noexcept {
		for (const auto &overlay : SharedSubtypes())
			if (overlay.NodeId == id && overlay.Port == port) return &overlay;
		return nullptr;
	}
	bool GroupReplayState::InstancesBound() const noexcept {
		return Data && Data->Bound;
	}
	uint64_t GroupReplayState::RetainedBytes() const noexcept {
		return Data ? Data->Charge.Bytes() : 0;
	}
	uint64_t GroupReplayState::AuthoringRevision() const noexcept {
		return Data ? Data->Revision : 0;
	}
	namespace {
		std::optional<uint64_t> AdmitGroupReplayDocument(
			const Document &document,
			const GroupReplayState &previous,
			const GroupReplayState &result,
			uint64_t maximumBytes,
			Diagnostic &diagnostic
		) {
			const auto refuse = [&]() -> std::optional<uint64_t> {
				diagnostic = {
					Status::LimitExceeded, {}, {}, "Group document or replacement overlap exceeds bounds"
				};
				return std::nullopt;
			};
			if (!maximumBytes || maximumBytes > Limits::MaximumEvaluationBytes ||
				document.Nodes.size() > Limits::MaximumNodes ||
				document.Groups.size() > Limits::MaximumGroups ||
				document.Junctions.size() > Limits::MaximumJunctions ||
				document.Links.size() > Limits::MaximumLinks ||
				document.Keyframes.size() > Limits::MaximumKeyframes ||
				document.Tracks.size() > Limits::MaximumTracks ||
				document.Outputs.size() > Limits::MaximumOutputs)
				return refuse();
			// Bound public record counts before following owners or comparing payloads.
			for (const auto &node : document.Nodes)
				if (node.Values.size() > Limits::MaximumArrayElements ||
					node.SourceProperties.size() > Limits::MaximumPropertiesPerNode ||
					node.DynamicInputs.size() > MaximumDynamicInputsForNode(node) ||
					node.InstanceOverrides.size() > Limits::MaximumArrayElements ||
					node.SourceAnimatedInputs.size() > Limits::MaximumArrayElements ||
					node.SourceStaticInputs.size() > Limits::MaximumArrayElements)
					return refuse();
			for (const auto &group : document.Groups)
				if (group.Ports.size() > Limits::MaximumGroupPorts) return refuse();
			const auto bytes = DocumentRetainedPayloadBytes(document);
			const uint64_t destination = &previous == &result ? 0 : result.RetainedBytes();
			if (!bytes || *bytes >= maximumBytes || previous.RetainedBytes() > maximumBytes - *bytes ||
				destination > maximumBytes - *bytes - previous.RetainedBytes())
				return refuse();
			return maximumBytes - *bytes;
		}
	}
	static bool RetireUnreferencedDetachedAnimators(detail::GroupReplayAccess::Owner &owner) {
		if (!owner.DetachedAnimators.empty() &&
			(owner.Bindings.size() + owner.SharedSubtypes.size() + owner.DetachedAnimators.size()) >
				64'000'000 / owner.DetachedAnimators.size())
			return false;
		uint64_t released = 0;
		for (auto animator = owner.DetachedAnimators.begin(); animator != owner.DetachedAnimators.end();) {
			if (std::any_of(owner.Bindings.begin(), owner.Bindings.end(), [&](const auto &binding) {
					return (binding.OwnerId == animator->OwnerId &&
							detail::BindingAnimatorPort(binding) == animator->Id) ||
						   (binding.Axes.Storage != GroupAxisStorage::None &&
							binding.Axes.OwnerId == animator->OwnerId && binding.Axes.Port == animator->Id);
				})) {
				++animator;
				continue;
			}
			for (auto effect = owner.SharedSubtypes.begin(); effect != owner.SharedSubtypes.end();) {
				if (effect->NodeId != animator->OwnerId || effect->Port != animator->Id) {
					++effect;
					continue;
				}
				released += std::max(effect->NodeId.size(), std::string{}.capacity()) + 1 +
							std::max(effect->Port.size(), std::string{}.capacity()) + 1;
				if (effect->Fixed) released += detail::RetainedPayloadBytes(*effect->Fixed);
				released += detail::SeparatedOverlayBytes(*effect);
				released += effect->Keys.capacity() * sizeof(Keyframe);
				for (const auto &key : effect->Keys)
					released += *KeyframePayloadBytes(key) - sizeof(Keyframe);
				effect = owner.SharedSubtypes.erase(effect);
			}
			released += detail::DetachedAnimatorBytes(*animator);
			animator = owner.DetachedAnimators.erase(animator);
		}
		// Vector capacity remains live; only destroyed nested storage leaves its charge.
		if (!owner.Charge.Resize(owner.Charge.Bytes() - released)) std::terminate();
		return true;
	}
	Status BindGroupReplay(
		const Document &document,
		std::span<const GroupSubtypeBinding> bindings,
		const GroupReplayState &previous,
		uint64_t revision,
		GroupReplayState &result,
		Diagnostic &diagnostic,
		uint64_t maximumBytes
	) try {
		const auto fail = [&](Status code, std::string message, std::string_view node = {}) {
			diagnostic = {code, std::string(node), "subtype", std::move(message)};
			return code;
		};
		if (previous.AuthoringRevision() != revision)
			return fail(Status::InvalidValue, "Group binding requires callback state at this revision");
		const auto ownerBudget =
			AdmitGroupReplayDocument(document, previous, result, maximumBytes, diagnostic);
		if (!ownerBudget) return diagnostic.Code;
		constexpr size_t maximumBindings = Limits::MaximumNodes * Limits::MaximumArrayElements;
		if (bindings.size() > maximumBindings)
			return fail(Status::LimitExceeded, "source input binding count exceeds public bounds");
		if (uint64_t(bindings.size()) * previous.Bindings().size() > 64'000'000)
			return fail(
				Status::LimitExceeded, "retained source animator binding reconciliation exceeds work bounds"
			);
		const auto retainedAnimatorPort = [&](const GroupSubtypeBinding &binding) -> std::string_view {
			if (!binding.AnimatorPort.empty()) return binding.AnimatorPort;
			const auto *old = previous.Binding(binding.NodeId, binding.Port);
			if (old && old->OwnerId == binding.OwnerId) return old->AnimatorPort;
			return {};
		};
		const auto animatorPort = [&](const GroupSubtypeBinding &binding) -> std::string_view {
			const auto retained = retainedAnimatorPort(binding);
			return retained.empty() ? std::string_view(binding.Port) : retained;
		};
		struct AxisDefinition {
			GroupAxisStorage Storage = GroupAxisStorage::None;
			std::string_view OwnerId, Port, InstanceBase;
			GroupSubtypeAnimator Writer = GroupSubtypeAnimator::Static;
			bool Valid = true;
			bool Ready = true;
		};
		uint64_t names = bindings.size() * sizeof(GroupSubtypeBinding);
		const uint64_t scratchBytes = bindings.size() * (sizeof(const GroupSubtypeBinding *) +
														 sizeof(AxisDefinition) + sizeof(uint8_t)) +
									  document.Nodes.size() * sizeof(const Node *);
		const uint64_t priorBytes =
			previous.RetainedBytes() + (&previous == &result ? 0 : result.RetainedBytes());
		if (names > *ownerBudget || scratchBytes > *ownerBudget - names ||
			priorBytes > *ownerBudget - names - scratchBytes)
			return fail(Status::LimitExceeded, "source input binding slots exceed operation bounds");
		for (const auto &binding : bindings)
			for (const auto id :
				 {std::string_view(binding.NodeId),
				  std::string_view(binding.OwnerId),
				  std::string_view(binding.Port),
				  retainedAnimatorPort(binding)}) {
				const auto bytes = std::max(id.size(), std::string{}.capacity()) + 1;
				if (id.size() > Limits::MaximumTextBytes || bytes > UINT64_MAX - names)
					return fail(Status::LimitExceeded, "source input binding names exceed bounds");
				names += bytes;
			}
		if (names > *ownerBudget || scratchBytes > *ownerBudget - names ||
			priorBytes > *ownerBudget - names - scratchBytes)
			return fail(
				Status::LimitExceeded, "source input bindings and sorted indices exceed operation bounds"
			);
		detail::EvaluationBudget scratchBudget(scratchBytes);
		auto scratchCharge = scratchBudget.Reserve(scratchBytes);
		if (!scratchCharge) return fail(Status::LimitExceeded, "source input binding index admission failed");
		std::vector<const Node *> nodeIndex;
		nodeIndex.reserve(document.Nodes.size());
		for (const auto &node : document.Nodes)
			nodeIndex.push_back(&node);
		std::sort(nodeIndex.begin(), nodeIndex.end(), [](const auto *left, const auto *right) {
			return left->Id < right->Id;
		});
		const auto nodeById = [&](std::string_view id) -> const Node * {
			const auto found = std::lower_bound(
				nodeIndex.begin(), nodeIndex.end(), id, [](const auto *node, std::string_view id) {
					return node->Id < id;
				}
			);
			return found != nodeIndex.end() && (*found)->Id == id ? *found : nullptr;
		};
		const auto writerMode = [&](const GroupSubtypeBinding &binding) {
			const auto retained = retainedAnimatorPort(binding);
			if (retained.empty()) return binding.Writer;
			const auto *old = previous.Binding(binding.NodeId, binding.Port);
			auto mode = old ? old->Writer : binding.Writer;
			const auto *owner = nodeById(binding.OwnerId);
			if (const auto *detached = previous.DetachedAnimator(binding.OwnerId, retained))
				return detached->Writer;
			if (owner &&
				std::find(owner->SourceStaticInputs.begin(), owner->SourceStaticInputs.end(), retained) !=
					owner->SourceStaticInputs.end())
				mode = GroupSubtypeAnimator::Static;
			else if (owner &&
					 std::find(
						 owner->SourceAnimatedInputs.begin(), owner->SourceAnimatedInputs.end(), retained
					 ) != owner->SourceAnimatedInputs.end())
				mode = GroupSubtypeAnimator::Animated;
			return mode;
		};
		std::vector<AxisDefinition> axisDefinitions(bindings.size());
		std::vector<uint8_t> axisReady(bindings.size(), 0);
		uint64_t axisBindingWork = 0;
		bool axisWorkValid = true;
		const auto admitAxisBindingWork = [&](uint64_t visits) {
			if (visits > 64'000'000 - axisBindingWork) {
				axisWorkValid = false;
				return false;
			}
			axisBindingWork += visits;
			return true;
		};
		const auto axisWriter = [&](std::string_view id,
									std::string_view port,
									GroupSubtypeAnimator fallback) {
			if (!admitAxisBindingWork(previous.DetachedAnimators().size() + document.Nodes.size()))
				return fallback;
			if (const auto *detached = previous.DetachedAnimator(id, port)) return detached->Writer;
			const auto *node = nodeById(id);
			if (!node) return fallback;
			if (!admitAxisBindingWork(node->SourceStaticInputs.size() + node->SourceAnimatedInputs.size()))
				return fallback;
			if (std::find(node->SourceStaticInputs.begin(), node->SourceStaticInputs.end(), port) !=
				node->SourceStaticInputs.end())
				return GroupSubtypeAnimator::Static;
			if (std::find(node->SourceAnimatedInputs.begin(), node->SourceAnimatedInputs.end(), port) !=
				node->SourceAnimatedInputs.end())
				return GroupSubtypeAnimator::Animated;
			return fallback;
		};
		const auto axisDefinition = [&](const GroupSubtypeBinding &binding) -> AxisDefinition {
			if (!admitAxisBindingWork(document.Nodes.size()))
				return {GroupAxisStorage::None, {}, {}, {}, GroupSubtypeAnimator::Static, false};
			const auto *target = nodeById(binding.NodeId);
			if (!target) return {};
			if (!admitAxisBindingWork(target->DynamicInputs.size()))
				return {GroupAxisStorage::None, {}, {}, {}, GroupSubtypeAnimator::Static, false};
			if (!detail::SourceSeparatedVec2Input(*target, binding.Port)) return {};
			if (!admitAxisBindingWork(previous.Bindings().size() + document.Nodes.size()))
				return {GroupAxisStorage::None, {}, {}, {}, GroupSubtypeAnimator::Static, false};
			const auto *old = previous.Binding(binding.NodeId, binding.Port);
			if (old && old->OwnerId == binding.OwnerId && old->Axes.Storage != GroupAxisStorage::None &&
				old->Axes.InstanceBase == target->InstanceBase)
				return {
					old->Axes.Storage,
					old->Axes.OwnerId,
					old->Axes.Port,
					old->Axes.InstanceBase,
					axisWriter(old->Axes.OwnerId, old->Axes.Port, old->Axes.Writer)
				};
			const Node *source = nodeById(target->InstanceBase);
			if (source) {
				if (!admitAxisBindingWork(
						previous.Bindings().size() + previous.SharedSubtypes().size() +
						(source->SourceSeparatedVec2Animators
							 ? source->SourceSeparatedVec2Animators->Inputs.size()
							 : 0)
					))
					return {GroupAxisStorage::None, {}, {}, {}, GroupSubtypeAnimator::Static, false};
				if (!admitAxisBindingWork(bindings.size()))
					return {GroupAxisStorage::None, {}, {}, {}, GroupSubtypeAnimator::Static, false};
				const auto requested = std::find_if(bindings.begin(), bindings.end(), [&](const auto &item) {
					return item.NodeId == source->Id && item.Port == binding.Port;
				});
				if (requested != bindings.end()) {
					const size_t index = size_t(requested - bindings.begin());
					if (!axisReady[index])
						return {
							GroupAxisStorage::None, {}, {}, {}, GroupSubtypeAnimator::Static, true, false
						};
					const auto &baseAxes = axisDefinitions[index];
					if (baseAxes.Storage != GroupAxisStorage::None &&
						baseAxes.Storage != GroupAxisStorage::Uninitialized)
						return {
							GroupAxisStorage::Shared,
							baseAxes.OwnerId,
							baseAxes.Port,
							target->InstanceBase,
							axisWriter(baseAxes.OwnerId, baseAxes.Port, baseAxes.Writer)
						};
					return {
						GroupAxisStorage::Uninitialized,
						target->Id,
						binding.Port,
						target->InstanceBase,
						axisWriter(target->Id, binding.Port, GroupSubtypeAnimator::Static)
					};
				}
				const auto *baseBinding = previous.Binding(source->Id, binding.Port);
				if (baseBinding && baseBinding->Axes.Storage != GroupAxisStorage::None &&
					baseBinding->Axes.InstanceBase == source->InstanceBase) {
					if (baseBinding->Axes.Storage != GroupAxisStorage::Uninitialized)
						return {
							GroupAxisStorage::Shared,
							baseBinding->Axes.OwnerId,
							baseBinding->Axes.Port,
							target->InstanceBase,
							axisWriter(
								baseBinding->Axes.OwnerId, baseBinding->Axes.Port, baseBinding->Axes.Writer
							)
						};
				} else {
					const auto *overlay = previous.SharedSubtype(source->Id, binding.Port);
					if ((overlay && overlay->SeparatedVec2) ||
						detail::FindSeparatedVec2(*source, binding.Port))
						return {
							GroupAxisStorage::Shared,
							source->Id,
							binding.Port,
							target->InstanceBase,
							axisWriter(source->Id, binding.Port, binding.Writer)
						};
				}
			}
			return {
				GroupAxisStorage::Uninitialized,
				target->Id,
				binding.Port,
				target->InstanceBase,
				axisWriter(target->Id, binding.Port, GroupSubtypeAnimator::Static)
			};
		};
		size_t unresolved = bindings.size();
		while (unresolved) {
			if (!admitAxisBindingWork(bindings.size()))
				return fail(Status::LimitExceeded, "axis binding dependency checks exceed work bounds");
			const size_t before = unresolved;
			for (size_t index = 0; index < bindings.size(); ++index) {
				if (axisReady[index]) continue;
				const auto axes = axisDefinition(bindings[index]);
				if (!axes.Valid || !axisWorkValid)
					return fail(
						Status::LimitExceeded, "axis binding exceeds work bounds", bindings[index].NodeId
					);
				if (!axes.Ready) continue;
				axisDefinitions[index] = axes;
				axisReady[index] = 1;
				--unresolved;
			}
			if (unresolved == before)
				return fail(Status::InvalidGroup, "axis bindings contain an unresolved instance dependency");
		}
		for (const auto &binding : bindings) {
			for (const auto text :
				 {std::string_view(binding.Axes.OwnerId),
				  std::string_view(binding.Axes.Port),
				  std::string_view(binding.Axes.InstanceBase)})
				if (text.size() > Limits::MaximumTextBytes)
					return fail(Status::LimitExceeded, "axis binding names exceed bounds", binding.NodeId);
			if (binding.Axes.Storage == GroupAxisStorage::None &&
				(!binding.Axes.OwnerId.empty() || !binding.Axes.Port.empty() ||
				 !binding.Axes.InstanceBase.empty()))
				return fail(
					Status::InvalidGroup, "unspecified axis binding carries identity names", binding.NodeId
				);
			if (binding.Axes.Storage != GroupAxisStorage::None) {
				const auto *old = previous.Binding(binding.NodeId, binding.Port);
				if (!old || !(binding.Axes == old->Axes))
					return fail(
						Status::InvalidGroup, "axis identity requires an admitted binding", binding.NodeId
					);
			}
			const auto &axes = axisDefinitions[size_t(&binding - bindings.data())];
			if (!axes.Valid || !axisWorkValid)
				return fail(Status::LimitExceeded, "axis binding exceeds work bounds", binding.NodeId);
			for (const auto text : {axes.OwnerId, axes.Port, axes.InstanceBase}) {
				const auto bytes = std::max(text.size(), std::string{}.capacity()) + 1;
				if (bytes > UINT64_MAX - names)
					return fail(Status::LimitExceeded, "axis binding names exceed bounds", binding.NodeId);
				names += bytes;
			}
		}
		if (names > *ownerBudget || scratchBytes > *ownerBudget - names ||
			priorBytes > *ownerBudget - names - scratchBytes)
			return fail(Status::LimitExceeded, "axis bindings exceed operation bounds");
		std::vector<const GroupSubtypeBinding *> sorted;
		sorted.reserve(bindings.size());
		for (const auto &binding : bindings)
			sorted.push_back(&binding);
		std::sort(sorted.begin(), sorted.end(), [](const auto *left, const auto *right) {
			return std::tie(left->NodeId, left->Port) < std::tie(right->NodeId, right->Port);
		});
		for (size_t index = 1; index < sorted.size(); ++index)
			if (sorted[index - 1]->NodeId == sorted[index]->NodeId &&
				sorted[index - 1]->Port == sorted[index]->Port)
				return fail(
					Status::InvalidValue, "source input binding target is duplicated", sorted[index]->NodeId
				);
		std::sort(sorted.begin(), sorted.end(), [&](const auto *left, const auto *right) {
			return std::tuple<std::string_view, std::string_view>{left->OwnerId, animatorPort(*left)} <
				   std::tuple<std::string_view, std::string_view>{right->OwnerId, animatorPort(*right)};
		});
		for (size_t index = 1; index < sorted.size(); ++index)
			if (sorted[index - 1]->OwnerId == sorted[index]->OwnerId &&
				animatorPort(*sorted[index - 1]) == animatorPort(*sorted[index]) &&
				writerMode(*sorted[index - 1]) != writerMode(*sorted[index]))
				return fail(
					Status::InvalidValue,
					"source input bindings disagree about their original writer mode",
					sorted[index]->NodeId
				);

		for (size_t index = 0; index < bindings.size(); ++index) {
			const auto &binding = bindings[index];
			if (!binding.AnimatorPort.empty()) {
				const auto *old = previous.Binding(binding.NodeId, binding.Port);
				if (!old || old->OwnerId != binding.OwnerId ||
					detail::BindingAnimatorPort(*old) != binding.AnimatorPort)
					return fail(
						Status::InvalidGroup,
						"retained animator identity must come from an admitted physical port transaction",
						binding.NodeId
					);
			}

			const Node *target = nodeById(binding.NodeId);
			if (!target || target->InstanceBase.empty() || !detail::AliasedSourceInput(*target, binding.Port))
				return fail(
					Status::InvalidGroup, "Group binding target is not an instance input", binding.NodeId
				);

			const auto validMode = [](GroupSubtypeAnimator mode) {
				return mode == GroupSubtypeAnimator::Static || mode == GroupSubtypeAnimator::Animated;
			};
			if (!validMode(binding.Getter) || !validMode(binding.Writer))
				return fail(Status::InvalidValue, "Group binding animator mode is invalid", binding.NodeId);
			const Node *owner = target;
			for (size_t hop = 0; owner && !owner->InstanceBase.empty() && hop < document.Nodes.size(); ++hop)
				owner = nodeById(owner->InstanceBase);
			if (!owner || !owner->InstanceBase.empty() || owner->Id != binding.OwnerId ||
				owner->Type != target->Type ||
				(!detail::AliasedSourceInput(*owner, animatorPort(binding)) &&
				 !previous.DetachedAnimator(binding.OwnerId, animatorPort(binding))))
				return fail(
					Status::InvalidGroup,
					"Group binding does not name its bounded original owner",
					binding.NodeId
				);
		}
		for (const auto &node : document.Nodes)
			if (node.Type == "pc.group_input" && !node.InstanceBase.empty() &&
				std::none_of(bindings.begin(), bindings.end(), [&](const auto &binding) {
					return binding.NodeId == node.Id && binding.Port == "subtype";
				}))
				return fail(Status::InvalidGroup, "Group binding omits an instance input", node.Id);
		auto candidate = detail::CloneGroupReplay(
			previous,
			bindings.size(),
			revision,
			*ownerBudget - scratchBytes,
			&previous == &result ? 0 : result.RetainedBytes(),
			diagnostic
		);
		if (!candidate) return diagnostic.Code;
		auto bindingCharge = candidate->Budget.Reserve(names);
		if (!bindingCharge) return fail(Status::LimitExceeded, "Group binding exceeds live operation budget");
		// A revision refresh replaces bindings while retaining frozen callback declarations.
		std::vector<GroupSubtypeBinding> replacementBindings;
		replacementBindings.reserve(bindings.size());
		for (const auto &binding : bindings) {
			const auto &axes = axisDefinitions[size_t(&binding - bindings.data())];
			if (!axes.Valid || !axisWorkValid)
				return fail(Status::LimitExceeded, "axis binding exceeds work bounds", binding.NodeId);
			replacementBindings.push_back(
				{binding.NodeId,
				 binding.OwnerId,
				 binding.Getter,
				 writerMode(binding),
				 binding.Port,
				 std::string(retainedAnimatorPort(binding)),
				 {axes.Storage,
				  std::string(axes.OwnerId),
				  std::string(axes.Port),
				  std::string(axes.InstanceBase),
				  axes.Writer}}
			);
		}
		uint64_t removedBytes = candidate->Bindings.size() * sizeof(GroupSubtypeBinding);
		for (const auto &binding : candidate->Bindings)
			removedBytes += std::max(binding.NodeId.size(), std::string{}.capacity()) + 1 +
							std::max(binding.OwnerId.size(), std::string{}.capacity()) + 1 +
							std::max(binding.Port.size(), std::string{}.capacity()) + 1 +
							std::max(binding.AnimatorPort.size(), std::string{}.capacity()) + 1 +
							std::max(binding.Axes.OwnerId.size(), std::string{}.capacity()) + 1 +
							std::max(binding.Axes.Port.size(), std::string{}.capacity()) + 1 +
							std::max(binding.Axes.InstanceBase.size(), std::string{}.capacity()) + 1;
		candidate->Bindings.swap(replacementBindings);
		std::vector<GroupSubtypeBinding>{}.swap(replacementBindings);
		if (!candidate->Charge.Merge(std::move(*bindingCharge))) std::terminate();
		for (auto &entry : candidate->Entries) {
			const Node *node = nodeById(entry.NodeId);
			const bool retired = !node || node->Type != "pc.group_input";
			const bool target = node && !node->InstanceBase.empty() &&
								std::any_of(bindings.begin(), bindings.end(), [&](const auto &binding) {
									return binding.NodeId == node->Id && binding.Port == "subtype";
								});
			const bool sharedOwner = std::any_of(bindings.begin(), bindings.end(), [&](const auto &binding) {
				return binding.OwnerId == entry.NodeId && binding.Port == "subtype";
			});
			if (retired || target) {
				if (entry.SubtypeStatic) removedBytes += detail::RetainedPayloadBytes(*entry.SubtypeStatic);
				for (const auto &key : entry.SubtypeKeys)
					removedBytes += *KeyframePayloadBytes(key);
				entry.SubtypeStatic.reset();
				std::vector<Keyframe>{}.swap(entry.SubtypeKeys);
			} else if (sharedOwner && (entry.SubtypeStatic || !entry.SubtypeKeys.empty())) {
				const uint64_t bytes = std::max(entry.NodeId.size(), std::string{}.capacity()) + 1 +
									   std::string{}.capacity() + 1;
				auto nameCharge = candidate->Budget.Reserve(bytes);
				if (!nameCharge)
					return fail(Status::LimitExceeded, "Group owner name exceeds operation budget");
				GroupSubtypeOverlay overlay;
				overlay.NodeId = std::string(entry.NodeId);
				overlay.Fixed = std::move(entry.SubtypeStatic);
				overlay.Keys = std::move(entry.SubtypeKeys);
				entry.SubtypeStatic.reset();
				candidate->SharedSubtypes.push_back(std::move(overlay));
				if (!candidate->Charge.Merge(std::move(*nameCharge))) std::terminate();
			}
			if (retired) {
				removedBytes += std::max(entry.NodeId.size(), std::string{}.capacity()) + 1;
				if (entry.ParentReset) removedBytes += detail::RetainedPayloadBytes(*entry.ParentReset);
				for (const auto &key : entry.ParentKeys)
					removedBytes += *KeyframePayloadBytes(key);
			}
		}
		std::erase_if(candidate->Entries, [&](const auto &entry) {
			const Node *node = nodeById(entry.NodeId);
			return !node || node->Type != "pc.group_input";
		});
		if (!candidate->Charge.Resize(candidate->Charge.Bytes() - removedBytes)) std::terminate();
		candidate->Bound = true;
		if (!RetireUnreferencedDetachedAnimators(*candidate)) {
			diagnostic = {
				Status::LimitExceeded, {}, {}, "detached animator reference checks exceed work bounds"
			};
			return diagnostic.Code;
		}
		detail::GroupReplayAccess::Install(result, std::move(candidate));
		diagnostic = {};
		return Status::Ok;
	} catch (const std::bad_alloc &) {
		diagnostic = {Status::LimitExceeded, {}, {}, "Group binding allocation failed"};
		return diagnostic.Code;
	}

	Status RebindGroupReplay(
		const Document &document,
		const GroupReplayState &previous,
		uint64_t revision,
		GroupReplayState &result,
		Diagnostic &diagnostic,
		uint64_t maximumBytes
	) try {
		const auto ownerBudget =
			AdmitGroupReplayDocument(document, previous, result, maximumBytes, diagnostic);
		if (!ownerBudget) return diagnostic.Code;

		const auto sourceSurvives = [&](std::string_view id, std::string_view port) {
			return std::any_of(document.Nodes.begin(), document.Nodes.end(), [&](const Node &node) {
				return node.Id == id && detail::AliasedSourceInput(node, port);
			});
		};
		const auto survives = [&](std::string_view id) {
			return std::any_of(document.Nodes.begin(), document.Nodes.end(), [&](const Node &node) {
				return node.Id == id && node.Type == "pc.group_input";
			});
		};
		auto candidate = detail::CloneGroupReplay(
			previous, 0, revision, *ownerBudget, &previous == &result ? 0 : result.RetainedBytes(), diagnostic
		);
		if (!candidate) return diagnostic.Code;
		uint64_t removedBytes = 0;
		for (const auto &entry : previous.Entries()) {
			if (survives(entry.NodeId)) continue;
			removedBytes += std::max(entry.NodeId.size(), std::string{}.capacity()) + 1;
			for (const auto *value : {&entry.ParentReset, &entry.SubtypeStatic})
				if (*value) removedBytes += detail::RetainedPayloadBytes(**value);
			for (const auto *keys : {&entry.SubtypeKeys, &entry.ParentKeys})
				for (const auto &key : *keys)
					removedBytes += *KeyframePayloadBytes(key);
		}
		std::erase_if(candidate->Entries, [&](const auto &entry) { return !survives(entry.NodeId); });
		for (const auto &binding : candidate->Bindings) {
			if (sourceSurvives(binding.NodeId, binding.Port) &&
				binding.Axes.Storage != GroupAxisStorage::None &&
				!sourceSurvives(binding.Axes.OwnerId, binding.Axes.Port) &&
				!previous.DetachedAnimator(binding.Axes.OwnerId, binding.Axes.Port)) {
				diagnostic = {
					Status::InvalidGroup,
					binding.NodeId,
					binding.Port,
					"retained axis identity requires a physical input move transaction"
				};
				return diagnostic.Code;
			}
			if (sourceSurvives(binding.NodeId, binding.Port) &&
				(sourceSurvives(binding.OwnerId, detail::BindingAnimatorPort(binding)) ||
				 previous.DetachedAnimator(binding.OwnerId, detail::BindingAnimatorPort(binding))))
				continue;
			removedBytes += std::max(binding.NodeId.size(), std::string{}.capacity()) + 1;
			removedBytes += std::max(binding.OwnerId.size(), std::string{}.capacity()) + 1;
			removedBytes += std::max(binding.Port.size(), std::string{}.capacity()) + 1;
			removedBytes += std::max(binding.AnimatorPort.size(), std::string{}.capacity()) + 1 +
							std::max(binding.Axes.OwnerId.size(), std::string{}.capacity()) + 1 +
							std::max(binding.Axes.Port.size(), std::string{}.capacity()) + 1 +
							std::max(binding.Axes.InstanceBase.size(), std::string{}.capacity()) + 1;
		}
		std::erase_if(candidate->Bindings, [&](const auto &binding) {
			return !sourceSurvives(binding.NodeId, binding.Port) ||
				   !(sourceSurvives(binding.OwnerId, detail::BindingAnimatorPort(binding)) ||
					 previous.DetachedAnimator(binding.OwnerId, detail::BindingAnimatorPort(binding)));
		});
		for (const auto &overlay : candidate->SharedSubtypes) {
			if (sourceSurvives(overlay.NodeId, overlay.Port) ||
				previous.DetachedAnimator(overlay.NodeId, overlay.Port))
				continue;
			removedBytes += std::max(overlay.NodeId.size(), std::string{}.capacity()) + 1;
			removedBytes += std::max(overlay.Port.size(), std::string{}.capacity()) + 1;
			if (overlay.Fixed) removedBytes += detail::RetainedPayloadBytes(*overlay.Fixed);
			removedBytes += detail::SeparatedOverlayBytes(overlay);
			for (const auto &key : overlay.Keys)
				removedBytes += *KeyframePayloadBytes(key);
		}
		std::erase_if(candidate->SharedSubtypes, [&](const auto &overlay) {
			return !sourceSurvives(overlay.NodeId, overlay.Port) &&
				   !previous.DetachedAnimator(overlay.NodeId, overlay.Port);
		});
		// Entry-vector capacity remains live; only deleted owned names and payloads
		// leave its charge.
		if (!candidate->Charge.Resize(candidate->Charge.Bytes() - removedBytes)) std::terminate();
		if (!RetireUnreferencedDetachedAnimators(*candidate)) {
			diagnostic = {
				Status::LimitExceeded, {}, {}, "detached animator reference checks exceed work bounds"
			};
			return diagnostic.Code;
		}
		detail::GroupReplayAccess::Install(result, std::move(candidate));
		diagnostic = {};
		return Status::Ok;
	} catch (const std::bad_alloc &) {
		diagnostic = {Status::LimitExceeded, {}, {}, "Group rebind allocation failed"};
		return diagnostic.Code;
	}
	Status RebindGroupReplayWithInputMoves(
		const Document &original,
		const Document &staged,
		std::span<const SourceInputMove> moves,
		const GroupReplayState &previous,
		uint64_t revision,
		GroupReplayState &result,
		Diagnostic &diagnostic,
		uint64_t maximumBytes
	) try {
		const auto fail = [&](Status code, std::string_view message) {
			diagnostic = {code, {}, {}, std::string(message)};
			return code;
		};
		const auto available = AdmitGroupReplayDocument(staged, previous, result, maximumBytes, diagnostic);
		if (!available) return diagnostic.Code;
		const auto originalAvailable =
			AdmitGroupReplayDocument(original, previous, result, maximumBytes, diagnostic);
		if (!originalAvailable) return diagnostic.Code;
		const auto oldBytes = DocumentRetainedPayloadBytes(original);
		const uint64_t overlap = &original == &staged ? 0 : *oldBytes;
		if (overlap >= *available || moves.size() > Limits::MaximumNodes * Limits::MaximumArrayElements)
			return fail(Status::LimitExceeded, "source input move document overlap exceeds bounds");
		const uint64_t records = original.Nodes.size() + staged.Nodes.size() + previous.Bindings().size() +
								 previous.SharedSubtypes().size();
		if (moves.size() && records > 64'000'000 / moves.size())
			return fail(Status::LimitExceeded, "source input move reconciliation exceeds work bounds");
		const auto nodeIn = [](const Document &document, std::string_view id) -> const Node * {
			const auto found =
				std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const Node &node) {
					return node.Id == id;
				});
			return found == document.Nodes.end() ? nullptr : &*found;
		};
		const auto mapped = [&](std::string_view id, std::string_view port) -> std::string_view {
			for (const auto &move : moves)
				if (move.NodeId == id && move.OldPort == port) return move.NewPort;
			return port;
		};
		if (original.Nodes.size() != staged.Nodes.size())
			return fail(Status::InvalidGroup, "source input move cannot add or remove nodes");
		for (const auto &node : original.Nodes) {
			const auto *target = nodeIn(staged, node.Id);
			if (!target || target->Type != node.Type || target->InstanceBase != node.InstanceBase)
				return fail(Status::InvalidGroup, "source input move changes node ownership");
		}
		uint64_t work = 0;
		for (size_t i = 0; i < moves.size(); ++i) {
			const auto &move = moves[i];
			if (move.NodeId.empty() || move.OldPort.empty() || move.OldPort == move.NewPort ||
				move.NodeId.size() > Limits::MaximumTextBytes ||
				move.OldPort.size() > Limits::MaximumTextBytes ||
				move.NewPort.size() > Limits::MaximumTextBytes)
				return fail(Status::InvalidValue, "source input move identity is invalid");
			const auto *before = nodeIn(original, move.NodeId);
			const auto *after = nodeIn(staged, move.NodeId);
			const auto *oldInput = before ? detail::AliasedSourceInput(*before, move.OldPort) : nullptr;
			if (!oldInput || std::none_of(
								 before->DynamicInputs.begin(),
								 before->DynamicInputs.end(),
								 [&](const auto &input) { return input.Id == move.OldPort; }
							 ))
				return fail(
					Status::InvalidGroup, "source input move requires an original dynamic physical socket"
				);
			if (!move.NewPort.empty()) {
				const auto *newInput = detail::AliasedSourceInput(*after, move.NewPort);
				if (!newInput || oldInput->Id != newInput->Id ||
					oldInput->SourceIndex != newInput->SourceIndex)
					return fail(
						Status::InvalidGroup, "source input move changes the physical socket template"
					);
			}
			if (moves.size() > 64'000'000 - work)
				return fail(Status::LimitExceeded, "source input move collision checks exceed work bounds");
			work += moves.size();
			for (size_t j = 0; j < i; ++j) {
				const auto &other = moves[j];
				if (other.NodeId == move.NodeId && (other.OldPort == move.OldPort ||
													(!move.NewPort.empty() && other.NewPort == move.NewPort)))
					return fail(Status::InvalidValue, "source input moves collide");
			}
			if (uint64_t(before->DynamicInputs.size()) * after->DynamicInputs.size() * (moves.size() + 1) >
				64'000'000 - work)
				return fail(Status::LimitExceeded, "source input move membership checks exceed work bounds");
			work += uint64_t(before->DynamicInputs.size()) * after->DynamicInputs.size() * (moves.size() + 1);
			for (const auto &input : before->DynamicInputs) {
				const auto target = mapped(move.NodeId, input.Id);
				if (!target.empty() && std::count_if(
										   after->DynamicInputs.begin(),
										   after->DynamicInputs.end(),
										   [&](const auto &candidate) { return candidate.Id == target; }
									   ) != 1)
					return fail(Status::InvalidGroup, "staged source input move membership is incomplete");
			}
			for (const auto &input : after->DynamicInputs)
				if (std::count_if(
						before->DynamicInputs.begin(),
						before->DynamicInputs.end(),
						[&](const auto &candidate) { return mapped(move.NodeId, candidate.Id) == input.Id; }
					) != 1)
					return fail(
						Status::InvalidGroup, "staged source input move has an unmatched or colliding socket"
					);
		}
		auto candidate = detail::CloneGroupReplay(
			previous,
			moves.size(),
			revision,
			*available - overlap,
			&previous == &result ? 0 : result.RetainedBytes(),
			diagnostic,
			moves.size()
		);
		if (!candidate) return diagnostic.Code;
		const auto bytes = [](std::string_view text) {
			return std::max(text.size(), std::string{}.capacity()) + 1;
		};
		const auto rename = [&](std::string &text, std::string_view replacement) {
			if (text == replacement) return true;
			auto charge = candidate->Budget.Reserve(bytes(replacement));
			if (!charge) return false;
			const auto released = bytes(text);
			{
				std::string newText(replacement);
				text.swap(newText);
			}
			if (!candidate->Charge.Merge(std::move(*charge))) std::terminate();
			if (!candidate->Charge.Resize(candidate->Charge.Bytes() - released)) std::terminate();
			return true;
		};
		const size_t oldDetachedCount = candidate->DetachedAnimators.size();
		const auto renameAxes = [&](GroupSubtypeOverlay &overlay, std::string_view port) {
			if (!overlay.SeparatedVec2) return true;
			for (auto &axis : overlay.SeparatedVec2->Axes)
				for (auto &key : axis.Keys)
					if (!rename(key.Port, port)) return false;
			return rename(overlay.SeparatedVec2->Port, port);
		};
		for (const auto &move : moves) {
			if (!move.NewPort.empty() ||
				std::none_of(
					previous.Bindings().begin(), previous.Bindings().end(), [&](const auto &binding) {
						return ((binding.OwnerId == move.NodeId &&
								 detail::BindingAnimatorPort(binding) == move.OldPort) ||
								(binding.Axes.Storage != GroupAxisStorage::None &&
								 binding.Axes.OwnerId == move.NodeId && binding.Axes.Port == move.OldPort)) &&
							   !mapped(binding.NodeId, binding.Port).empty();
					}
				))
				continue;
			if (candidate->NextAnimatorId == UINT64_MAX)
				return fail(Status::LimitExceeded, "native retained animator identity space is exhausted");
			std::array<char, 64> identity{};
			constexpr std::string_view prefix = "native:animator:";
			std::copy(prefix.begin(), prefix.end(), identity.begin());
			const auto digits = std::to_chars(
				identity.data() + prefix.size(), identity.data() + identity.size(), candidate->NextAnimatorId
			);
			if (digits.ec != std::errc{})
				return fail(Status::LimitExceeded, "native retained animator identity exceeds bounds");
			const std::string_view id(identity.data(), size_t(digits.ptr - identity.data()));
			const auto *originalNode = nodeIn(original, move.NodeId);
			const auto physical = std::find_if(
				originalNode->DynamicInputs.begin(),
				originalNode->DynamicInputs.end(),
				[&](const auto &input) { return input.Id == move.OldPort; }
			);
			const auto track =
				std::find_if(original.Tracks.begin(), original.Tracks.end(), [&](const auto &item) {
					return item.NodeId == move.NodeId && item.Port == move.OldPort;
				});
			uint64_t metadataBytes = bytes(id) + bytes(move.NodeId) + bytes(move.OldPort);
			if (track != original.Tracks.end())
				metadataBytes += bytes(track->NodeId) + bytes(id) + bytes(track->End);
			auto metadataCharge = candidate->Budget.Reserve(metadataBytes);
			if (!metadataCharge)
				return fail(
					Status::LimitExceeded, "detached source animator metadata exceeds overlap bounds"
				);
			DetachedSourceAnimator metadata;
			metadata.Id = std::string(id);
			metadata.OwnerId = std::string(move.NodeId);
			metadata.OriginalPort = std::string(move.OldPort);
			metadata.Type = physical->Type;
			metadata.ArrayClassification =
				detail::AliasedSourceInput(*originalNode, move.OldPort)->SourceArrayClassification;
			if (originalNode->Type == "pc.hlsl" && move.OldPort.starts_with("argument_value_"))
				metadata.ArrayClassification = physical->Type == ValueType::Array;
			for (const auto &binding : previous.Bindings()) {
				if (binding.Axes.Storage != GroupAxisStorage::None && binding.Axes.OwnerId == move.NodeId &&
					binding.Axes.Port == move.OldPort)
					metadata.Writer = binding.Axes.Writer;
				if (binding.OwnerId == move.NodeId && detail::BindingAnimatorPort(binding) == move.OldPort) {
					metadata.Writer = binding.Writer;
					break;
				}
			}
			if (track != original.Tracks.end()) {
				metadata.Track = AnimationTrack{
					track->NodeId, std::string(id), track->End, track->LoopRange, track->QuaternionMode
				};
			}
			candidate->DetachedAnimators.push_back(std::move(metadata));
			++candidate->NextAnimatorId;
			if (!candidate->Charge.Merge(std::move(*metadataCharge))) std::terminate();
			auto overlay = std::find_if(
				candidate->SharedSubtypes.begin(), candidate->SharedSubtypes.end(), [&](const auto &item) {
					return item.NodeId == move.NodeId && item.Port == move.OldPort;
				}
			);
			if (overlay != candidate->SharedSubtypes.end()) {
				for (auto &key : overlay->Keys)
					if (!rename(key.Port, id))
						return fail(
							Status::LimitExceeded, "detached source key identity exceeds overlap bounds"
						);
				if (!rename(overlay->Port, id))
					return fail(
						Status::LimitExceeded, "detached source effect identity exceeds overlap bounds"
					);
			} else {
				uint64_t payloadBytes = bytes(move.NodeId) + bytes(id);
				size_t keyCount = 0;
				for (const auto &key : original.Keyframes)
					if (key.NodeId == move.NodeId && key.Port == move.OldPort) {
						const auto owned = KeyframePayloadBytes(key);
						if (!owned || *owned > UINT64_MAX - payloadBytes)
							return fail(Status::LimitExceeded, "detached source key payload exceeds bounds");
						payloadBytes += *owned;
						++keyCount;
					}
				const Value *fixed = nullptr;
				if (!keyCount) {
					const auto authored = std::find_if(
						originalNode->Values.begin(), originalNode->Values.end(), [&](const auto &value) {
							return value.Port == move.OldPort;
						}
					);
					fixed = authored != originalNode->Values.end() ? &authored->Data
							: physical->Default					   ? &*physical->Default
																   : nullptr;
					if (!fixed && !keyCount && !candidate->DetachedAnimators.back().ArrayClassification)
						return fail(
							Status::UnsupportedExecution,
							"retained empty source animator array classification is unresolved"
						);
					if (fixed) payloadBytes += detail::RetainedPayloadBytes(*fixed);
				}
				auto payloadCharge = candidate->Budget.Reserve(payloadBytes);
				if (!payloadCharge)
					return fail(
						Status::LimitExceeded, "detached source animator copy exceeds overlap bounds"
					);
				GroupSubtypeOverlay owned;
				owned.NodeId = std::string(move.NodeId);
				owned.Port = std::string(id);
				owned.Keys.reserve(keyCount);
				for (const auto &key : original.Keyframes)
					if (key.NodeId == move.NodeId && key.Port == move.OldPort) owned.Keys.push_back(key);
				if (fixed)
					owned.Fixed = *fixed;
				else if (!keyCount)
					owned.Fixed = candidate->DetachedAnimators.back().ArrayClassification.value_or(false)
									  ? Value{ArrayValue{ValueType::Scalar, {}}}
									  : Value{0.};
				candidate->SharedSubtypes.push_back(std::move(owned));
				if (!candidate->Charge.Merge(std::move(*payloadCharge))) std::terminate();
				overlay = std::prev(candidate->SharedSubtypes.end());
				for (auto &key : overlay->Keys)
					if (!rename(key.Port, id))
						return fail(
							Status::LimitExceeded, "detached source copied key identity exceeds bounds"
						);
			}
			if (!overlay->SeparatedVec2) {
				if (const auto *axes = detail::FindSeparatedVec2(*originalNode, move.OldPort)) {
					const auto payload = detail::SeparatedAnimatorBytes(*axes, false);
					auto charge = payload ? candidate->Budget.Reserve(*payload) : std::nullopt;
					if (!charge)
						return fail(Status::LimitExceeded, "detached source axes exceed overlap bounds");
					overlay->SeparatedVec2.emplace() = *axes;
					if (!candidate->Charge.Merge(std::move(*charge))) std::terminate();
				}
			}
			if (!renameAxes(*overlay, id))
				return fail(Status::LimitExceeded, "detached source axis names exceed overlap bounds");
		}
		uint64_t released = 0;
		for (auto &binding : candidate->Bindings) {
			const auto target = mapped(binding.NodeId, binding.Port);
			auto animator = mapped(binding.OwnerId, detail::BindingAnimatorPort(binding));
			if (animator.empty() && !target.empty())
				for (size_t i = oldDetachedCount; i < candidate->DetachedAnimators.size(); ++i) {
					const auto &detached = candidate->DetachedAnimators[i];
					if (detached.OwnerId == binding.OwnerId &&
						detached.OriginalPort == detail::BindingAnimatorPort(binding)) {
						animator = detached.Id;
						break;
					}
				}
			if (target.empty() || animator.empty()) {
				released += bytes(binding.NodeId) + bytes(binding.OwnerId) + bytes(binding.Port) +
							bytes(binding.AnimatorPort) + bytes(binding.Axes.OwnerId) +
							bytes(binding.Axes.Port) + bytes(binding.Axes.InstanceBase);
				binding.NodeId.clear();
				continue;
			}
			if (binding.Axes.Storage != GroupAxisStorage::None) {
				auto axes = mapped(binding.Axes.OwnerId, binding.Axes.Port);
				if (axes.empty())
					for (size_t i = oldDetachedCount; i < candidate->DetachedAnimators.size(); ++i) {
						const auto &detached = candidate->DetachedAnimators[i];
						if (detached.OwnerId == binding.Axes.OwnerId &&
							detached.OriginalPort == binding.Axes.Port) {
							axes = detached.Id;
							break;
						}
					}
				if (axes.empty())
					return fail(Status::InvalidGroup, "source axis identity was lost during input move");
				if (!rename(binding.Axes.Port, axes))
					return fail(Status::LimitExceeded, "source axis move names exceed live overlap bounds");
			}
			// Preserve the old writer independently when only its target moved.
			const auto retained = animator == target ? std::string_view{} : animator;
			if (!rename(binding.AnimatorPort, retained) || !rename(binding.Port, target))
				return fail(Status::LimitExceeded, "source input move names exceed live overlap bounds");
		}
		std::erase_if(candidate->Bindings, [](const auto &binding) { return binding.NodeId.empty(); });
		for (auto &overlay : candidate->SharedSubtypes) {
			const auto target = mapped(overlay.NodeId, overlay.Port);
			if (target.empty()) {
				released += bytes(overlay.NodeId) + bytes(overlay.Port);
				if (overlay.Fixed) released += detail::RetainedPayloadBytes(*overlay.Fixed);
				released += detail::SeparatedOverlayBytes(overlay);
				released += overlay.Keys.capacity() * sizeof(Keyframe);
				for (const auto &key : overlay.Keys)
					released += *KeyframePayloadBytes(key) - sizeof(Keyframe);
				overlay.NodeId.clear();
				continue;
			}
			// The mapped view may refer to the old port. Change key names before it.
			for (auto &key : overlay.Keys)
				if (!rename(key.Port, target))
					return fail(
						Status::LimitExceeded, "source input move key names exceed live overlap bounds"
					);
			if (!renameAxes(overlay, target))
				return fail(Status::LimitExceeded, "source input move axis names exceed overlap bounds");
			if (!rename(overlay.Port, target))
				return fail(
					Status::LimitExceeded, "source input move overlay names exceed live overlap bounds"
				);
		}
		std::erase_if(candidate->SharedSubtypes, [](const auto &overlay) { return overlay.NodeId.empty(); });
		// Vector slots remain charged after erase; names and nested payloads are destroyed.
		if (!candidate->Charge.Resize(candidate->Charge.Bytes() - released)) std::terminate();
		if (!RetireUnreferencedDetachedAnimators(*candidate)) {
			diagnostic = {
				Status::LimitExceeded, {}, {}, "detached animator reference checks exceed work bounds"
			};
			return diagnostic.Code;
		}
		detail::GroupReplayAccess::Install(result, std::move(candidate));
		diagnostic = {};
		return Status::Ok;
	} catch (const std::bad_alloc &) {
		diagnostic = {Status::LimitExceeded, {}, {}, "source input move allocation was refused"};
		return diagnostic.Code;
	}
	Status RebindGroupReplayWithAnimatorReplacements(
		const Document &document,
		std::span<const SourceAnimatorReplacement> replacements,
		const GroupReplayState &previous,
		uint64_t revision,
		GroupReplayState &result,
		Diagnostic &diagnostic,
		uint64_t maximumBytes
	) try {
		const auto refuse =
			[&](Status code, std::string message, std::string_view node = {}, std::string_view port = {}) {
				diagnostic = {code, std::string(node), std::string(port), std::move(message)};
				return code;
			};
		if (document.FormatVersion < 8 || replacements.size() > Limits::MaximumArrayElements)
			return refuse(
				Status::LimitExceeded, "source animator replacement format or count exceeds bounds"
			);
		if (!AdmitGroupReplayDocument(document, previous, result, maximumBytes, diagnostic))
			return diagnostic.Code;
		uint64_t work = 0;
		for (size_t index = 0; index < replacements.size(); ++index) {
			const auto &target = replacements[index];
			const uint64_t visits = index + document.Nodes.size() + document.Keyframes.size();
			if (visits > 64'000'000 - work)
				return refuse(
					Status::LimitExceeded,
					"source animator replacement exceeds work bound",
					target.NodeId,
					target.Port
				);
			work += visits;
			if (target.NodeId.empty() || target.Port.empty() ||
				target.NodeId.size() > Limits::MaximumTextBytes ||
				target.Port.size() > Limits::MaximumTextBytes)
				return refuse(
					Status::InvalidValue,
					"source animator replacement needs bounded physical socket names",
					target.NodeId,
					target.Port
				);
			for (size_t prior = 0; prior < index; ++prior)
				if (replacements[prior].NodeId == target.NodeId && replacements[prior].Port == target.Port)
					return refuse(
						Status::DuplicateId,
						"source animator replacement target is duplicated",
						target.NodeId,
						target.Port
					);
			const auto node =
				std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const auto &item) {
					return item.Id == target.NodeId;
				});
			if (node == document.Nodes.end() || node->Type == "pc.group_input" ||
				!detail::AliasedSourceInput(*node, target.Port))
				return refuse(
					Status::UnknownPort,
					"source animator replacement needs an ordinary physical source input",
					target.NodeId,
					target.Port
				);
			const Keyframe *replacement = nullptr;
			for (const auto &key : document.Keyframes) {
				if (key.NodeId != target.NodeId || key.Port != target.Port) continue;
				if (replacement)
					return refuse(
						Status::InvalidValue,
						"source animator replacement needs one fresh frame-zero key",
						target.NodeId,
						target.Port
					);
				replacement = &key;
			}
			if (!replacement || replacement->Tick || replacement->Subframe != 0 ||
				replacement->NegativeFrame || replacement->Kind != KeyframeKind::Normal ||
				replacement->Interpolation != "source" || replacement->SineDriver ||
				replacement->SourceDriver || !detail::ValidRuntimeValue(replacement->Data))
				return refuse(
					Status::InvalidValue,
					"source animator replacement needs one fresh frame-zero key",
					target.NodeId,
					target.Port
				);
			const auto validSide = [](std::string_view type) {
				return type == "linear" || type == "bezier" || type == "cut";
			};
			if (!replacement->Ease || !validSide(replacement->Ease->InType) ||
				!validSide(replacement->Ease->OutType) || !std::isfinite(replacement->Ease->In.X) ||
				!std::isfinite(replacement->Ease->In.Y) || !std::isfinite(replacement->Ease->Out.X) ||
				!std::isfinite(replacement->Ease->Out.Y) ||
				replacement->SourceKeyId.size() > Limits::MaximumSourceKeyIdBytes)
				return refuse(
					Status::InvalidValue,
					"source animator replacement needs finite registered easing and bounded record identity",
					target.NodeId,
					target.Port
				);
		}
		// Rebind's admission still includes the old owner and borrowed document.
		// A separate destination remains resident until this candidate is published.
		const uint64_t destinationBytes = &previous == &result ? 0 : result.RetainedBytes();
		GroupReplayState candidate;
		const auto status = RebindGroupReplay(
			document, previous, revision, candidate, diagnostic, maximumBytes - destinationBytes
		);
		if (status != Status::Ok) return status;
		auto *owner = detail::GroupReplayAccess::Get(candidate);
		if (owner) {
			uint64_t removed = 0;
			const auto selected = [&](const GroupSubtypeOverlay &overlay) {
				return std::any_of(replacements.begin(), replacements.end(), [&](const auto &target) {
					return target.NodeId == overlay.NodeId && target.Port == overlay.Port;
				});
			};
			const uint64_t selectionWork = 2 * uint64_t(owner->SharedSubtypes.size()) * replacements.size();
			if (selectionWork > 64'000'000 - work)
				return refuse(Status::LimitExceeded, "source animator replacement exceeds work bound");
			for (auto &overlay : owner->SharedSubtypes) {
				if (!selected(overlay)) continue;
				if (!overlay.SeparatedVec2)
					removed += overlay.NodeId.capacity() + 1 + overlay.Port.capacity() + 1;
				if (overlay.Fixed) removed += detail::RetainedPayloadBytes(*overlay.Fixed);
				removed += overlay.Keys.capacity() * sizeof(Keyframe);
				for (const auto &key : overlay.Keys)
					removed += *KeyframePayloadBytes(key) - sizeof(Keyframe);
				overlay.Fixed.reset();
				std::vector<Keyframe>{}.swap(overlay.Keys);
			}
			// The replaced unsplit animator is dormant while separate axes are enabled.
			std::erase_if(owner->SharedSubtypes, [&](const auto &overlay) {
				return selected(overlay) && !overlay.SeparatedVec2;
			});
			// The overlay-vector capacity survives erasure; only destroyed payloads leave its charge.
			if (!owner->Charge.Resize(owner->Charge.Bytes() - removed)) std::terminate();
		}
		result = std::move(candidate);
		diagnostic = {};
		return Status::Ok;
	} catch (const std::bad_alloc &) {
		diagnostic = {Status::LimitExceeded, {}, {}, "source animator replacement allocation failed"};
		return diagnostic.Code;
	}
	static Status BootstrapGroupReplay(
		const Document &document,
		const Plan &plan,
		std::span<const GroupBootstrapTarget> order,
		const EvaluationRequest &clock,
		const GroupReplayState &previous,
		uint64_t revision,
		GroupReplayState &result,
		Diagnostic &diagnostic,
		uint64_t maximumBytes,
		bool restoreOnly
	) try {
		const auto fail = [&](Status code, std::string_view message) {
			diagnostic = {code, {}, {}, std::string(message)};
			return code;
		};
		if (!maximumBytes || maximumBytes > Limits::MaximumEvaluationBytes)
			return fail(Status::LimitExceeded, "group bootstrap budget is outside the bounded domain");
		if (!ValidFrameTime(GetFrameTime(clock)))
			return fail(Status::InvalidValue, "group bootstrap requires a valid signed caller clock");
		if (order.size() > Limits::MaximumNodes)
			return fail(Status::LimitExceeded, "group bootstrap callback count exceeds the node bound");
		uint64_t bytes = order.size() * sizeof(GroupRefreshEvent);
		for (const auto &target : order) {
			if (target.NodeId.empty() || target.NodeId.size() > Limits::MaximumTextBytes ||
				(target.SubtypeAnimator != GroupSubtypeAnimator::Static &&
				 target.SubtypeAnimator != GroupSubtypeAnimator::Animated))
				return fail(Status::InvalidValue, "group bootstrap callback identity or animator is invalid");
			const uint64_t nameBytes = std::max(target.NodeId.size(), std::string{}.capacity()) + 1;
			if (nameBytes > maximumBytes || bytes > maximumBytes - nameBytes)
				return fail(
					Status::LimitExceeded, "group bootstrap callback storage exceeds operation budget"
				);
			bytes += nameBytes;
		}
		if (bytes >= maximumBytes)
			return fail(Status::LimitExceeded, "group bootstrap leaves no budget for replay state");
		// Callback storage overlaps both previous states and the replay candidate.
		// Lower the replay admission while these owned names remain alive, rather
		// than giving each phase the full cap.
		detail::EvaluationBudget scratch{maximumBytes};
		auto charge = scratch.Reserve(bytes);
		if (!charge) return fail(Status::LimitExceeded, "group bootstrap callback admission failed");
		std::vector<GroupRefreshEvent> events;
		events.reserve(order.size());
		for (const auto &target : order) {
			GroupRefreshEvent event;
			event.NodeId = std::string(target.NodeId);
			event.At = clock;
			event.Reason = restoreOnly ? GroupRefreshReason::Restore : GroupRefreshReason::Load;
			event.SubtypeAnimator = target.SubtypeAnimator;
			events.push_back(std::move(event));
		}
		return ReplayGroupRefresh(
			document, plan, events, previous, revision, result, diagnostic, maximumBytes - bytes
		);
	} catch (const std::bad_alloc &) {
		diagnostic = {Status::LimitExceeded, {}, {}, "group bootstrap allocation failed"};
		return diagnostic.Code;
	}
	Status ReplayGroupBootstrap(
		const Document &document,
		const Plan &plan,
		std::span<const GroupBootstrapTarget> order,
		const EvaluationRequest &clock,
		const GroupReplayState &previous,
		uint64_t revision,
		GroupReplayState &result,
		Diagnostic &diagnostic,
		uint64_t maximumBytes
	) {
		return BootstrapGroupReplay(
			document, plan, order, clock, previous, revision, result, diagnostic, maximumBytes, false
		);
	}
	Status RestoreGroupDeclarations(
		const Document &document,
		const Plan &plan,
		std::span<const GroupBootstrapTarget> order,
		const EvaluationRequest &clock,
		const GroupReplayState &previous,
		uint64_t revision,
		GroupReplayState &result,
		Diagnostic &diagnostic,
		uint64_t maximumBytes
	) {
		return BootstrapGroupReplay(
			document, plan, order, clock, previous, revision, result, diagnostic, maximumBytes, true
		);
	}

	namespace detail {
		namespace {
			uint64_t TextBytes(std::string_view text) {
				return std::max<size_t>(text.size(), 15) + 1;
			}
			bool Add(uint64_t &bytes, uint64_t extra) {
				if (extra > UINT64_MAX - bytes) return false;
				bytes += extra;
				return true;
			}
			bool Choice(NodeContext &context, std::string_view port, int64_t &result) {
				const auto *value = context.Find(port);
				const auto number = value ? SourceChoiceNumber(*value) : std::optional<double>{};
				if (!number || !std::isfinite(*number) || *number != std::trunc(*number) || *number < 0 ||
					*number >= 0x1p63)
					return context.Fail(
						Status::UnsupportedExecution,
						"group refresh requires an exact represented control index",
						port
					);
				result = static_cast<int64_t>(*number);
				return true;
			}
			std::optional<size_t> ArrayLength(const Value &value) {
				return std::visit(
					[](const auto &item) -> std::optional<size_t> {
						using T = std::decay_t<decltype(item)>;
						if constexpr (std::is_same_v<T, ArrayValue>)
							return !item.Items.empty()	 ? item.Items.size()
								   : item.Nested.empty() ? item.Elements.size()
														 : item.Nested.size();
						else if constexpr (std::is_same_v<T, Vector2>)
							return 2;
						else if constexpr (std::is_same_v<T, Vector3>)
							return 3;
						else if constexpr (std::is_same_v<T, Vector4> || std::is_same_v<T, Quaternion>)
							return 4;
						else if constexpr (std::is_same_v<T, Area>)
							return 6;
						else if constexpr (std::is_same_v<T, Curve>)
							return item.Anchors.size() + 1;
						else
							return std::nullopt;
					},
					value
				);
			}
			bool ReplaceParent(
				NodeContext &context,
				GroupReplayAccess::Owner &owner,
				GroupReplayEntry &entry,
				Value replacement,
				AllocationReservation lease,
				bool sourceAnimator = false
			) {
				uint64_t oldBytes = entry.ParentReset ? RetainedPayloadBytes(*entry.ParentReset) : 0;
				oldBytes += entry.ParentKeys.capacity() * sizeof(Keyframe);
				for (const auto &key : entry.ParentKeys)
					oldBytes += *KeyframePayloadBytes(key) - sizeof(Keyframe);
				if (!ValidRuntimeValue(replacement))
					return context.Fail(
						Status::InvalidValue, "group animator reset has an invalid payload", "parent_value"
					);
				AllocationReservation keyCharge;
				std::vector<Keyframe> replacementKeys;
				if (sourceAnimator) {
					auto extra = owner.Budget.Reserve(
						sizeof(Keyframe) + TextBytes(entry.NodeId) + TextBytes("parent_value") +
						TextBytes("source") + TextBytes("linear") * 2
					);
					if (!extra)
						return context.Fail(
							Status::LimitExceeded, "group reset source key exceeds budget", "parent_value"
						);
					keyCharge = std::move(*extra);
					Keyframe key;
					key.NodeId = std::string(entry.NodeId);
					key.Port = "parent_value";
					key.Interpolation = "source";
					key.Ease = KeyframeEase{};
					key.Data = std::move(replacement);
					replacementKeys.reserve(1);
					replacementKeys.push_back(std::move(key));
					entry.ParentReset.reset();
				} else {
					entry.ParentReset = std::move(replacement);
				}
				entry.ParentKeys.swap(replacementKeys);
				std::vector<Keyframe>{}.swap(replacementKeys);
				if (sourceAnimator && !owner.Charge.Merge(std::move(keyCharge))) std::terminate();
				if (!owner.Charge.Merge(std::move(lease)) ||
					!owner.Charge.Resize(owner.Charge.Bytes() - oldBytes))
					std::terminate();
				return true;
			}
			bool ResetArray(
				NodeContext &context,
				GroupReplayAccess::Owner &owner,
				GroupReplayEntry &entry,
				size_t count,
				bool palette = false,
				bool sourceAnimator = false
			) {
				auto lease = owner.Budget.Reserve(count * sizeof(ElementValue));
				if (!lease)
					return context.Fail(
						Status::LimitExceeded,
						"group local animator reset exceeds the operation budget",
						"parent_value"
					);
				ArrayValue replacement;
				replacement.ElementType = palette ? ValueType::Colour : ValueType::Scalar;
				replacement.Elements.reserve(count);
				for (size_t index = 0; index < count; ++index)
					replacement.Elements.emplace_back(
						palette ? ElementValue{Colour{0, 0, 0, 255}} : ElementValue{0.0}
					);
				return ReplaceParent(
					context, owner, entry, std::move(replacement), std::move(*lease), sourceAnimator
				);
			}
			bool ResetScalar(
				NodeContext &context,
				GroupReplayAccess::Owner &owner,
				GroupReplayEntry &entry,
				double value = 0,
				bool sourceAnimator = false
			) {
				auto lease = owner.Budget.Reserve(0);
				return ReplaceParent(context, owner, entry, value, std::move(*lease), sourceAnimator);
			}
			uint64_t KeyBytes(const std::vector<Keyframe> &keys) {
				uint64_t bytes = keys.capacity() * sizeof(Keyframe);
				for (const auto &key : keys)
					bytes += *KeyframePayloadBytes(key) - sizeof(Keyframe);
				return bytes;
			}
			bool EditAnimator(
				NodeContext &context,
				const GroupRefreshEvent &event,
				GroupReplayAccess::Owner &owner,
				std::string_view nodeId,
				std::vector<Keyframe> &keys,
				std::optional<Value> &fixed,
				const Document &document,
				std::string_view port,
				const Value &value,
				bool animated,
				bool preserveStaticKeys = false,
				bool trigger = false,
				bool ignoreDocumentKeys = false
			) {

				const auto retainedKeys = [&]() -> uint64_t {
					if (!ignoreDocumentKeys) return KeyBytes(keys);
					uint64_t bytes = keys.capacity() * sizeof(Keyframe);
					for (const auto &key : keys) {
						const auto owned = SeparatedKeyBytes(key, true);
						if (!owned || !Add(bytes, *owned - sizeof(Keyframe))) return UINT64_MAX;
					}
					return bytes;
				};
				if (!ValidRuntimeValue(value))
					return context.Fail(
						Status::InvalidValue, "group local edit requires a represented finite value", port
					);
				if (!animated && preserveStaticKeys && !fixed) {
					EvaluationVector<const Keyframe *> source{
						EvaluationAllocator<const Keyframe *>{owner.Budget}
					};
					if (!keys.empty()) {
						source.reserve(keys.size());
						for (const auto &key : keys)
							source.push_back(&key);
					} else if (!ignoreDocumentKeys) {
						const auto count = std::count_if(
							document.Keyframes.begin(), document.Keyframes.end(), [&](const auto &key) {
								return key.NodeId == nodeId && key.Port == port;
							}
						);
						source.reserve(count);
						for (const auto &key : document.Keyframes)
							if (key.NodeId == nodeId && key.Port == port) source.push_back(&key);
					}
					if (!source.empty()) {
						uint64_t bytes = source.size() * sizeof(Keyframe);
						for (const auto *key : source) {
							const auto size = ignoreDocumentKeys ? SeparatedKeyBytes(*key, false)
																 : KeyframePayloadBytes(*key);
							if (!size || !Add(bytes, *size - sizeof(Keyframe)))
								return context.Fail(
									Status::LimitExceeded,
									"Group static owner key payload exceeds bounds",
									port
								);
						}
						if (!Add(bytes, RetainedPayloadBytes(value)))
							return context.Fail(
								Status::LimitExceeded, "Group static owner replacement overflows", port
							);
						auto lease = owner.Budget.Reserve(bytes);
						if (!lease)
							return context.Fail(
								Status::LimitExceeded, "Group static owner replacement exceeds budget", port
							);
						Value replacementValue = value;
						std::vector<Keyframe> replacement;
						replacement.reserve(source.size());
						for (const auto *key : source)
							replacement.push_back(*key);
						replacement.front().Data = std::move(replacementValue);
						const uint64_t old = retainedKeys();
						keys.swap(replacement);
						std::vector<Keyframe>{}.swap(replacement);
						if (!owner.Charge.Merge(std::move(*lease)) ||
							!owner.Charge.Resize(owner.Charge.Bytes() - old))
							std::terminate();
						return true;
					}
				}
				if (!animated) {
					auto lease = owner.Budget.Reserve(RetainedPayloadBytes(value));
					if (!lease)
						return context.Fail(
							Status::LimitExceeded, "group local edit exceeds the live operation budget", port
						);
					Value replacement = value;
					const uint64_t old = retainedKeys() + (fixed ? RetainedPayloadBytes(*fixed) : 0);
					fixed = std::move(replacement);
					std::vector<Keyframe>{}.swap(keys);
					if (!owner.Charge.Merge(std::move(*lease)) ||
						!owner.Charge.Resize(owner.Charge.Bytes() - old))
						std::terminate();
					return true;
				}
				EvaluationVector<const Keyframe *> source{
					EvaluationAllocator<const Keyframe *>{owner.Budget}
				};
				AllocationReservation seedCharge;
				Keyframe seed;
				// A destroyed animator contributes only its reset value, never its original
				// keys.
				if (fixed && !trigger) {
					auto seedLease = owner.Budget.Reserve(
						sizeof(Keyframe) + TextBytes(nodeId) + TextBytes(port) + TextBytes("source") +
						TextBytes("linear") * 2 + RetainedPayloadBytes(*fixed)
					);
					if (!seedLease)
						return context.Fail(
							Status::LimitExceeded, "group reset key exceeds the live operation budget", port
						);
					seedCharge = std::move(*seedLease);
					seed.NodeId = std::string(nodeId);
					seed.Port = std::string(port);
					seed.Interpolation = "source";
					seed.Ease = KeyframeEase{};
					seed.Data = *fixed;
					source.push_back(&seed);
				} else if (!keys.empty()) {
					source.reserve(keys.size());
					for (const auto &key : keys)
						source.push_back(&key);
				} else if (!fixed && !ignoreDocumentKeys) {
					const auto count = std::count_if(
						document.Keyframes.begin(), document.Keyframes.end(), [&](const auto &key) {
							return key.NodeId == nodeId && key.Port == port;
						}
					);
					source.reserve(count);
					for (const auto &key : document.Keyframes)
						if (key.NodeId == nodeId && key.Port == port) source.push_back(&key);
				}
				const auto time = GetFrameTime(event.At);
				const auto existing = std::find_if(source.begin(), source.end(), [&](const auto *key) {
					return CompareFrameTime(GetFrameTime(*key), time) == 0;
				});
				if (existing != source.end() && !event.ReplaceExistingKey) return true;
				const size_t count = source.size() + (existing == source.end());
				if (count > Limits::MaximumKeyframes)
					return context.Fail(
						Status::LimitExceeded, "group local overlay exceeds the key count bound", port
					);
				uint64_t bytes = count * sizeof(Keyframe);
				for (const auto *key : source) {
					const auto cloneBytes =
						ignoreDocumentKeys ? SeparatedKeyBytes(*key, false) : KeyframePayloadBytes(*key);
					if (!cloneBytes || !Add(bytes, *cloneBytes - sizeof(Keyframe)))
						return context.Fail(
							Status::LimitExceeded, "group local key payload exceeds bounds", port
						);
				}
				// Replacing Data overlaps its copied old payload. Admit both before
				// constructing the clone.
				if (!Add(bytes, RetainedPayloadBytes(value)))
					return context.Fail(Status::LimitExceeded, "group local key data overflows", port);
				if (existing == source.end() &&
					!Add(
						bytes,
						TextBytes(nodeId) + TextBytes(port) + TextBytes("source") + TextBytes("linear") * 2
					))
					return context.Fail(Status::LimitExceeded, "group local key names overflow", port);
				auto lease = owner.Budget.Reserve(bytes);
				if (!lease)
					return context.Fail(
						Status::LimitExceeded,
						"group local replacement exceeds the live operation budget",
						port
					);
				// The scalar source setter sorts and removes equal-time records. The bounded
				// native profile preserves physical order for ties; source runner tie order is unverified.
				auto sorting = owner.Budget.Reserve(ignoreDocumentKeys ? count * sizeof(Keyframe) : 0);
				if (!sorting)
					return context.Fail(Status::LimitExceeded, "split key sorting exceeds live budget", port);
				Value edited = value;
				std::vector<Keyframe> replacement;
				replacement.reserve(count);
				for (const auto *key : source) {
					replacement.push_back(*key);
					if (key == (existing == source.end() ? nullptr : *existing))
						replacement.back().Data = std::move(edited);
				}
				if (existing == source.end()) {
					Keyframe key;
					key.NodeId = std::string(nodeId);
					key.Port = std::string(port);
					key.Data = std::move(edited);
					key.Interpolation = "source";
					key.Ease = KeyframeEase{};
					if (!SetFrameTime(key, time))
						return context.Fail(
							Status::InvalidValue, "group local edit time is outside the bounded clock", port
						);
					replacement.push_back(std::move(key));
				}
				const auto beforeKey = [](const auto &left, const auto &right) {
					return CompareFrameTime(GetFrameTime(left), GetFrameTime(right)) < 0;
				};
				if (ignoreDocumentKeys) {
					std::stable_sort(replacement.begin(), replacement.end(), beforeKey);
					replacement.erase(
						std::unique(
							replacement.begin(),
							replacement.end(),
							[](const auto &left, const auto &right) {
								return GetFrameTime(left) == GetFrameTime(right);
							}
						),
						replacement.end()
					);
				} else
					std::sort(replacement.begin(), replacement.end(), beforeKey);
				const uint64_t oldBytes = retainedKeys() + (fixed ? RetainedPayloadBytes(*fixed) : 0);
				keys.swap(replacement);
				replacement.clear();
				std::vector<Keyframe>{}.swap(replacement);
				fixed.reset();
				if (!owner.Charge.Merge(std::move(*lease)) ||
					!owner.Charge.Resize(owner.Charge.Bytes() - oldBytes))
					std::terminate();
				return true;
			}

			bool EditLocalAnimator(
				NodeContext &context,
				const GroupRefreshEvent &event,
				GroupReplayAccess::Owner &owner,
				GroupReplayEntry &entry,
				const Document &document,
				std::string_view port,
				const Value &value,
				bool animated
			) {
				const auto targetNode =
					std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const auto &node) {
						return node.Id == entry.NodeId;
					});
				const auto *catalogue =
					targetNode == document.Nodes.end() ? nullptr : FindCatalogueEntry(targetNode->Type);
				const auto *sourceInput = catalogue ? FindCatalogueInput(*catalogue, port) : nullptr;
				const bool trigger =
					(port == "parent_value" && entry.Domain.Kind == SourceSocketKind::Trigger) ||
					(sourceInput && sourceInput->SourceKind == "Trigger");
				if (trigger && !std::holds_alternative<bool>(value))
					return context.Fail(
						Status::InvalidValue, "Trigger edits require a boolean and integral frame", port
					);
				if (trigger && (GetFrameTime(event.At).Subframe != 0 || GetFrameTime(event.At).NegativeFrame))
					return context.Fail(
						Status::UnsupportedExecution,
						"source Trigger map requires a nonnegative integer frame",
						port
					);
				if (trigger && !animated) {
					auto atZero = event;
					(void)SetFrameTime(atZero.At, {});
					return EditAnimator(
						context,
						atZero,
						owner,
						entry.NodeId,
						entry.ParentKeys,
						entry.ParentReset,
						document,
						port,
						value,
						true,
						false,
						true
					);
				}
				if (port != "parent_value" && owner.Bound) {
					std::string_view sourceId = entry.NodeId;
					std::string_view sourcePort = port;
					for (const auto &binding : owner.Bindings)
						if (binding.NodeId == entry.NodeId && binding.Port == port) {
							sourceId = binding.OwnerId;
							sourcePort = BindingAnimatorPort(binding);
							animated = binding.Writer == GroupSubtypeAnimator::Animated;
							break;
						}
					for (const auto &binding : owner.Bindings)
						if (binding.OwnerId == sourceId && BindingAnimatorPort(binding) == sourcePort) {
							animated = binding.Writer == GroupSubtypeAnimator::Animated;
							break;
						}
					auto found = std::find_if(
						owner.SharedSubtypes.begin(), owner.SharedSubtypes.end(), [&](const auto &overlay) {
							return overlay.NodeId == sourceId && overlay.Port == sourcePort;
						}
					);
					if (found == owner.SharedSubtypes.end()) {
						auto nameCharge = owner.Budget.Reserve(TextBytes(sourceId) + TextBytes(sourcePort));
						if (!nameCharge || owner.SharedSubtypes.size() == owner.SharedSubtypes.capacity())
							return context.Fail(
								Status::LimitExceeded,
								"Group shared animator owner exceeds operation budget",
								port
							);
						GroupSubtypeOverlay overlay;
						overlay.NodeId = std::string(sourceId);
						overlay.Port = std::string(sourcePort);
						owner.SharedSubtypes.push_back(std::move(overlay));
						if (!owner.Charge.Merge(std::move(*nameCharge))) std::terminate();
						found = std::prev(owner.SharedSubtypes.end());
					}
					return EditAnimator(
						context,
						event,
						owner,
						found->NodeId,
						found->Keys,
						found->Fixed,
						document,
						sourcePort,
						value,
						animated,
						true
					);
				}
				return EditAnimator(
					context,
					event,
					owner,
					entry.NodeId,
					port == "subtype" ? entry.SubtypeKeys : entry.ParentKeys,
					port == "subtype" ? entry.SubtypeStatic : entry.ParentReset,
					document,
					port,
					value,
					animated,
					true,
					trigger
				);
			}

		} // namespace
		std::unique_ptr<GroupReplayAccess::Owner> CloneGroupReplay(
			const GroupReplayState &previous,
			size_t extraSlots,
			uint64_t revision,
			uint64_t maximumBytes,
			uint64_t destinationBytes,
			Diagnostic &diagnostic,
			size_t extraDetachedSlots
		) try {
			const auto refuse = [&]() -> std::unique_ptr<GroupReplayAccess::Owner> {
				diagnostic = {
					Status::LimitExceeded, {}, {}, "group replay state exceeds the live operation budget"
				};
				return {};
			};
			if (!maximumBytes || maximumBytes > Limits::MaximumEvaluationBytes ||
				extraSlots > Limits::MaximumNodes * Limits::MaximumArrayElements)
				return refuse();
			const size_t slots = std::min(Limits::MaximumNodes, previous.Entries().size() + extraSlots);
			uint64_t bytes = sizeof(GroupReplayAccess::Owner) + slots * sizeof(GroupReplayEntry);
			for (const auto &entry : previous.Entries()) {
				if (entry.NodeId.size() > Limits::MaximumTextBytes || !Add(bytes, TextBytes(entry.NodeId)))
					return refuse();
				for (const auto *value : {&entry.ParentReset, &entry.SubtypeStatic})
					if (*value && (!ValidRuntimeValue(**value) || !Add(bytes, RetainedPayloadBytes(**value))))
						return refuse();
				for (const auto *keys : {&entry.SubtypeKeys, &entry.ParentKeys}) {
					if (!Add(bytes, keys->size() * sizeof(Keyframe))) return refuse();
					for (const auto &key : *keys) {
						const auto size = KeyframePayloadBytes(key);
						if (!size || !Add(bytes, *size - sizeof(Keyframe))) return refuse();
					}
				}
			}
			const size_t sharedSlots = previous.SharedSubtypes().size() + extraSlots;
			if (!Add(bytes, sharedSlots * sizeof(GroupSubtypeOverlay)) ||
				!Add(bytes, previous.Bindings().size() * sizeof(GroupSubtypeBinding)))
				return refuse();
			for (const auto &binding : previous.Bindings())
				if (!Add(bytes, TextBytes(binding.NodeId)) || !Add(bytes, TextBytes(binding.OwnerId)) ||
					!Add(bytes, TextBytes(binding.Port)) || !Add(bytes, TextBytes(binding.AnimatorPort)) ||
					!Add(bytes, TextBytes(binding.Axes.OwnerId)) ||
					!Add(bytes, TextBytes(binding.Axes.Port)) ||
					!Add(bytes, TextBytes(binding.Axes.InstanceBase)))
					return refuse();
			for (const auto &overlay : previous.SharedSubtypes()) {
				if (overlay.SeparatedVec2) {
					const auto payload = SeparatedAnimatorBytes(*overlay.SeparatedVec2, false);
					if (!payload || !Add(bytes, *payload) || overlay.SeparatedVec2->Port != overlay.Port)
						return refuse();
					for (const auto &axis : overlay.SeparatedVec2->Axes)
						for (const auto &key : axis.Keys)
							if (key.NodeId != overlay.NodeId || key.Port != overlay.Port) return refuse();
				}
				if (!Add(bytes, TextBytes(overlay.NodeId)) || !Add(bytes, TextBytes(overlay.Port)) ||
					(overlay.Fixed && !Add(bytes, RetainedPayloadBytes(*overlay.Fixed))) ||
					!Add(bytes, overlay.Keys.size() * sizeof(Keyframe)))
					return refuse();
				for (const auto &key : overlay.Keys) {
					const auto size = KeyframePayloadBytes(key);
					if (!size || !Add(bytes, *size - sizeof(Keyframe))) return refuse();
				}
			}
			if (extraDetachedSlots > Limits::MaximumArrayElements ||
				previous.DetachedAnimators().size() > Limits::MaximumArrayElements - extraDetachedSlots ||
				!Add(
					bytes,
					(previous.DetachedAnimators().size() + extraDetachedSlots) *
						sizeof(DetachedSourceAnimator)
				))
				return refuse();
			for (const auto &animator : previous.DetachedAnimators())
				if (!Add(bytes, DetachedAnimatorBytes(animator))) return refuse();
			if (bytes > maximumBytes || previous.RetainedBytes() > maximumBytes - bytes ||
				destinationBytes > maximumBytes - bytes - previous.RetainedBytes())
				return refuse();
			auto owner = std::make_unique<GroupReplayAccess::Owner>(maximumBytes);
			auto shadow = owner->Budget.Reserve(previous.RetainedBytes() + destinationBytes);
			auto charge = owner->Budget.Reserve(bytes);
			if (!shadow || !charge) return refuse();
			owner->PreviousShadow = std::move(*shadow);
			owner->Charge = std::move(*charge);
			owner->Revision = revision;
			owner->Bound = previous.InstancesBound();
			owner->Bindings.assign(previous.Bindings().begin(), previous.Bindings().end());
			if (const auto *old = GroupReplayAccess::Get(previous))
				owner->NextAnimatorId = old->NextAnimatorId;
			owner->DetachedAnimators.reserve(previous.DetachedAnimators().size() + extraDetachedSlots);
			for (const auto &animator : previous.DetachedAnimators())
				owner->DetachedAnimators.push_back(animator);
			owner->SharedSubtypes.reserve(sharedSlots);
			for (const auto &overlay : previous.SharedSubtypes())
				owner->SharedSubtypes.push_back(overlay);
			owner->Entries.reserve(slots);
			for (const auto &entry : previous.Entries())
				owner->Entries.push_back(entry);
			return owner;
		} catch (const std::bad_alloc &) {
			diagnostic = {Status::LimitExceeded, {}, {}, "group replay allocation was refused"};
			return {};
		}
		bool ApplyGroupRefreshContext(
			NodeContext &context,
			const GroupRefreshEvent &event,
			GroupReplayAccess::Owner &owner,
			const Document &document
		) {
			int64_t type, subtype, size;
			if (!Choice(context, "input_type", type) || !Choice(context, "subtype", subtype) ||
				!Choice(context, "vector_size", size))
				return false;
			auto found = std::find_if(owner.Entries.begin(), owner.Entries.end(), [&](const auto &entry) {
				return entry.NodeId == event.NodeId;
			});
			const bool fresh = found == owner.Entries.end();
			const bool typeChanged = fresh || found->InputType != type;
			if (event.Reason == GroupRefreshReason::ParentEdit) {
				if (fresh || !event.LocalValue)
					return context.Fail(
						Status::InvalidValue,
						"a local parent edit requires a loaded boundary and "
						"its actual edited value",
						"parent_value"
					);
				if (!EditLocalAnimator(
						context,
						event,
						owner,
						*found,
						document,
						"parent_value",
						*event.LocalValue,
						event.LocalAnimated
					))
					return false;
			}
			if (!fresh && event.EditedPort == "subtype" && event.LocalValue &&
				!EditLocalAnimator(
					context,
					event,
					owner,
					*found,
					document,
					"subtype",
					*event.LocalValue,
					event.SubtypeAnimator == GroupSubtypeAnimator::Animated
				))
				return false;
			if (!fresh && !typeChanged && found->Subtype == subtype &&
				event.Reason != GroupRefreshReason::Restore)
				return true;
			if (typeChanged && event.Reason != GroupRefreshReason::Load &&
				event.Reason != GroupRefreshReason::Restore)
				subtype = 0;
			SourceSocketDomain domain;
			if (!ResolveGroupSocketDomain(type, subtype, size, domain))
				return context.Fail(
					Status::InvalidValue, "group refresh choices exceed the source domain", "input_type"
				);
			if (fresh) {
				if (owner.Entries.size() == Limits::MaximumNodes)
					return context.Fail(Status::LimitExceeded, "group replay count exceeds node bound");
				auto nameCharge = owner.Budget.Reserve(TextBytes(event.NodeId));
				if (!nameCharge || !owner.Charge.Merge(std::move(*nameCharge)))
					return context.Fail(
						Status::LimitExceeded, "group replay node name exceeds operation budget"
					);
				GroupReplayEntry entry;
				entry.NodeId = std::string(event.NodeId);
				owner.Entries.push_back(std::move(entry));
				found = std::prev(owner.Entries.end());
			}
			GroupReplayEntry &entry = *found;
			if (event.Reason == GroupRefreshReason::Restore) {
				entry.InputType = type;
				entry.Subtype = subtype;
				entry.VectorSize = size;
				entry.Domain = domain;
				return true;
			}
			if (typeChanged && event.Reason != GroupRefreshReason::Load &&
				!EditLocalAnimator(
					context,
					event,
					owner,
					entry,
					document,
					"subtype",
					EnumValue{0},
					event.SubtypeAnimator == GroupSubtypeAnimator::Animated
				))
				return false;
			const auto sourceNode =
				std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const Node &node) {
					return node.Id == entry.NodeId;
				});
			const bool sourceAnimator =
				sourceNode != document.Nodes.end() && (std::find(
														   sourceNode->SourceAnimatedInputs.begin(),
														   sourceNode->SourceAnimatedInputs.end(),
														   "parent_value"
													   ) != sourceNode->SourceAnimatedInputs.end() ||
													   std::find(
														   sourceNode->SourceStaticInputs.begin(),
														   sourceNode->SourceStaticInputs.end(),
														   "parent_value"
													   ) != sourceNode->SourceStaticInputs.end());
			const Value *raw = context.Find("parent_value");
			const auto length = raw ? ArrayLength(*raw) : std::optional<size_t>{};
			const auto display = domain.Display.value_or(SourceValueDisplay::Default);
			if (domain.Kind == SourceSocketKind::Trigger) {
				// A Trigger animator has no initial key and its empty getter is false.
				auto lease = owner.Budget.Reserve(0);
				if (!ReplaceParent(context, owner, entry, false, std::move(*lease))) return false;
			} else if (display == SourceValueDisplay::Range || display == SourceValueDisplay::SliderRange ||
					   display == SourceValueDisplay::RotationRange) {
				if (length != 2 && !ResetArray(context, owner, entry, 2, false, sourceAnimator)) return false;
			} else if (display == SourceValueDisplay::Padding) {
				if (length != 4 && !ResetArray(context, owner, entry, 4, false, sourceAnimator)) return false;
			} else if (display == SourceValueDisplay::Vector || display == SourceValueDisplay::VectorRange) {
				if (length != size + 2 && !ResetArray(context, owner, entry, size + 2, false, sourceAnimator))
					return false;
			} else if (display == SourceValueDisplay::Area) {
				if (length != 5) {
					auto lease = owner.Budget.Reserve(0);
					Area area;
					area.CenterX = context.Project.SurfaceWidth / 2.0;
					area.CenterY = context.Project.SurfaceHeight / 2.0;
					area.HalfWidth = area.CenterX;
					area.HalfHeight = area.CenterY;
					if (!ReplaceParent(context, owner, entry, area, std::move(*lease), sourceAnimator))
						return false;
				}
			} else if (display == SourceValueDisplay::Palette) {
				if (!length && !ResetArray(context, owner, entry, 1, true, sourceAnimator)) return false;
			} else if (domain.Kind == SourceSocketKind::Curve) {
				auto lease = owner.Budget.Reserve(2 * sizeof(std::array<double, 6>));
				if (!lease)
					return context.Fail(Status::LimitExceeded, "group curve reset exceeds operation budget");
				Curve curve;
				curve.Header = {0, 1, 0, 0, 1, 0};
				curve.Anchors = {{0, 0, 0, 1, 1.0 / 3, 0}, {-1.0 / 3, 0, 1, 1, 0, 0}};
				if (!ReplaceParent(
						context, owner, entry, std::move(curve), std::move(*lease), sourceAnimator
					))
					return false;
			} else if (domain.Kind == SourceSocketKind::Gradient) {
				auto lease = owner.Budget.Reserve(sizeof(GradientKey));
				if (!lease)
					return context.Fail(
						Status::LimitExceeded, "group gradient reset exceeds operation budget"
					);
				Gradient gradient;
				gradient.Keys = {{0, Colour{255, 255, 255, 255}}};
				if (!ReplaceParent(
						context, owner, entry, std::move(gradient), std::move(*lease), sourceAnimator
					))
					return false;
			} else if (domain.Kind != SourceSocketKind::Integer && domain.Kind != SourceSocketKind::Float &&
					   domain.Kind != SourceSocketKind::Boolean && domain.Kind != SourceSocketKind::Colour &&
					   domain.Kind != SourceSocketKind::FilePath && domain.Kind != SourceSocketKind::Text &&
					   domain.Kind != SourceSocketKind::Any) {
				if (!ResetScalar(context, owner, entry, -4.0, sourceAnimator)) return false;
			} else if (length && display != SourceValueDisplay::Seed &&
					   !ResetScalar(context, owner, entry, 0, sourceAnimator))
				return false;
			entry.InputType = type;
			entry.Subtype = subtype;
			entry.VectorSize = size;
			entry.Domain = domain;
			return true;
		}
		bool EditSeparatedAnimator(
			NodeContext &context,
			const GroupRefreshEvent &event,
			GroupReplayAccess::Owner &owner,
			const Document &document,
			bool &handled,
			uint64_t &axisWork
		) {
			handled = false;
			const auto admit = [&](uint64_t visits) {
				return AdmitSourceAxisWork(axisWork, visits) ||
					   context.Fail(
						   Status::LimitExceeded,
						   "split animator edit batch exceeds work bounds",
						   event.EditedPort
					   );
			};
			if (!admit(document.Nodes.size())) return false;
			const auto selected =
				std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const auto &node) {
					return node.Id == event.NodeId;
				});
			if (selected == document.Nodes.end()) return true;
			const auto source = ResolveLocalSourceAxes(
				document,
				*selected,
				event.EditedPort,
				owner.Bindings,
				owner.SharedSubtypes,
				owner.DetachedAnimators,
				event.NodeId,
				event.EditedPort,
				event.LocalAnimated,
				axisWork
			);
			if (source.Code != Status::Ok)
				return context.Fail(source.Code, std::string(source.Message), event.EditedPort);
			if (!source.Separated) return true;
			const auto ownerId = std::string_view(source.Owner->Id), port = source.Port;
			const bool animated = source.WriterAnimated;
			const auto *axes = source.Axes;
			if (!admit(owner.SharedSubtypes.size())) return false;
			auto found = std::find_if(
				owner.SharedSubtypes.begin(), owner.SharedSubtypes.end(), [&](const auto &overlay) {
					return overlay.NodeId == ownerId && overlay.Port == port;
				}
			);
			handled = true;
			const auto *pair = std::get_if<Vector2>(event.LocalValue);
			if (!pair || !std::isfinite(pair->X) || !std::isfinite(pair->Y))
				return context.Fail(Status::TypeMismatch, "split animator edits require a finite Vec2", port);
			// Admission covers the final logical document before either axis grows.
			if (!admit(document.Nodes.size() + owner.SharedSubtypes.size())) return false;
			size_t total = document.Keyframes.size();
			for (const auto &n : document.Nodes)
				if (n.SourceSeparatedVec2Animators)
					for (const auto &input : n.SourceSeparatedVec2Animators->Inputs) {
						if (!admit(1 + owner.SharedSubtypes.size())) return false;
						const auto replacement = std::find_if(
							owner.SharedSubtypes.begin(), owner.SharedSubtypes.end(), [&](const auto &o) {
								return o.NodeId == n.Id && o.Port == input.Port && o.SeparatedVec2;
							}
						);
						const auto &stored =
							replacement == owner.SharedSubtypes.end() ? input : *replacement->SeparatedVec2;
						for (const auto &axis : stored.Axes)
							total += axis.Keys.size();
					}
			for (const auto &overlay : owner.SharedSubtypes) {
				if (!admit(owner.DetachedAnimators.size())) return false;
				if (overlay.SeparatedVec2 && std::any_of(
												 owner.DetachedAnimators.begin(),
												 owner.DetachedAnimators.end(),
												 [&](const auto &animator) {
													 return animator.OwnerId == overlay.NodeId &&
															animator.Id == overlay.Port;
												 }
											 ))
					for (const auto &axis : overlay.SeparatedVec2->Axes)
						total += axis.Keys.size();
			}
			const auto time = GetFrameTime(event.At);
			for (const auto &axis : axes->Axes) {
				if (!admit(axis.Keys.size())) return false;
				const bool existing = std::any_of(axis.Keys.begin(), axis.Keys.end(), [&](const auto &key) {
					return GetFrameTime(key) == time;
				});
				if ((animated && !existing) || (!animated && axis.Keys.empty())) ++total;
			}
			if (total > Limits::MaximumKeyframes)
				return context.Fail(
					Status::LimitExceeded, "split animator edits exceed aggregate key bounds", port
				);
			if (found == owner.SharedSubtypes.end()) {
				auto names = owner.Budget.Reserve(TextBytes(ownerId) + TextBytes(port));
				if (!names || owner.SharedSubtypes.size() == owner.SharedSubtypes.capacity())
					return context.Fail(
						Status::LimitExceeded, "split animator owner exceeds operation budget", port
					);
				GroupSubtypeOverlay overlay;
				overlay.NodeId = std::string(ownerId);
				overlay.Port = std::string(port);
				owner.SharedSubtypes.push_back(std::move(overlay));
				if (!owner.Charge.Merge(std::move(*names))) std::terminate();
				found = std::prev(owner.SharedSubtypes.end());
			}
			if (!found->SeparatedVec2) {
				const auto bytes = SeparatedAnimatorBytes(*axes, false);
				auto copy = bytes ? owner.Budget.Reserve(*bytes) : std::nullopt;
				if (!copy)
					return context.Fail(
						Status::LimitExceeded, "split animator copy exceeds operation budget", port
					);
				found->SeparatedVec2.emplace() = *axes;
				if (!owner.Charge.Merge(std::move(*copy))) std::terminate();
			}
			for (size_t index = 0; index < 2; ++index) {
				auto &axis = found->SeparatedVec2->Axes[index];
				std::optional<Value> fixed;
				if (!EditAnimator(
						context,
						event,
						owner,
						ownerId,
						axis.Keys,
						fixed,
						document,
						port,
						Value{index == 0 ? pair->X : pair->Y},
						animated,
						true,
						false,
						true
					))
					return false;
				if (fixed) {
					auto names = owner.Budget.Reserve(
						sizeof(Keyframe) + TextBytes(ownerId) + TextBytes(port) + TextBytes("source") +
						2 * TextBytes("linear")
					);
					if (!names)
						return context.Fail(
							Status::LimitExceeded, "split static key exceeds operation budget", port
						);
					Keyframe key;
					key.NodeId = std::string(ownerId);
					key.Port = std::string(port);
					key.Data = std::move(*fixed);
					key.Interpolation = "source";
					key.Ease = KeyframeEase{};
					axis.Keys.reserve(1);
					axis.Keys.push_back(std::move(key));
					if (!owner.Charge.Merge(std::move(*names))) std::terminate();
				}
			}
			return true;
		}

		bool ApplySourceAnimatorEdit(
			NodeContext &context,
			const GroupRefreshEvent &event,
			GroupReplayAccess::Owner &owner,
			const Document &document,
			uint64_t &axisWork
		) {
			bool separated = false;
			if (!EditSeparatedAnimator(context, event, owner, document, separated, axisWork)) return false;
			if (separated) return true;

			if (context.Authored.Type == "pc.group_input" && event.EditedPort == "parent_value") {
				auto parent =
					std::find_if(owner.Entries.begin(), owner.Entries.end(), [&](const auto &entry) {
						return entry.NodeId == event.NodeId;
					});
				if (parent == owner.Entries.end())
					return context.Fail(
						Status::InvalidValue,
						"local parent animator edit requires a loaded boundary",
						event.EditedPort
					);
				return EditLocalAnimator(
					context,
					event,
					owner,
					*parent,
					document,
					"parent_value",
					*event.LocalValue,
					event.LocalAnimated
				);
			}
			auto targetCharge = owner.Budget.Reserve(TextBytes(event.NodeId));
			if (!targetCharge)
				return context.Fail(
					Status::LimitExceeded,
					"shared animator temporary target exceeds operation budget",
					event.EditedPort
				);
			GroupReplayEntry target;
			target.NodeId = std::string(event.NodeId);
			return EditLocalAnimator(
				context,
				event,
				owner,
				target,
				document,
				event.EditedPort,
				*event.LocalValue,
				event.LocalAnimated
			);
		}

	} // namespace detail
} // namespace engine::imagegraph
namespace engine::imagegraph {
	Status ReplayGroupAnimatorEdits(
		const Document &document,
		std::span<const GroupRefreshEvent> edits,
		const GroupReplayState &previous,
		uint64_t revision,
		GroupReplayState &result,
		Diagnostic &diagnostic,
		uint64_t maximumBytes
	) try {
		ENGINE_PROFILE("imagegraph.group_animator_edit");
		const auto fail =
			[&](Status code, std::string message, std::string_view id = {}, std::string_view port = {}) {
				diagnostic = {code, std::string(id), std::string(port), std::move(message)};
				return code;
			};
		if (edits.size() > Limits::MaximumNodes * Limits::MaximumArrayElements)
			return fail(Status::LimitExceeded, "shared animator edit count exceeds bounds");
		if (!previous.InstancesBound() || previous.AuthoringRevision() != revision)
			return fail(Status::InvalidValue, "shared animator edits require bound owners at this revision");
		const auto ownerBudget =
			AdmitGroupReplayDocument(document, previous, result, maximumBytes, diagnostic);
		if (!ownerBudget) return diagnostic.Code;
		for (const auto &edit : edits) {
			if (edit.NodeId.size() > Limits::MaximumTextBytes ||
				edit.EditedPort.size() > Limits::MaximumTextBytes)
				return fail(Status::LimitExceeded, "shared animator edit names exceed bounds");
			const auto node =
				std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const auto &node) {
					return node.Id == edit.NodeId;
				});
			const bool localParent = node != document.Nodes.end() && node->Type == "pc.group_input" &&
									 edit.EditedPort == "parent_value";
			const auto *input = node == document.Nodes.end() ? nullptr
								: localParent
									? FindCatalogueInput(*FindCatalogueEntry(node->Type), edit.EditedPort)
									: detail::AliasedSourceInput(*node, edit.EditedPort);
			if (node == document.Nodes.end() || !input || (localParent && !previous.Find(node->Id)) ||
				!edit.LocalValue || !detail::ValidRuntimeValue(*edit.LocalValue) ||
				!ValidFrameTime(GetFrameTime(edit.At)))
				return fail(
					Status::InvalidValue,
					"animator edit requires a supported source input or loaded local parent and finite value",
					edit.NodeId,
					edit.EditedPort
				);
			const auto *array = std::get_if<ArrayValue>(edit.LocalValue);
			if (input->Type != ValueType::Any && detail::PayloadType(*edit.LocalValue) != input->Type &&
				!detail::SourceLuaArgumentType(*node, edit.EditedPort) &&
				!detail::SourceHlslArgumentValue(*node, edit.EditedPort, *edit.LocalValue) &&
				!CatalogueSourceRawValue(*input, *edit.LocalValue) &&
				!CatalogueSourceEnumValue(*input, *edit.LocalValue) &&
				!(array && CatalogueAuthoredArray(*FindCatalogueEntry(node->Type), *input, *array)))
				return fail(
					Status::TypeMismatch,
					"shared animator value has the wrong source input type",
					edit.NodeId,
					edit.EditedPort
				);
			if (!localParent && !node->InstanceBase.empty() && !previous.Binding(node->Id, edit.EditedPort))
				return fail(
					Status::InvalidGroup,
					"shared animator edit has no original owner binding",
					edit.NodeId,
					edit.EditedPort
				);
		}
		auto candidate = detail::CloneGroupReplay(
			previous,
			edits.size(),
			revision,
			*ownerBudget,
			&previous == &result ? 0 : result.RetainedBytes(),
			diagnostic
		);
		if (!candidate) return diagnostic.Code;
		uint64_t axisWork = 0;
		for (const auto &edit : edits) {
			const auto node =
				std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const auto &node) {
					return node.Id == edit.NodeId;
				});
			detail::NodeContext context(*node, *FindCatalogueEntry(node->Type), edit.At, candidate->Budget);
			if (!detail::ApplySourceAnimatorEdit(context, edit, *candidate, document, axisWork))
				return fail(context.FailureCode, context.FailureMessage, edit.NodeId, context.FailurePort);
		}
		detail::GroupReplayAccess::Install(result, std::move(candidate));
		diagnostic = {};
		return Status::Ok;
	} catch (const std::bad_alloc &) {
		diagnostic = {Status::LimitExceeded, {}, {}, "shared animator edit allocation failed"};
		return diagnostic.Code;
	}

	Status ProjectGroupReplay(
		const Document &authored,
		const GroupReplayState &replay,
		uint64_t revision,
		Document &result,
		Diagnostic &diagnostic,
		uint64_t maximumBytes
	) try {
		ENGINE_PROFILE("imagegraph.group_projection");
		const auto fail =
			[&](Status code, std::string message, std::string_view node = {}, std::string_view port = {}) {
				diagnostic = {code, std::string(node), std::string(port), std::move(message)};
				return code;
			};
		if (!maximumBytes || maximumBytes > Limits::MaximumEvaluationBytes)
			return fail(Status::LimitExceeded, "Group authored projection byte bound is invalid");
		if (replay.AuthoringRevision() != revision)
			return fail(Status::InvalidValue, "Group authored projection requires this authoring revision");
		const auto authoredBytes = DocumentRetainedPayloadBytes(authored);
		const auto destinationBytes =
			&authored == &result ? std::optional<uint64_t>{0} : DocumentRetainedPayloadBytes(result);
		if (!authoredBytes || !destinationBytes)
			return fail(Status::LimitExceeded, "Group authored projection document exceeds bounds");
		uint64_t projectionWork = 0;
		bool workExceeded = false;
		const auto admitVisit = [&]() {
			if (++projectionWork <= 64'000'000) return true;
			workExceeded = true;
			fail(Status::LimitExceeded, "Group animator projection exceeds work bounds");
			return false;
		};
		auto effects = [&](const auto &visit) {
			for (const auto &entry : replay.Entries()) {
				if (!admitVisit()) return false;
				if (!visit(
						entry.NodeId, std::string_view("parent_value"), entry.ParentReset, entry.ParentKeys
					))
					return false;
				if (!replay.Binding(entry.NodeId) &&
					!visit(entry.NodeId, std::string_view("subtype"), entry.SubtypeStatic, entry.SubtypeKeys))
					return false;
			}
			for (const auto &shared : replay.SharedSubtypes()) {
				if (!admitVisit()) return false;
				if (replay.DetachedAnimators().size() > 64'000'000 - projectionWork) {
					projectionWork = 64'000'000;
					return admitVisit();
				}
				projectionWork += replay.DetachedAnimators().size();
				if (replay.DetachedAnimator(shared.NodeId, shared.Port)) {
					for (const auto &binding : replay.Bindings()) {
						if (!admitVisit()) return false;
						if (binding.OwnerId == shared.NodeId &&
							detail::BindingAnimatorPort(binding) == shared.Port)
							if (!visit(
									binding.NodeId, std::string_view(binding.Port), shared.Fixed, shared.Keys
								))
								return false;
					}
				} else if (!visit(shared.NodeId, std::string_view(shared.Port), shared.Fixed, shared.Keys))
					return false;
			}
			return true;
		};
		auto changed = [&](std::string_view id, std::string_view port) {
			bool found = false;
			effects([&](std::string_view owner, std::string_view input, const auto &fixed, const auto &keys) {
				if (owner == id && input == port && (fixed || !keys.empty())) found = true;
				return true;
			});
			return found;
		};
		const auto projectedValue = [&](std::string_view id,
										std::string_view port,
										const std::optional<Value> &fixed,
										const std::vector<Keyframe> &keys) -> const Value * {
			if (fixed) return &*fixed;
			if (port != "parent_value" || keys.empty()) return nullptr;
			const auto node =
				std::find_if(authored.Nodes.begin(), authored.Nodes.end(), [&](const auto &value) {
					return value.Id == id;
				});
			if (node == authored.Nodes.end() || node->Type != "pc.group_input" ||
				std::find(node->SourceStaticInputs.begin(), node->SourceStaticInputs.end(), port) ==
					node->SourceStaticInputs.end())
				return nullptr;
			return &keys.front().Data;
		};
		struct ProjectedValue {
			std::string_view NodeId;
			std::string_view Port;
		};
		if (replay.Entries().size() >
			(std::numeric_limits<size_t>::max() - replay.SharedSubtypes().size()) / 2)
			return fail(Status::LimitExceeded, "Group projected value count exceeds bounds");
		const size_t projectedValueCapacity =
			replay.Entries().size() * 2 + replay.SharedSubtypes().size() + replay.Bindings().size();
		if (projectedValueCapacity > std::numeric_limits<uint64_t>::max() / sizeof(ProjectedValue))
			return fail(Status::LimitExceeded, "Group projected value storage exceeds bounds");
		uint64_t admitted = 0;
		const auto add = [&](uint64_t bytes) {
			if (bytes > maximumBytes - admitted) return false;
			admitted += bytes;
			return true;
		};
		if (!add(*authoredBytes) || !add(*destinationBytes) || !add(replay.RetainedBytes()) ||
			!add(*authoredBytes) || !add(projectedValueCapacity * sizeof(ProjectedValue)))
			return fail(
				Status::LimitExceeded, "Group authored projection replacement overlap exceeds bounds"
			);
		detail::EvaluationBudget budget(maximumBytes);
		auto reservation = budget.Reserve(admitted);
		if (!reservation) return fail(Status::LimitExceeded, "Group authored projection admission failed");
		const uint64_t preadmitted = admitted;
		// Reserve the bounded effect index before allocating its scratch array.
		auto projectedValues = std::make_unique<ProjectedValue[]>(projectedValueCapacity);
		size_t projectedValueCount = 0;
		effects([&](std::string_view id, std::string_view port, const auto &fixed, const auto &keys) {
			if (projectedValue(id, port, fixed, keys)) projectedValues[projectedValueCount++] = {id, port};
			return true;
		});
		size_t keyCount = 0, extraTracks = 0;
		for (const auto &key : authored.Keyframes)
			if (!changed(key.NodeId, key.Port)) ++keyCount;
		size_t projectedValueIndex = 0;
		uint64_t replacementCapacitySlots = 0;
		if (!effects([&](std::string_view id,
						 std::string_view port,
						 const std::optional<Value> &fixed,
						 const std::vector<Keyframe> &keys) {
				if (!fixed && keys.empty()) return true;
				const auto node =
					std::find_if(authored.Nodes.begin(), authored.Nodes.end(), [&](const auto &node) {
						return node.Id == id;
					});
				if (node == authored.Nodes.end() ||
					(port == "parent_value" ? node->Type != "pc.group_input"
											: !detail::AliasedSourceInput(*node, port))) {
					fail(Status::InvalidGroup, "Group animator projection owner is absent", id, port);
					return false;
				}
				if (fixed && !keys.empty()) {
					fail(
						Status::InvalidValue,
						"Group animator projection mixes fixed and keyed effects",
						id,
						port
					);
					return false;
				}
				for (const auto &key : keys)
					if ((key.NodeId != id || key.Port != port) &&
						!(replay.Binding(id, port) && replay.Binding(id, port)->OwnerId == key.NodeId &&
						  detail::BindingAnimatorPort(*replay.Binding(id, port)) == key.Port &&
						  replay.DetachedAnimator(key.NodeId, key.Port))) {
						fail(
							Status::InvalidValue, "Group animator projection key has another owner", id, port
						);
						return false;
					}
				if (keys.size() > Limits::MaximumKeyframes - keyCount) {
					fail(Status::LimitExceeded, "Group authored key count exceeds bounds", id, port);
					return false;
				}
				keyCount += keys.size();
				const Value *replacement = projectedValue(id, port, fixed, keys);
				if (replacement) {
					const auto payload = ValueClonePayloadBytes(*replacement);
					if (!payload || !add(*payload)) {
						fail(
							Status::LimitExceeded, "Group authored fixed replacement exceeds bounds", id, port
						);
						return false;
					}
					if (projectedValueIndex >= projectedValueCount) {
						fail(Status::InvalidValue, "Group projected value order is invalid", id, port);
						return false;
					}
					size_t valuesBefore = node->Values.size();
					for (size_t prior = 0; prior < projectedValueIndex; ++prior) {
						const auto &candidate = projectedValues[prior];
						if (candidate.NodeId != id) continue;
						const bool existsInAuthored =
							std::any_of(node->Values.begin(), node->Values.end(), [&](const auto &value) {
								return value.Port == candidate.Port;
							});
						bool wasInserted = false;
						for (size_t earlier = 0; earlier < prior; ++earlier)
							if (projectedValues[earlier].NodeId == id &&
								projectedValues[earlier].Port == candidate.Port) {
								wasInserted = true;
								break;
							}
						if (existsInAuthored || wasInserted) continue;
						++valuesBefore;
					}
					if (valuesBefore == std::numeric_limits<size_t>::max() ||
						valuesBefore + 1 >
							(std::numeric_limits<uint64_t>::max() - replacementCapacitySlots)) {
						fail(Status::LimitExceeded, "Group authored value capacity exceeds bounds", id, port);
						return false;
					}
					replacementCapacitySlots += valuesBefore + 1;
					++projectedValueIndex;
					for (const auto &value : node->Values) {
						const auto bytes = ValueClonePayloadBytes(value.Data);
						if (!bytes || !add(*bytes) ||
							!add(std::max(value.Port.size(), std::string{}.capacity()) + 1)) {
							fail(
								Status::LimitExceeded, "Group authored value clone exceeds bounds", id, port
							);
							return false;
						}
					}
					if (!add(std::max(port.size(), std::string{}.capacity()) + 1)) {
						fail(Status::LimitExceeded, "Group authored port name exceeds bounds", id, port);
						return false;
					}
					// Account for every projected control copied into a later replacement.
					if (!effects([&](std::string_view otherId,
									 std::string_view otherPort,
									 const auto &otherFixed,
									 const auto &otherKeys) {
							if (otherId != id) return true;
							const auto *otherValue =
								projectedValue(otherId, otherPort, otherFixed, otherKeys);
							if (!otherValue) return true;
							const auto bytes = ValueClonePayloadBytes(*otherValue);
							return bytes && add(*bytes) &&
								   add(std::max(otherPort.size(), std::string{}.capacity()) + 1);
						})) {
						fail(
							Status::LimitExceeded,
							"Group authored value replacement overlap exceeds bounds",
							id,
							port
						);
						return false;
					}
				}
				if (!keys.empty() &&
					std::any_of(
						keys.begin(),
						keys.end(),
						[](const auto &key) { return key.Interpolation == "source"; }
					) &&
					std::none_of(authored.Tracks.begin(), authored.Tracks.end(), [&](const auto &track) {
						return track.NodeId == id && track.Port == port;
					})) {
					++extraTracks;
					std::string_view end = "hold";
					if (const auto *binding = replay.Binding(id, port))
						if (const auto *detached = replay.DetachedAnimator(
								binding->OwnerId, detail::BindingAnimatorPort(*binding)
							);
							detached && detached->Track)
							end = detached->Track->End;
					if (!add(std::max(id.size(), std::string{}.capacity()) + 1) ||
						!add(std::max(port.size(), std::string{}.capacity()) + 1) ||
						!add(std::max(end.size(), std::string{}.capacity()) + 1)) {
						fail(Status::LimitExceeded, "Group authored source track exceeds bounds", id, port);
						return false;
					}
				}
				return true;
			}))
			return diagnostic.Code;
		if (projectedValueIndex != projectedValueCount ||
			replacementCapacitySlots > std::numeric_limits<uint64_t>::max() / sizeof(AuthoredValue) ||
			!add(replacementCapacitySlots * sizeof(AuthoredValue)))
			return fail(Status::LimitExceeded, "Group authored value capacities exceed bounds");
		if (keyCount > Limits::MaximumKeyframes ||
			extraTracks > Limits::MaximumTracks - authored.Tracks.size() ||
			!add(keyCount * sizeof(Keyframe)) ||
			!add((authored.Tracks.size() + extraTracks) * sizeof(AnimationTrack)))
			return fail(Status::LimitExceeded, "Group authored animator containers exceed bounds");
		for (const auto &key : authored.Keyframes)
			if (!changed(key.NodeId, key.Port)) {
				const auto bytes = KeyframePayloadBytes(key);
				if (!bytes || !add(*bytes))
					return fail(Status::LimitExceeded, "Group authored key clone exceeds bounds");
			}
		if (!effects([&](std::string_view id, std::string_view port, const auto &, const auto &keys) {
				for (const auto &key : keys) {
					const auto bytes = KeyframePayloadBytes(key);
					if (!bytes || !add(*bytes) || !add(std::max(id.size(), std::string{}.capacity()) + 1) ||
						!add(std::max(port.size(), std::string{}.capacity()) + 1))
						return false;
				}
				return true;
			}))
			return fail(Status::LimitExceeded, "Group authored effect keys exceed bounds");
		for (const auto &track : authored.Tracks)
			if (!add(std::max(track.NodeId.size(), std::string{}.capacity()) + 1) ||
				!add(std::max(track.Port.size(), std::string{}.capacity()) + 1) ||
				!add(std::max(track.End.size(), std::string{}.capacity()) + 1))
				return fail(Status::LimitExceeded, "Group authored track clone exceeds bounds");
		struct ProjectedAxes {
			std::string_view NodeId, Port;
			const SourceSeparatedVec2Animator *Source;
			bool Detached;
		};
		detail::EvaluationVector<ProjectedAxes> axisTargets{
			detail::EvaluationAllocator<ProjectedAxes>(budget)
		};
		const auto admitAxisWork = [&](uint64_t visits) {
			if (visits > 64'000'000 - projectionWork) {
				fail(Status::LimitExceeded, "split projection exceeds work bounds");
				return false;
			}
			projectionWork += visits;
			return true;
		};
		const auto addAxisTarget = [&](const GroupSubtypeOverlay &overlay,
									   std::string_view id,
									   std::string_view port,
									   bool detached) {
			if (!admitAxisWork(authored.Nodes.size() + axisTargets.size())) return false;
			const auto node = std::find_if(authored.Nodes.begin(), authored.Nodes.end(), [&](const auto &n) {
				return n.Id == id;
			});
			if (node != authored.Nodes.end() &&
				!admitAxisWork(
					node->DynamicInputs.size() + (node->SourceSeparatedVec2Animators
													  ? node->SourceSeparatedVec2Animators->Inputs.size()
													  : 0)
				))
				return false;
			if (node == authored.Nodes.end() || !detail::SourceSeparatedVec2Input(*node, port) ||
				(!detached && !detail::FindSeparatedVec2(*node, port)) ||
				std::any_of(axisTargets.begin(), axisTargets.end(), [&](const auto &target) {
					return target.NodeId == id && target.Port == port;
				})) {
				fail(Status::InvalidGroup, "split projection target is invalid", id, port);
				return false;
			}
			const auto bytes = detail::SeparatedAnimatorBytes(*overlay.SeparatedVec2, false);
			if (!bytes || !add(*bytes) || !add(std::max(port.size(), std::string{}.capacity()) + 1)) {
				fail(Status::LimitExceeded, "split projection payload exceeds bounds", id, port);
				return false;
			}
			for (const auto &axis : overlay.SeparatedVec2->Axes)
				for (size_t key = 0; key < axis.Keys.size(); ++key) {
					if (!admitAxisWork(1) || !add(std::max(id.size(), std::string{}.capacity()) + 1) ||
						!add(std::max(port.size(), std::string{}.capacity()) + 1)) {
						fail(Status::LimitExceeded, "split projection key names exceed bounds", id, port);
						return false;
					}
				}
			axisTargets.push_back({id, port, &*overlay.SeparatedVec2, detached});
			return true;
		};
		for (const auto &overlay : replay.SharedSubtypes()) {
			if (!overlay.SeparatedVec2) continue;
			if (replay.DetachedAnimator(overlay.NodeId, overlay.Port)) {
				for (const auto &binding : replay.Bindings()) {
					if (!admitVisit()) return diagnostic.Code;
					if (detail::BindingReferencesAxes(binding, overlay.NodeId, overlay.Port) &&
						!addAxisTarget(overlay, binding.NodeId, binding.Port, true))
						return diagnostic.Code;
				}
			} else if (!addAxisTarget(overlay, overlay.NodeId, overlay.Port, false))
				return diagnostic.Code;
		}
		size_t combinedKeys = keyCount;
		const auto addAxisKeys = [&](const SourceSeparatedVec2Animator &input) {
			for (const auto &axis : input.Axes) {
				if (axis.Keys.size() > Limits::MaximumKeyframes - combinedKeys) return false;
				combinedKeys += axis.Keys.size();
			}
			return true;
		};
		for (const auto &node : authored.Nodes) {
			const size_t stored =
				node.SourceSeparatedVec2Animators ? node.SourceSeparatedVec2Animators->Inputs.size() : 0;
			if (!admitAxisWork(uint64_t(stored + 1) * axisTargets.size() * 4)) return diagnostic.Code;
			size_t added = 0;
			for (const auto &target : axisTargets) {
				if (!admitVisit()) return diagnostic.Code;
				if (target.NodeId != node.Id) continue;
				if (!detail::FindSeparatedVec2(node, target.Port)) {
					++added;
					if (!addAxisKeys(*target.Source))
						return fail(
							Status::LimitExceeded,
							"projected split keys exceed aggregate bounds",
							node.Id,
							target.Port
						);
				}
			}
			if (added && (!add((stored + added) * sizeof(SourceSeparatedVec2Animator)) ||
						  (!node.SourceSeparatedVec2Animators && !add(sizeof(SourceSeparatedVec2Data)))))
				return fail(Status::LimitExceeded, "projected split input storage exceeds bounds", node.Id);
			if (node.SourceSeparatedVec2Animators)
				for (const auto &input : node.SourceSeparatedVec2Animators->Inputs) {
					const auto replacement =
						std::find_if(axisTargets.begin(), axisTargets.end(), [&](const auto &target) {
							return target.NodeId == node.Id && target.Port == input.Port;
						});
					if (!addAxisKeys(replacement == axisTargets.end() ? input : *replacement->Source))
						return fail(
							Status::LimitExceeded,
							"projected split keys exceed aggregate bounds",
							node.Id,
							input.Port
						);
				}
		}

		auto additionalAdmission = budget.Reserve(admitted - preadmitted);
		if (!additionalAdmission || !reservation->Merge(std::move(*additionalAdmission)))
			return fail(Status::LimitExceeded, "Group authored projection admission failed");
		core::Metrics::Count(
			"imagegraph.group_projection.admitted_payload_bytes", static_cast<double>(admitted)
		);
		core::Metrics::Count("imagegraph.group_projection.operations", 1);
		Document candidate = authored;
		for (auto &node : candidate.Nodes) {
			size_t added = 0;
			for (const auto &target : axisTargets)
				if (target.NodeId == node.Id && !detail::FindSeparatedVec2(node, target.Port)) ++added;
			if (added) {
				if (!node.SourceSeparatedVec2Animators) node.SourceSeparatedVec2Animators.emplace();
				node.SourceSeparatedVec2Animators->Inputs.reserve(
					node.SourceSeparatedVec2Animators->Inputs.size() + added
				);
			}
			for (const auto &target : axisTargets) {
				if (target.NodeId != node.Id) continue;
				const auto *old = detail::FindSeparatedVec2(node, target.Port);
				SourceSeparatedVec2Animator replacement = *target.Source;
				replacement.Port = std::string(target.Port);
				if (target.Detached) replacement.Separated = old && old->Separated;
				for (auto &axis : replacement.Axes)
					for (auto &key : axis.Keys) {
						key.NodeId = std::string(node.Id);
						key.Port = std::string(target.Port);
						if (target.Detached) key.SourceKeyId.clear();
					}
				auto &inputs = node.SourceSeparatedVec2Animators->Inputs;
				const auto found = std::find_if(inputs.begin(), inputs.end(), [&](const auto &input) {
					return input.Port == target.Port;
				});
				if (found == inputs.end())
					inputs.push_back(std::move(replacement));
				else
					*found = std::move(replacement);
			}
		}

		std::vector<Keyframe> finalKeys;
		finalKeys.reserve(keyCount);
		for (const auto &key : authored.Keyframes)
			if (!changed(key.NodeId, key.Port)) finalKeys.push_back(key);
		std::vector<AnimationTrack> finalTracks;
		finalTracks.reserve(authored.Tracks.size() + extraTracks);
		for (const auto &track : authored.Tracks)
			finalTracks.push_back(track);
		effects([&](std::string_view id, std::string_view port, const auto &fixed, const auto &keys) {
			const auto *replacement = projectedValue(id, port, fixed, keys);
			if (replacement) {
				auto node =
					std::find_if(candidate.Nodes.begin(), candidate.Nodes.end(), [&](const auto &node) {
						return node.Id == id;
					});
				const auto dynamic = std::find_if(
					node->DynamicInputs.begin(), node->DynamicInputs.end(), [&](const auto &input) {
						return input.Id == port;
					}
				);
				if (dynamic != node->DynamicInputs.end()) {
					// Dynamic controls serialize their authored value in the socket declaration.
					dynamic->Default = *replacement;
				} else {
					std::vector<AuthoredValue> values;
					values.reserve(node->Values.size() + 1);
					bool replaced = false;
					for (const auto &value : node->Values) {
						if (value.Port == port) {
							values.push_back({std::string(port), *replacement});
							replaced = true;
						} else
							values.push_back(value);
					}
					if (!replaced) values.push_back({std::string(port), *replacement});
					node->Values.swap(values);
				}
			}
			for (const auto &key : keys) {
				finalKeys.push_back(key);
				if (key.NodeId != id || key.Port != port) {
					std::string nodeName(id), inputName(port);
					finalKeys.back().NodeId.swap(nodeName);
					finalKeys.back().Port.swap(inputName);
					finalKeys.back().SourceKeyId.clear();
				}
			}
			if (!keys.empty() &&
				std::any_of(
					keys.begin(), keys.end(), [](const auto &key) { return key.Interpolation == "source"; }
				) &&
				std::none_of(finalTracks.begin(), finalTracks.end(), [&](const auto &track) {
					return track.NodeId == id && track.Port == port;
				})) {
				AnimationTrack projected{std::string(id), std::string(port), "hold", -1};
				if (const auto *binding = replay.Binding(id, port))
					if (const auto *detached =
							replay.DetachedAnimator(binding->OwnerId, detail::BindingAnimatorPort(*binding));
						detached && detached->Track) {
						projected.End = detached->Track->End;
						projected.LoopRange = detached->Track->LoopRange;
						projected.QuaternionMode = detached->Track->QuaternionMode;
					}
				finalTracks.push_back(std::move(projected));
			}
			return true;
		});
		if (workExceeded) return diagnostic.Code;
		candidate.Keyframes.swap(finalKeys);
		candidate.Tracks.swap(finalTracks);
		const auto retained = DocumentRetainedPayloadBytes(candidate);
		if (!retained || *retained > maximumBytes)
			return fail(Status::LimitExceeded, "Group authored projection retained result exceeds bounds");
		result = std::move(candidate);
		diagnostic = {};
		return Status::Ok;
	} catch (const std::bad_alloc &) {
		diagnostic = {Status::LimitExceeded, {}, {}, "Group authored projection allocation failed"};
		return diagnostic.Code;
	}
}
namespace engine::imagegraph {
	Status RebindProjectedGroupReplay(
		const Document &document,
		const GroupReplayState &previous,
		uint64_t revision,
		GroupReplayState &result,
		Diagnostic &diagnostic,
		uint64_t maximumBytes
	) try {
		if (!AdmitGroupReplayDocument(document, previous, result, maximumBytes, diagnostic))
			return diagnostic.Code;

		const auto matches = [&](std::string_view id,
								 std::string_view port,
								 const std::optional<Value> &fixed,
								 const std::vector<Keyframe> &keys) {
			if (!fixed && keys.empty()) return true;
			const auto node =
				std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const auto &node) {
					return node.Id == id;
				});
			if (node == document.Nodes.end()) return false;
			if (fixed) {
				if (!keys.empty() ||
					std::any_of(document.Keyframes.begin(), document.Keyframes.end(), [&](const auto &key) {
						return key.NodeId == id && key.Port == port;
					}))
					return false;
				const auto dynamic = std::find_if(
					node->DynamicInputs.begin(), node->DynamicInputs.end(), [&](const auto &input) {
						return input.Id == port;
					}
				);
				if (dynamic != node->DynamicInputs.end())
					return dynamic->Default && *dynamic->Default == *fixed;
				const auto value =
					std::find_if(node->Values.begin(), node->Values.end(), [&](const auto &value) {
						return value.Port == port;
					});
				return value != node->Values.end() && value->Data == *fixed;
			}
			size_t index = 0;
			for (const auto &key : document.Keyframes)
				if (key.NodeId == id && key.Port == port) {
					if (index >= keys.size()) return false;
					const auto &expected = keys[index++];
					if (expected.NodeId == id && expected.Port == port) {
						if (key != expected) return false;
					} else if (!key.SourceKeyId.empty() || std::tie(
															   key.Tick,
															   key.Data,
															   key.Interpolation,
															   key.Ease,
															   key.SineDriver,
															   key.SourceDriver,
															   key.Subframe,
															   key.NegativeFrame,
															   key.Kind
														   ) !=
															   std::tie(
																   expected.Tick,
																   expected.Data,
																   expected.Interpolation,
																   expected.Ease,
																   expected.SineDriver,
																   expected.SourceDriver,
																   expected.Subframe,
																   expected.NegativeFrame,
																   expected.Kind
															   ))
						return false;
				}
			return index == keys.size();
		};
		for (const auto &entry : previous.Entries())
			if (!matches(entry.NodeId, "parent_value", entry.ParentReset, entry.ParentKeys) ||
				(!previous.Binding(entry.NodeId) &&
				 !matches(entry.NodeId, "subtype", entry.SubtypeStatic, entry.SubtypeKeys))) {
				diagnostic = {
					Status::InvalidValue,
					entry.NodeId,
					{},
					"Group replay effects have not been projected into this document"
				};
				return diagnostic.Code;
			}
		for (const auto &shared : previous.SharedSubtypes()) {
			if (shared.SeparatedVec2) {
				const auto projectedAxesMatch =
					[&](std::string_view id, std::string_view port, bool detached) {
						const auto node =
							std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const auto &n) {
								return n.Id == id;
							});
						const auto *axes =
							node == document.Nodes.end() ? nullptr : detail::FindSeparatedVec2(*node, port);
						if (!axes) return false;
						if (!detached) return *axes == *shared.SeparatedVec2;
						for (size_t axis = 0; axis < 2; ++axis) {
							const auto &actual = axes->Axes[axis].Keys;
							const auto &expected = shared.SeparatedVec2->Axes[axis].Keys;
							if (actual.size() != expected.size()) return false;
							for (size_t i = 0; i < actual.size(); ++i) {
								const auto &a = actual[i];
								const auto &e = expected[i];
								if (a.NodeId != id || a.Port != port || !a.SourceKeyId.empty() ||
									std::tie(
										a.Tick,
										a.Data,
										a.Interpolation,
										a.Ease,
										a.SineDriver,
										a.SourceDriver,
										a.Subframe,
										a.NegativeFrame,
										a.Kind
									) !=
										std::tie(
											e.Tick,
											e.Data,
											e.Interpolation,
											e.Ease,
											e.SineDriver,
											e.SourceDriver,
											e.Subframe,
											e.NegativeFrame,
											e.Kind
										))
									return false;
							}
						}
						return true;
					};
				bool represented = true;
				if (previous.DetachedAnimator(shared.NodeId, shared.Port)) {
					for (const auto &binding : previous.Bindings())
						if (detail::BindingReferencesAxes(binding, shared.NodeId, shared.Port))
							represented =
								represented && projectedAxesMatch(binding.NodeId, binding.Port, true);
				} else
					represented = projectedAxesMatch(shared.NodeId, shared.Port, false);
				if (!represented) {
					diagnostic = {
						Status::InvalidValue,
						shared.NodeId,
						shared.Port,
						"split animator effect has not been projected"
					};
					return diagnostic.Code;
				}
			}

			bool represented = true;
			if (previous.DetachedAnimator(shared.NodeId, shared.Port)) {
				for (const auto &binding : previous.Bindings())
					if (binding.OwnerId == shared.NodeId &&
						detail::BindingAnimatorPort(binding) == shared.Port)
						represented =
							represented && matches(binding.NodeId, binding.Port, shared.Fixed, shared.Keys);
			} else
				represented = matches(shared.NodeId, shared.Port, shared.Fixed, shared.Keys);
			if (!represented) {
				diagnostic = {
					Status::InvalidValue,
					shared.NodeId,
					shared.Port,
					"Shared animator effect has not been projected into this document"
				};
				return diagnostic.Code;
			}
		}
		const auto status = RebindGroupReplay(document, previous, revision, result, diagnostic, maximumBytes);
		if (status != Status::Ok) return status;
		auto *owner = detail::GroupReplayAccess::Get(result);
		if (!owner) return Status::Ok;
		const bool retainedDetached = !owner->DetachedAnimators.empty();
		uint64_t removed =
			retainedDetached ? 0 : owner->SharedSubtypes.capacity() * sizeof(GroupSubtypeOverlay);
		const auto clearKeys = [&](std::vector<Keyframe> &keys) {
			removed += keys.capacity() * sizeof(Keyframe);
			for (const auto &key : keys)
				removed += *KeyframePayloadBytes(key) - sizeof(Keyframe);
			std::vector<Keyframe>{}.swap(keys);
		};
		const auto clearFixed = [&](std::optional<Value> &value) {
			if (value) removed += detail::RetainedPayloadBytes(*value);
			value.reset();
		};
		for (auto &entry : owner->Entries) {
			clearKeys(entry.ParentKeys);
			clearKeys(entry.SubtypeKeys);
			clearFixed(entry.ParentReset);
			clearFixed(entry.SubtypeStatic);
		}
		for (auto &shared : owner->SharedSubtypes) {
			if (previous.DetachedAnimator(shared.NodeId, shared.Port)) continue;
			removed += std::max<size_t>(shared.NodeId.size(), 15) + 1;
			removed += std::max(shared.Port.size(), std::string{}.capacity()) + 1;
			clearKeys(shared.Keys);
			clearFixed(shared.Fixed);
			removed += detail::SeparatedOverlayBytes(shared);
			shared.SeparatedVec2 = {};
		}
		if (retainedDetached)
			std::erase_if(owner->SharedSubtypes, [&](const auto &shared) {
				return !previous.DetachedAnimator(shared.NodeId, shared.Port);
			});
		else
			std::vector<GroupSubtypeOverlay>{}.swap(owner->SharedSubtypes);
		// Clone admission charges exact-sized owned slots; discarded effect payloads
		// are released only after their containers have relinquished the storage.
		const bool resized = owner->Charge.Resize(owner->Charge.Bytes() - removed);
		(void)resized;
		assert(resized);
		return Status::Ok;
	} catch (const std::bad_alloc &) {
		diagnostic = {Status::LimitExceeded, {}, {}, "Projected group rebind allocation failed"};
		return diagnostic.Code;
	}
}
