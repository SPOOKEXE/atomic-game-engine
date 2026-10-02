#include "EvaluationAllocator.hpp"
#include "GroupBoundary.hpp"
#include "GroupReplayInternal.hpp"
#include "ValuePayload.hpp"

#include <engine/imagegraph/FrameTime.hpp>

#include <algorithm>
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
					node.DynamicInputs.size() > Limits::MaximumDynamicInputsPerNode ||
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
		if (previous.InstancesBound() || previous.AuthoringRevision() != revision)
			return fail(
				Status::InvalidValue, "Group binding requires the local callback stage at this revision"
			);
		const auto ownerBudget =
			AdmitGroupReplayDocument(document, previous, result, maximumBytes, diagnostic);
		if (!ownerBudget) return diagnostic.Code;
		constexpr size_t maximumBindings = Limits::MaximumNodes * Limits::MaximumArrayElements;
		if (bindings.size() > maximumBindings)
			return fail(Status::LimitExceeded, "source input binding count exceeds public bounds");
		uint64_t names = bindings.size() * sizeof(GroupSubtypeBinding);
		const uint64_t scratchBytes = bindings.size() * sizeof(const GroupSubtypeBinding *) +
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
				  std::string_view(binding.Port)}) {
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
		std::sort(sorted.begin(), sorted.end(), [](const auto *left, const auto *right) {
			return std::tie(left->OwnerId, left->Port) < std::tie(right->OwnerId, right->Port);
		});
		for (size_t index = 1; index < sorted.size(); ++index)
			if (sorted[index - 1]->OwnerId == sorted[index]->OwnerId &&
				sorted[index - 1]->Port == sorted[index]->Port &&
				sorted[index - 1]->Writer != sorted[index]->Writer)
				return fail(
					Status::InvalidValue,
					"source input bindings disagree about their original writer mode",
					sorted[index]->NodeId
				);

		for (size_t index = 0; index < bindings.size(); ++index) {
			const auto &binding = bindings[index];
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
				owner->Type != target->Type || !detail::AliasedSourceInput(*owner, binding.Port))
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
		candidate->Bindings.assign(bindings.begin(), bindings.end());
		if (!candidate->Charge.Merge(std::move(*bindingCharge))) std::terminate();
		uint64_t removedBytes = 0;
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
			if (sourceSurvives(binding.NodeId, binding.Port) && sourceSurvives(binding.OwnerId, binding.Port))
				continue;
			removedBytes += std::max(binding.NodeId.size(), std::string{}.capacity()) + 1;
			removedBytes += std::max(binding.OwnerId.size(), std::string{}.capacity()) + 1;
			removedBytes += std::max(binding.Port.size(), std::string{}.capacity()) + 1;
		}
		std::erase_if(candidate->Bindings, [&](const auto &binding) {
			return !sourceSurvives(binding.NodeId, binding.Port) ||
				   !sourceSurvives(binding.OwnerId, binding.Port);
		});
		for (const auto &overlay : candidate->SharedSubtypes) {
			if (sourceSurvives(overlay.NodeId, overlay.Port)) continue;
			removedBytes += std::max(overlay.NodeId.size(), std::string{}.capacity()) + 1;
			removedBytes += std::max(overlay.Port.size(), std::string{}.capacity()) + 1;
			if (overlay.Fixed) removedBytes += detail::RetainedPayloadBytes(*overlay.Fixed);
			for (const auto &key : overlay.Keys)
				removedBytes += *KeyframePayloadBytes(key);
		}
		std::erase_if(candidate->SharedSubtypes, [&](const auto &overlay) {
			return !sourceSurvives(overlay.NodeId, overlay.Port);
		});
		// Entry-vector capacity remains live; only deleted owned names and payloads
		// leave its charge.
		if (!candidate->Charge.Resize(candidate->Charge.Bytes() - removedBytes)) std::terminate();
		detail::GroupReplayAccess::Install(result, std::move(candidate));
		diagnostic = {};
		return Status::Ok;
	} catch (const std::bad_alloc &) {
		diagnostic = {Status::LimitExceeded, {}, {}, "Group rebind allocation failed"};
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
				bool preserveStaticKeys = false
			) {
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
					} else {
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
							const auto size = KeyframePayloadBytes(*key);
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
						const uint64_t old = KeyBytes(keys);
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
					const uint64_t old = KeyBytes(keys) + (fixed ? RetainedPayloadBytes(*fixed) : 0);
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
				if (fixed) {
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
				} else {
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
					const auto cloneBytes = KeyframePayloadBytes(*key);
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
				std::sort(replacement.begin(), replacement.end(), [](const auto &left, const auto &right) {
					return CompareFrameTime(GetFrameTime(left), GetFrameTime(right)) < 0;
				});
				const uint64_t oldBytes = KeyBytes(keys) + (fixed ? RetainedPayloadBytes(*fixed) : 0);
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
				if (port == "parent_value" && entry.Domain.Kind == SourceSocketKind::Trigger)
					return context.Fail(
						Status::UnsupportedExecution,
						"Group Trigger parent button/tick editing is not represented",
						port
					);
				if (port != "parent_value" && owner.Bound) {
					std::string_view sourceId = entry.NodeId;
					for (const auto &binding : owner.Bindings)
						if (binding.NodeId == entry.NodeId && binding.Port == port) {
							sourceId = binding.OwnerId;
							animated = binding.Writer == GroupSubtypeAnimator::Animated;
							break;
						}
					for (const auto &binding : owner.Bindings)
						if (binding.OwnerId == sourceId && binding.Port == port) {
							animated = binding.Writer == GroupSubtypeAnimator::Animated;
							break;
						}
					auto found = std::find_if(
						owner.SharedSubtypes.begin(), owner.SharedSubtypes.end(), [&](const auto &overlay) {
							return overlay.NodeId == sourceId && overlay.Port == port;
						}
					);
					if (found == owner.SharedSubtypes.end()) {
						auto nameCharge = owner.Budget.Reserve(TextBytes(sourceId) + TextBytes(port));
						if (!nameCharge || owner.SharedSubtypes.size() == owner.SharedSubtypes.capacity())
							return context.Fail(
								Status::LimitExceeded,
								"Group shared animator owner exceeds operation budget",
								port
							);
						GroupSubtypeOverlay overlay;
						overlay.NodeId = std::string(sourceId);
						overlay.Port = std::string(port);
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
						port,
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
					true
				);
			}

		} // namespace
		std::unique_ptr<GroupReplayAccess::Owner> CloneGroupReplay(
			const GroupReplayState &previous,
			size_t extraSlots,
			uint64_t revision,
			uint64_t maximumBytes,
			uint64_t destinationBytes,
			Diagnostic &diagnostic
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
					!Add(bytes, TextBytes(binding.Port)))
					return refuse();
			for (const auto &overlay : previous.SharedSubtypes()) {
				if (!Add(bytes, TextBytes(overlay.NodeId)) || !Add(bytes, TextBytes(overlay.Port)) ||
					(overlay.Fixed && !Add(bytes, RetainedPayloadBytes(*overlay.Fixed))) ||
					!Add(bytes, overlay.Keys.size() * sizeof(Keyframe)))
					return refuse();
				for (const auto &key : overlay.Keys) {
					const auto size = KeyframePayloadBytes(key);
					if (!size || !Add(bytes, *size - sizeof(Keyframe))) return refuse();
				}
			}
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
		bool ApplySourceAnimatorEdit(
			NodeContext &context,
			const GroupRefreshEvent &event,
			GroupReplayAccess::Owner &owner,
			const Document &document
		) {
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
		for (const auto &edit : edits) {
			const auto node =
				std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const auto &node) {
					return node.Id == edit.NodeId;
				});
			detail::NodeContext context(*node, *FindCatalogueEntry(node->Type), edit.At, candidate->Budget);
			if (!detail::ApplySourceAnimatorEdit(context, edit, *candidate, document))
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
		auto effects = [&](const auto &visit) {
			for (const auto &entry : replay.Entries()) {
				if (!visit(
						entry.NodeId, std::string_view("parent_value"), entry.ParentReset, entry.ParentKeys
					))
					return false;
				if (!replay.Binding(entry.NodeId) &&
					!visit(entry.NodeId, std::string_view("subtype"), entry.SubtypeStatic, entry.SubtypeKeys))
					return false;
			}
			for (const auto &shared : replay.SharedSubtypes())
				if (!visit(shared.NodeId, std::string_view(shared.Port), shared.Fixed, shared.Keys))
					return false;
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
		const auto projectedValue = [&](
			std::string_view id,
			std::string_view port,
			const std::optional<Value> &fixed,
			const std::vector<Keyframe> &keys
		) -> const Value * {
			if (fixed) return &*fixed;
			if (port != "parent_value" || keys.empty()) return nullptr;
			const auto node = std::find_if(authored.Nodes.begin(), authored.Nodes.end(), [&](const auto &value) {
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
		const size_t projectedValueCapacity = replay.Entries().size() * 2 + replay.SharedSubtypes().size();
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
		if (!reservation)
			return fail(Status::LimitExceeded, "Group authored projection admission failed");
		const uint64_t preadmitted = admitted;
		// Reserve the bounded effect index before allocating its scratch array.
		auto projectedValues = std::make_unique<ProjectedValue[]>(projectedValueCapacity);
		size_t projectedValueCount = 0;
		effects([&](std::string_view id, std::string_view port, const auto &fixed, const auto &keys) {
			if (projectedValue(id, port, fixed, keys))
				projectedValues[projectedValueCount++] = {id, port};
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
					if (key.NodeId != id || key.Port != port) {
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
						const bool existsInAuthored = std::any_of(
							node->Values.begin(), node->Values.end(), [&](const auto &value) {
								return value.Port == candidate.Port;
							}
						);
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
					if (!add(std::max(id.size(), std::string{}.capacity()) + 1) ||
						!add(std::max(port.size(), std::string{}.capacity()) + 1) ||
						!add(std::string{}.capacity() + 1)) {
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
		if (!effects([&](std::string_view, std::string_view, const auto &, const auto &keys) {
				for (const auto &key : keys) {
					const auto bytes = KeyframePayloadBytes(key);
					if (!bytes || !add(*bytes)) return false;
				}
				return true;
			}))
			return fail(Status::LimitExceeded, "Group authored effect keys exceed bounds");
		for (const auto &track : authored.Tracks)
			if (!add(std::max(track.NodeId.size(), std::string{}.capacity()) + 1) ||
				!add(std::max(track.Port.size(), std::string{}.capacity()) + 1) ||
				!add(std::max(track.End.size(), std::string{}.capacity()) + 1))
				return fail(Status::LimitExceeded, "Group authored track clone exceeds bounds");
		auto additionalAdmission = budget.Reserve(admitted - preadmitted);
		if (!additionalAdmission || !reservation->Merge(std::move(*additionalAdmission)))
			return fail(Status::LimitExceeded, "Group authored projection admission failed");
		core::Metrics::Count(
			"imagegraph.group_projection.admitted_payload_bytes", static_cast<double>(admitted)
		);
		core::Metrics::Count("imagegraph.group_projection.operations", 1);
		Document candidate = authored;
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
			finalKeys.insert(finalKeys.end(), keys.begin(), keys.end());
			if (!keys.empty() &&
				std::any_of(
					keys.begin(), keys.end(), [](const auto &key) { return key.Interpolation == "source"; }
				) &&
				std::none_of(finalTracks.begin(), finalTracks.end(), [&](const auto &track) {
					return track.NodeId == id && track.Port == port;
				}))
				finalTracks.push_back({std::string(id), std::string(port), "hold", -1});
			return true;
		});
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
				const auto value =
					std::find_if(node->Values.begin(), node->Values.end(), [&](const auto &value) {
						return value.Port == port;
					});
				return value != node->Values.end() && value->Data == *fixed;
			}
			size_t index = 0;
			for (const auto &key : document.Keyframes)
				if (key.NodeId == id && key.Port == port) {
					if (index >= keys.size() || key != keys[index++]) return false;
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
		for (const auto &shared : previous.SharedSubtypes())
			if (!matches(shared.NodeId, shared.Port, shared.Fixed, shared.Keys)) {
				diagnostic = {
					Status::InvalidValue,
					shared.NodeId,
					shared.Port,
					"Shared animator effect has not been projected into this document"
				};
				return diagnostic.Code;
			}
		const auto status = RebindGroupReplay(document, previous, revision, result, diagnostic, maximumBytes);
		if (status != Status::Ok) return status;
		auto *owner = detail::GroupReplayAccess::Get(result);
		if (!owner) return Status::Ok;
		uint64_t removed = owner->SharedSubtypes.capacity() * sizeof(GroupSubtypeOverlay);
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
			removed += std::max<size_t>(shared.NodeId.size(), 15) + 1;
			removed += std::max(shared.Port.size(), std::string{}.capacity()) + 1;
			clearKeys(shared.Keys);
			clearFixed(shared.Fixed);
		}
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
