#pragma once

#include <engine/imagegraph/Catalogue.hpp>
#include <engine/imagegraph/GroupReplay.hpp>
#include <engine/imagegraph/SourceModeTransition.hpp>

#include <algorithm>
#include <initializer_list>
#include <new>
#include <studio/ImageGraph.hpp>

namespace studio {

	// Sampling borrows declarations. Only explicit editor events refresh them.
	struct ImageGraphGroupHost {
		engine::imagegraph::GroupReplayState Replay;
		uint64_t Revision = 0;
		uint64_t BorrowedBytes = 0;

		uint64_t Budget(
			engine::imagegraph::Diagnostic &error,
			std::initializer_list<const engine::imagegraph::Document *> documents = {},
			std::initializer_list<const engine::imagegraph::GroupReplayState *> states = {},
			uint64_t scratchBytes = 0
		) const {
			using namespace engine::imagegraph;
			uint64_t remaining = Limits::MaximumEvaluationBytes;
			const auto charge = [&](uint64_t bytes) {
				if (bytes >= remaining) {
					error = {
						Status::LimitExceeded,
						{},
						{},
						"Studio Group transaction exceeds its live payload budget"
					};
					return false;
				}
				remaining -= bytes;
				return true;
			};
			if (!charge(BorrowedBytes) || !charge(scratchBytes)) return 0;
			for (const auto *document : documents) {
				if (!document) continue;
				const auto bytes = DocumentRetainedPayloadBytes(*document);
				if (!bytes || !charge(*bytes)) return 0;
			}
			for (const auto *state : states)
				if (state && !charge(state->RetainedBytes())) return 0;
			return remaining;
		}

		static engine::imagegraph::GroupSubtypeAnimator Mode(
			const engine::imagegraph::Document &document,
			const engine::imagegraph::Node &node,
			std::string_view port
		) {
			using engine::imagegraph::GroupSubtypeAnimator;
			if (std::find(node.SourceStaticInputs.begin(), node.SourceStaticInputs.end(), port) !=
				node.SourceStaticInputs.end())
				return GroupSubtypeAnimator::Static;
			return std::find(node.SourceAnimatedInputs.begin(), node.SourceAnimatedInputs.end(), port) !=
							   node.SourceAnimatedInputs.end() ||
						   std::any_of(
							   document.Keyframes.begin(),
							   document.Keyframes.end(),
							   [&](const auto &key) { return key.NodeId == node.Id && key.Port == port; }
						   )
					   ? GroupSubtypeAnimator::Animated
					   : GroupSubtypeAnimator::Static;
		}

		void Clear() {
			Replay = {};
			Revision = 0;
			BorrowedBytes = 0;
		}

		bool Prepare(
			const engine::imagegraph::Document &document,
			const engine::imagegraph::Plan &plan,
			uint64_t revision,
			engine::imagegraph::EvaluationRequest &request,
			engine::imagegraph::Diagnostic &error
		) try {
			using namespace engine::imagegraph;
			if (Revision != revision) {
				GroupReplayState rebound, restored, bound;
				if (RebindGroupReplay(document, Replay, revision, rebound, error) != Status::Ok) return false;
				std::vector<GroupBootstrapTarget> fresh;
				std::vector<GroupSubtypeBinding> bindings;
				const uint64_t scratchBudget = Budget(error, {&document}, {&Replay, &rebound});
				if (!scratchBudget) return false;
				uint64_t bindingNames = 0;
				const auto admits = [&](uint64_t bytes) {
					if (bytes > scratchBudget) {
						error = {
							Status::LimitExceeded,
							{},
							{},
							"Studio Group bindings exceed their live payload budget"
						};
						return false;
					}
					return true;
				};
				for (const auto &node : document.Nodes) {
					if (node.Type == "pc.group_input" && !rebound.Find(node.Id)) {
						if (fresh.size() == fresh.capacity()) {
							const size_t capacity = std::max(size_t{8}, fresh.capacity() * 2);
							if (!admits(
									(fresh.capacity() + capacity) * sizeof(GroupBootstrapTarget) +
									bindings.capacity() * sizeof(GroupSubtypeBinding) + bindingNames
								))
								return false;
							fresh.reserve(capacity);
						}
						fresh.push_back({node.Id, Mode(document, node, "subtype")});
					}
					if (node.InstanceBase.empty()) continue;
					const Node *owner = &node;
					for (size_t hop = 0; !owner->InstanceBase.empty() && hop < document.Nodes.size(); ++hop) {
						const auto found =
							std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const auto &item) {
								return item.Id == owner->InstanceBase;
							});
						if (found == document.Nodes.end()) return false;
						owner = &*found;
					}
					const auto *entry = FindCatalogueEntry(node.Type);
					if (!entry || !HasNativeExecutor(node.Type)) continue;
					const auto append = [&](std::string_view port, int32_t sourceIndex) {
						if (sourceIndex < 0 || (node.Type == "pc.group_input" && port == "parent_value"))
							return true;
						const uint64_t names = std::max(node.Id.size(), size_t{15}) +
											   std::max(owner->Id.size(), size_t{15}) +
											   std::max(port.size(), size_t{15}) + 3;
						const size_t capacity = bindings.size() == bindings.capacity()
													? std::max(size_t{8}, bindings.capacity() * 2)
													: bindings.capacity();
						if (!admits(
								(bindings.capacity() + capacity) * sizeof(GroupSubtypeBinding) +
								fresh.capacity() * sizeof(GroupBootstrapTarget) + bindingNames + names
							))
							return false;
						if (capacity > bindings.capacity()) bindings.reserve(capacity);
						bindingNames += names;
						const Node *getter = &node;
						for (size_t hop = 0; !getter->InstanceBase.empty() && hop < document.Nodes.size();
							 ++hop) {
							if (std::find(
									getter->InstanceOverrides.begin(), getter->InstanceOverrides.end(), port
								) != getter->InstanceOverrides.end())
								break;
							const auto base = std::find_if(
								document.Nodes.begin(), document.Nodes.end(), [&](const auto &candidate) {
									return candidate.Id == getter->InstanceBase;
								}
							);
							if (base == document.Nodes.end()) return false;
							getter = &*base;
						}
						bindings.push_back(
							{node.Id,
							 owner->Id,
							 Mode(document, *getter, port),
							 Mode(document, *owner, port),
							 std::string(port)}
						);
						return true;
					};
					for (const auto &input : entry->Inputs)
						if (!append(input.Id, input.SourceIndex)) return false;
					for (const auto &input : node.DynamicInputs) {
						size_t group = 0;
						if (const auto *source = FindDynamicTemplate(*entry, input.Id, group))
							if (!append(input.Id, source->SourceIndex)) return false;
					}
				}
				uint64_t scratch = fresh.capacity() * sizeof(GroupBootstrapTarget) +
								   bindings.capacity() * sizeof(GroupSubtypeBinding);
				for (const auto &binding : bindings)
					scratch +=
						binding.NodeId.capacity() + binding.OwnerId.capacity() + binding.Port.capacity();
				if (RestoreGroupDeclarations(
						document,
						plan,
						fresh,
						request,
						rebound,
						revision,
						restored,
						error,
						Budget(error, {}, {&Replay}, scratch)
					) != Status::Ok)
					return false;
				rebound = {};
				if (BindGroupReplay(
						document,
						bindings,
						restored,
						revision,
						bound,
						error,
						Budget(error, {}, {&Replay}, scratch)
					) != Status::Ok)
					return false;
				Replay = std::move(bound);
				Revision = revision;
			}
			request.GroupReplay = &Replay;
			request.GroupAuthoringRevision = revision;
			return true;
		} catch (const std::bad_alloc &) {
			error = {
				engine::imagegraph::Status::LimitExceeded, {}, {}, "Studio Group host allocation failed"
			};
			return false;
		}

		bool Edit(
			engine::imagegraph::Document &document,
			ImageGraphHistory &history,
			uint64_t revision,
			engine::imagegraph::GroupRefreshEvent event,
			engine::imagegraph::Diagnostic &error,
			bool recordHistory = true
		) try {
			using namespace engine::imagegraph;
			const uint64_t nextRevision = revision == UINT64_MAX ? 1 : revision + 1;
			Plan plan;
			if (Compile(document, plan, error) != Status::Ok ||
				!Prepare(document, plan, revision, event.At, error))
				return false;
			GroupReplayState animatorEdited, controlState, edited, projectedState;
			Document controls, projected, modeDocument, transitioned;
			GroupReplayState modeReplay, transitionedReplay;
			const Document *source = &document;
			const GroupReplayState *sourceReplay = &Replay;
			const auto node =
				std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const auto &item) {
					return item.Id == event.NodeId;
				});
			const auto *declaration = Replay.Find(event.NodeId);
			const auto *catalogue = node == document.Nodes.end() ? nullptr : FindCatalogueEntry(node->Type);
			const auto *sourceInput = catalogue ? FindCatalogueInput(*catalogue, event.EditedPort) : nullptr;
			size_t dynamicGroup = 0;
			if (!sourceInput && catalogue)
				sourceInput = FindDynamicTemplate(*catalogue, event.EditedPort, dynamicGroup);
			const bool trigger = (declaration && event.EditedPort == "parent_value" &&
								  declaration->Domain.Kind == SourceSocketKind::Trigger) ||
								 (sourceInput && sourceInput->SourceKind == "Trigger");
			const auto *binding = Replay.Binding(event.NodeId, event.EditedPort);
			const auto getter = binding						   ? binding->Getter
								: node != document.Nodes.end() ? Mode(document, *node, event.EditedPort)
															   : GroupSubtypeAnimator::Static;
			if (node != document.Nodes.end() && event.LocalAnimated && (trigger || sourceInput) &&
				getter != GroupSubtypeAnimator::Animated) {
				const auto originalBytes = DocumentRetainedPayloadBytes(document);
				if (!originalBytes || !Budget(error, {&document}, {&Replay}, *originalBytes)) return false;
				modeDocument = document;
				auto &target = modeDocument.Nodes[size_t(node - document.Nodes.begin())];
				if (std::find(
						target.SourceStaticInputs.begin(), target.SourceStaticInputs.end(), event.EditedPort
					) == target.SourceStaticInputs.end())
					target.SourceStaticInputs.push_back(std::string(event.EditedPort));
				if (RebindGroupReplay(modeDocument, Replay, revision, modeReplay, error) != Status::Ok ||
					ToggleSourceInputMode(
						modeDocument,
						modeReplay,
						revision,
						{event.NodeId, event.EditedPort, true, GetFrameTime(event.At)},
						transitioned,
						error,
						Budget(error, {&document}, {&Replay})
					) != Status::Ok ||
					RebindProjectedGroupReplay(
						transitioned,
						modeReplay,
						revision,
						transitionedReplay,
						error,
						Budget(error, {&document, &modeDocument}, {&Replay})
					) != Status::Ok)
					return false;
				source = &transitioned;
				sourceReplay = &transitionedReplay;
				modeDocument = {};
				modeReplay = {};
			}
			const bool modeChanged = source != &document;
			if (ReplayGroupAnimatorEdits(
					*source,
					{&event, 1},
					*sourceReplay,
					revision,
					animatorEdited,
					error,
					Budget(error, {modeChanged ? &document : nullptr}, {modeChanged ? &Replay : nullptr})
				) != Status::Ok)
				return false;
			if (ProjectGroupReplay(
					*source,
					animatorEdited,
					revision,
					controls,
					error,
					Budget(
						error,
						{modeChanged ? &document : nullptr},
						{&Replay, modeChanged ? &transitionedReplay : nullptr}
					)
				) != Status::Ok)
				return false;
			if (RebindProjectedGroupReplay(
					controls,
					animatorEdited,
					revision,
					controlState,
					error,
					Budget(
						error,
						{&document, modeChanged ? &transitioned : nullptr},
						{&Replay, modeChanged ? &transitionedReplay : nullptr}
					)
				) != Status::Ok)
				return false;
			animatorEdited = {};
			transitioned = {};
			transitionedReplay = {};
			if (node != document.Nodes.end() && node->Type != "pc.group_input") {
				if (RebindProjectedGroupReplay(
						controls,
						controlState,
						nextRevision,
						projectedState,
						error,
						Budget(error, {&document}, {&Replay})
					) != Status::Ok)
					return false;
				if (recordHistory && !history.TryRecord(document, controls)) {
					error = {
						Status::LimitExceeded,
						std::string(event.NodeId),
						std::string(event.EditedPort),
						"Group undo transition exceeds its history budget"
					};
					return false;
				}
				document = std::move(controls);
				Replay = std::move(projectedState);
				// A mode toggle changes getter bindings as well as the projected key map.
				Revision = modeChanged ? 0 : nextRevision;
				return true;
			}
			if (Compile(controls, plan, error) != Status::Ok || ReplayGroupRefresh(
																	controls,
																	plan,
																	{&event, 1},
																	controlState,
																	revision,
																	edited,
																	error,
																	Budget(error, {&document}, {&Replay})
																) != Status::Ok)
				return false;
			controlState = {};
			if (ProjectGroupReplay(
					controls, edited, revision, projected, error, Budget(error, {&document}, {&Replay})
				) != Status::Ok)
				return false;
			controls = {};
			if (RebindProjectedGroupReplay(
					projected,
					edited,
					nextRevision,
					projectedState,
					error,
					Budget(error, {&document}, {&Replay})
				) != Status::Ok)
				return false;
			if (recordHistory && !history.TryRecord(document, projected)) {
				error = {
					Status::LimitExceeded,
					std::string(event.NodeId),
					std::string(event.EditedPort),
					"Group undo transition exceeds its history budget"
				};
				return false;
			}
			document = std::move(projected);
			Replay = std::move(projectedState);
			Revision = modeChanged ? 0 : nextRevision;
			return true;
		} catch (const std::bad_alloc &) {
			error = {
				engine::imagegraph::Status::LimitExceeded,
				{},
				{},
				"Studio Group transaction allocation failed"
			};
			return false;
		}
	};
} // namespace studio
