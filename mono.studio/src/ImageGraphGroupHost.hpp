#pragma once

#include <engine/imagegraph/Catalogue.hpp>
#include <engine/imagegraph/GroupReplay.hpp>
#include <engine/imagegraph/SourceAxisTransition.hpp>
#include <engine/imagegraph/SourceModeTransition.hpp>

#include <algorithm>
#include <initializer_list>
#include <new>
#include <studio/ImageGraph.hpp>

namespace studio {

	// sampling borrows declarations; editor and constructor events refresh retained storage.
	struct ImageGraphGroupHost {
		engine::imagegraph::GroupReplayState Replay;
		uint64_t Revision = 0;
		uint64_t BorrowedBytes = 0;

		uint64_t Budget(
			engine::imagegraph::Diagnostic &error,
			std::initializer_list<const engine::imagegraph::Document *> documents = {},
			std::initializer_list<const engine::imagegraph::GroupReplayState *> states = {},
			uint64_t scratchBytes = 0,
			uint64_t maximumBytes = engine::imagegraph::Limits::MaximumEvaluationBytes
		) const {
			using namespace engine::imagegraph;
			uint64_t remaining = std::min(maximumBytes, Limits::MaximumEvaluationBytes);
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

		// Source boundary defaults mirror the compact animator storage, so native history
		// and source save retain the same declaration after a parent edit.
		bool NormalizeSourceParent(
			engine::imagegraph::Document &document,
			std::string_view nodeId,
			uint64_t headroom,
			engine::imagegraph::Diagnostic &error
		) const {
			using namespace engine::imagegraph;
			for (const auto &group : document.Groups) {
				for (const auto &port : group.Ports) {
					if (port.Direction != PortDirection::Input || port.ControlNodeId != nodeId ||
						port.JunctionId != std::string(nodeId) + "/parent-value")
						continue;
					auto node =
						std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const auto &n) {
							return n.Id == nodeId;
						});
					auto junction = std::find_if(
						document.Junctions.begin(), document.Junctions.end(), [&](const auto &j) {
							return j.Id == port.JunctionId;
						}
					);
					if (node == document.Nodes.end() || junction == document.Junctions.end()) return true;
					const bool animated = std::find(
											  node->SourceAnimatedInputs.begin(),
											  node->SourceAnimatedInputs.end(),
											  "parent_value"
										  ) != node->SourceAnimatedInputs.end();
					auto value = std::find_if(node->Values.begin(), node->Values.end(), [](const auto &v) {
						return v.Port == "parent_value";
					});
					if (animated) {
						if (value != node->Values.end()) node->Values.erase(value);
						junction->Default = -1.;
					} else if (value != node->Values.end()) {
						const auto key = std::find_if(
							document.Keyframes.begin(), document.Keyframes.end(), [&](const auto &k) {
								return k.NodeId == nodeId && k.Port == "parent_value";
							}
						);
						const Value &fixed = key == document.Keyframes.end() ? value->Data : key->Data;
						const auto bytes = ValueClonePayloadBytes(fixed);
						if (!bytes || *bytes > headroom / 2) {
							error = {
								Status::LimitExceeded,
								std::string(nodeId),
								"parent_value",
								"source Group default copies exceed the transaction budget"
							};
							return false;
						}
						Value replacement = fixed;
						value->Data = replacement;
						junction->Default = std::move(replacement);
					}
					const bool keyed =
						std::any_of(document.Keyframes.begin(), document.Keyframes.end(), [&](const auto &k) {
							return k.NodeId == nodeId && k.Port == "parent_value";
						});
					const bool linked =
						std::any_of(document.Links.begin(), document.Links.end(), [&](const auto &l) {
							return l.ToNode == port.JunctionId;
						});
					if (keyed && !linked)
						std::erase_if(document.Links, [&](const auto &l) {
							return l.FromNode == port.JunctionId && l.ToNode == nodeId &&
								   l.ToPort == "parent_value";
						});
					return true;
				}
			}
			return true;
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
				const auto prepareAllowance = Budget(error);
				if (!prepareAllowance ||
					RebindGroupReplay(document, Replay, revision, rebound, error, prepareAllowance) !=
						Status::Ok)
					return false;
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
				// grug restore callbacks need bound aliases, even when those aliases sit outside the
				// callback. this is native restore; source append still owns its separate load callback
				// order.
				if (!fresh.empty() && !rebound.InstancesBound() &&
					std::any_of(document.Nodes.begin(), document.Nodes.end(), [](const auto &node) {
						return !node.InstanceBase.empty();
					})) {
					GroupReplayState callbackBindings;
					const auto allowance = Budget(error, {}, {&Replay}, scratch);
					const auto status =
						document.SourceAnimators && !Replay.InstancesBound()
							? RestoreSourceAnimatorBindings(
								  document, rebound, revision, callbackBindings, error, allowance
							  )
							: BindGroupReplay(
								  document, bindings, rebound, revision, callbackBindings, error, allowance
							  );
					if (status != Status::Ok) return false;
					rebound = std::move(callbackBindings);
				}
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
				if (document.SourceAnimators && !Replay.InstancesBound()) {
					if (RestoreSourceAnimatorBindings(
							document, restored, revision, bound, error, Budget(error, {}, {&Replay}, scratch)
						) != Status::Ok)
						return false;
				} else if (BindGroupReplay(
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

		// owner-thread constructor events remain observable even when the following evaluation refuses.
		bool RetainAxisRead(
			const engine::imagegraph::Document &document,
			uint64_t revision,
			engine::imagegraph::EvaluationRequest &request,
			const engine::imagegraph::Diagnostic &receipt,
			engine::imagegraph::Diagnostic &error,
			uint64_t maximumBytes = engine::imagegraph::Limits::MaximumEvaluationBytes
		) {
			using namespace engine::imagegraph;
			if (receipt.Code != Status::SourceAxisInitializationRequired || Revision != revision ||
				request.GroupReplay != &Replay || request.GroupAuthoringRevision != revision ||
				!Replay.InstancesBound() || Replay.AuthoringRevision() != revision) {
				error = {
					Status::InvalidValue, {}, {}, "source getter initialization receipt is stale or not bound"
				};
				return false;
			}
			if (receipt.NodeId.empty() || receipt.Port.empty() ||
				receipt.NodeId.size() > Limits::MaximumTextBytes ||
				receipt.Port.size() > Limits::MaximumTextBytes) {
				error = {
					Status::LimitExceeded,
					{},
					{},
					"source getter initialization receipt identity exceeds bounds"
				};
				return false;
			}
			const auto *binding = Replay.Binding(receipt.NodeId, receipt.Port);
			const auto *overlay = Replay.SharedSubtype(receipt.NodeId, receipt.Port);
			if ((binding && binding->Axes.Storage != GroupAxisStorage::Uninitialized) ||
				(!binding && overlay && overlay->SeparatedVec2 && overlay->SeparatedVec2->Initialized)) {
				error = {
					Status::InvalidValue,
					receipt.NodeId,
					receipt.Port,
					"source getter initialization receipt already completed"
				};
				return false;
			}
			Diagnostic candidateError;
			auto maximum = Budget(candidateError, {}, {}, sizeof(Diagnostic), maximumBytes);
			for (const auto bytes :
				 {receipt.NodeId.capacity(), receipt.Port.capacity(), receipt.Message.capacity()}) {
				if (bytes >= maximum) {
					error = {
						Status::LimitExceeded, {}, {}, "source getter receipt leaves no constructor allowance"
					};
					return false;
				}
				maximum -= bytes;
			}
			if (!maximum) {
				error = std::move(candidateError);
				return false;
			}
			GroupReplayState initialized;
			const SourceAxisInitialization target{receipt.NodeId, receipt.Port};
			if (InitializeSourceVec2Axes(
					document, {&target, 1}, Replay, revision, initialized, candidateError, maximum
				) != Status::Ok) {
				error = std::move(candidateError);
				return false;
			}
			Replay = std::move(initialized);
			request.GroupReplay = &Replay;
			error = {};
			return true;
		}

		bool ProjectForSave(
			const engine::imagegraph::Document &document,
			uint64_t revision,
			engine::imagegraph::Document &projected,
			engine::imagegraph::Diagnostic &error,
			uint64_t maximumBytes = engine::imagegraph::Limits::MaximumEvaluationBytes
		) const {
			using namespace engine::imagegraph;
			if (Revision != revision || !Replay.InstancesBound() || Replay.AuthoringRevision() != revision) {
				error = {Status::InvalidValue, {}, {}, "source constructor save projection is stale"};
				return false;
			}
			const auto allowance = Budget(error, {}, {}, 0, maximumBytes);
			if (!allowance) return false;
			return ProjectGroupReplay(document, Replay, revision, projected, error, allowance) == Status::Ok;
		}

		bool ToggleAxes(
			engine::imagegraph::Document &document,
			ImageGraphHistory &history,
			uint64_t revision,
			const engine::imagegraph::SourceAxisTransition &transition,
			engine::imagegraph::EvaluationRequest request,
			engine::imagegraph::Diagnostic &error,
			uint64_t maximumBytes = engine::imagegraph::Limits::MaximumEvaluationBytes
		) try {
			using namespace engine::imagegraph;
			const auto initialAllowance = Budget(error, {&document}, {&Replay}, 0, maximumBytes);
			if (!initialAllowance) return false;
			ImageGraphGroupHost prepared;
			const auto allowance = Budget(error, {}, {&Replay}, 0, maximumBytes);
			if (!allowance) return false;
			prepared.BorrowedBytes = Limits::MaximumEvaluationBytes - allowance;
			prepared.Revision = Revision;
			if (RebindGroupReplay(
					document, Replay, revision, prepared.Replay, error, Budget(error, {}, {}, 0, maximumBytes)
				) != Status::Ok)
				return false;
			const auto planAllowance = prepared.Budget(error, {&document}, {&prepared.Replay}) / 2;
			if (!planAllowance) return false;
			Plan plan;
			if (Compile(document, plan, error, planAllowance) != Status::Ok) return false;
			// reserve the admitted compiler workspace until its plan leaves the preparation overlap.
			prepared.BorrowedBytes += planAllowance;
			if (!prepared.Prepare(document, plan, revision, request, error)) return false;
			plan = {};
			prepared.BorrowedBytes -= planAllowance;
			Document before, changed;
			GroupReplayState changedReplay, rebound;
			const auto projectAllowance = prepared.Budget(error);
			if (!projectAllowance ||
				ProjectGroupReplay(document, prepared.Replay, revision, before, error, projectAllowance) !=
					Status::Ok)
				return false;
			const auto transitionAllowance = prepared.Budget(error, {&before});
			if (!transitionAllowance || ToggleSourceAxes(
											document,
											prepared.Replay,
											revision,
											transition,
											request,
											changed,
											changedReplay,
											error,
											transitionAllowance
										) != Status::Ok)
				return false;
			const uint64_t nextRevision = revision == UINT64_MAX ? 1 : revision + 1;
			const auto rebindAllowance = prepared.Budget(error, {&document, &before}, {&prepared.Replay});
			if (!rebindAllowance || RebindProjectedGroupReplay(
										changed, changedReplay, nextRevision, rebound, error, rebindAllowance
									) != Status::Ok)
				return false;
			if (!history.TryRecord(before, changed)) {
				error = {
					Status::LimitExceeded,
					std::string(transition.NodeId),
					std::string(transition.Port),
					"source axis edit exceeds undo history budget"
				};
				return false;
			}
			document = std::move(changed);
			Replay = std::move(rebound);
			Revision = nextRevision;
			error = {};
			return true;
		} catch (const std::bad_alloc &) {
			error = {engine::imagegraph::Status::LimitExceeded, {}, {}, "Studio axis edit allocation failed"};
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
			GroupReplayState modeReplay, modeTransitionReplay, transitionedReplay;
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
						modeTransitionReplay,
						error,
						Budget(error, {&document}, {&Replay})
					) != Status::Ok ||
					RebindProjectedGroupReplay(
						transitioned,
						modeTransitionReplay,
						revision,
						transitionedReplay,
						error,
						Budget(error, {&document, &modeDocument}, {&Replay, &modeTransitionReplay})
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
			const auto parentHeadroom = Budget(error, {&document, &projected}, {&Replay, &edited});
			if (!parentHeadroom || !NormalizeSourceParent(projected, event.NodeId, parentHeadroom, error))
				return false;
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
