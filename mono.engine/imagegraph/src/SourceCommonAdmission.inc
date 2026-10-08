// Included after the group lifecycle helpers. Candidate admission never
// executes a source callback.
namespace detail {
	namespace common_admission {
		Status Fail(Diagnostic &diagnostic, Status status, const char *message) {
			SetDiagnostic(diagnostic, status, message);
			return status;
		}
		bool Text(uint64_t &bytes, const std::string &text) {
			return text.size() <= Limits::MaximumTextBytes && AddBytes(bytes, text.capacity());
		}
		template <class T> bool Slots(uint64_t &bytes, const std::vector<T> &rows) {
			return AddArrayBytes(bytes, rows.capacity(), sizeof(T));
		}
		std::optional<uint64_t> PlanBytes(const Plan &plan) {
			uint64_t bytes = sizeof(plan);
			if (plan.NodeOrder.size() > Limits::MaximumNodes ||
				plan.OutputNodes.size() > Limits::MaximumOutputs ||
				plan.EffectiveLinks.size() > Limits::MaximumLinks ||
				plan.ResolvedInputs.size() > Limits::MaximumNodes * Limits::MaximumPropertiesPerNode ||
				plan.SourceCommonRoutes.size() > Limits::MaximumLinks)
				return std::nullopt;
			if (!Slots(bytes, plan.NodeOrder) || !Slots(bytes, plan.OutputNodes) ||
				!Slots(bytes, plan.EffectiveLinks) || !Slots(bytes, plan.ResolvedInputs) ||
				!Slots(bytes, plan.GroupSurfaceDependencies) || !Slots(bytes, plan.InlineOwnerDependencies) ||
				!Slots(bytes, plan.InlineControlDependencies) || !Slots(bytes, plan.PcxNamedDependencies) ||
				!Slots(bytes, plan.SourceCommonRoutes))
				return std::nullopt;
			if (bytes > Limits::MaximumEvaluationBytes) return std::nullopt;
			for (const auto &row : plan.EffectiveLinks)
				if (!Text(bytes, row.FromNode) || !Text(bytes, row.FromPort) || !Text(bytes, row.ToNode) ||
					!Text(bytes, row.ToPort))
					return std::nullopt;
			for (const auto &row : plan.ResolvedInputs)
				if (!Text(bytes, row.NodeId) || !Text(bytes, row.Port) || !ValidRuntimeValue(row.Data) ||
					!AddBytes(bytes, RetainedPayloadBytes(row.Data)))
					return std::nullopt;
			for (const auto &row : plan.GroupSurfaceDependencies)
				if (!Text(bytes, row.Port)) return std::nullopt;
			for (const auto &row : plan.PcxNamedDependencies)
				if (!Text(bytes, row.Name) || !Text(bytes, row.Port)) return std::nullopt;
			for (const auto &row : plan.SourceCommonRoutes)
				if (!Text(bytes, row.OwnerId) || !Text(bytes, row.NodeId) || !Text(bytes, row.Port) ||
					!Text(bytes, row.SourcePort))
					return std::nullopt;
			return bytes;
		}
		Status Journals(const GroupRenderSession &session, Diagnostic &diagnostic) {
			if (ValidateCacheGroupReplay(session.Outputs, Limits::MaximumEvaluationBytes, diagnostic) !=
					Status::Ok ||
				ValidateSimulationReplay(
					session.Replay.Simulation, Limits::MaximumEvaluationBytes, diagnostic
				) != Status::Ok ||
				ValidateSurfaceFrameReplay(
					session.Replay.Surfaces, Limits::MaximumEvaluationBytes, diagnostic
				) != Status::Ok ||
				ValidateRandomReplay(session.Replay.Random, Limits::MaximumEvaluationBytes, diagnostic) !=
					Status::Ok ||
				ValidateDataReplay(session.Replay.Data, Limits::MaximumEvaluationBytes, diagnostic) !=
					Status::Ok ||
				ValidateRigidReplay(session.Replay.Rigid, Limits::MaximumEvaluationBytes, diagnostic) !=
					Status::Ok)
				return diagnostic.Code;
			if (session.Nodes.size() > Limits::MaximumNodes ||
				session.Purities.size() > Limits::MaximumGroups ||
				RetainedGroupRenderSessionBytes(session) == UINT64_MAX ||
				RetainedSourceCommonMembershipBytes(session) == UINT64_MAX)
				return Fail(
					diagnostic, Status::LimitExceeded, "common session retained records exceed bounds"
				);
			for (size_t index = 0; index < session.Nodes.size(); ++index) {
				const auto &row = session.Nodes[index];
				if (row.NodeId.empty() || row.NodeId.size() > Limits::MaximumTextBytes ||
					row.GroupId.size() > Limits::MaximumTextBytes ||
					row.InstanceBase.size() > Limits::MaximumTextBytes ||
					row.SourceParentInputBase.size() > Limits::MaximumTextBytes ||
					(row.FrameActivity != SourceFrameActivity::Unknown &&
					 row.FrameActivity != SourceFrameActivity::Static &&
					 row.FrameActivity != SourceFrameActivity::FrameDriven) ||
					std::any_of(session.Nodes.begin(), session.Nodes.begin() + index, [&](const auto &old) {
						return old.NodeId == row.NodeId;
					}))
					return Fail(
						diagnostic, Status::InvalidValue, "common native readiness record is invalid"
					);
			}
			for (size_t index = 0; index < session.Purities.size(); ++index) {
				const auto &row = session.Purities[index];
				if (row.GroupId.empty() || row.GroupId.size() > Limits::MaximumTextBytes ||
					row.ParentId.size() > Limits::MaximumTextBytes ||
					row.OwnerNodeId.size() > Limits::MaximumTextBytes ||
					row.InstanceBase.size() > Limits::MaximumTextBytes ||
					(row.State != SourceGroupPurity::Unknown && row.State != SourceGroupPurity::Nonpure &&
					 row.State != SourceGroupPurity::Pure) ||
					std::any_of(
						session.Purities.begin(), session.Purities.begin() + index, [&](const auto &old) {
							return old.GroupId == row.GroupId;
						}
					))
					return Fail(diagnostic, Status::InvalidValue, "common cached purity record is invalid");
			}
			return Status::Ok;
		}
		Status
		Membership(const Document &document, const GroupRenderSession &session, Diagnostic &diagnostic) {
			if (session.SourceCommonBindings.size() != document.SourceCommonOwners.size() ||
				session.Common.Owners.size() != document.SourceCommonOwners.size() ||
				session.SourceCommonInputs.size() != document.SourceCommonOwners.size())
				return Fail(
					diagnostic, Status::InvalidValue, "common runtime requires explicit owner initialization"
				);
			for (size_t index = 0; index < document.SourceCommonOwners.size(); ++index) {
				const auto &owner = document.SourceCommonOwners[index];
				const auto &binding = session.SourceCommonBindings[index];
				const auto &socket = session.Common.Owners[index];
				const auto &inputs = session.SourceCommonInputs[index];
				if (binding.SourceOwnerId != owner.SourceOwnerId || binding.SourceType != owner.SourceType ||
					binding.NativeOwnerKind != owner.NativeOwnerKind ||
					binding.NativeOwnerId != owner.NativeOwnerId || socket.OwnerId != owner.SourceOwnerId ||
					socket.OwnerType != owner.SourceType || inputs.OwnerId != owner.SourceOwnerId ||
					inputs.OwnerType != owner.SourceType)
					return Fail(
						diagnostic, Status::InvalidValue, "common runtime source membership is stale"
					);
			}
			if (!session.CommonAnimators.Bindings.empty() ||
				session.CommonAnimators.Detached.size() != session.CommonAnimators.DetachedValues.size())
				return Fail(diagnostic, Status::InvalidValue, "common local writer records are inconsistent");
			for (size_t index = 0; index < session.CommonAnimators.Detached.size(); ++index) {
				const auto &writer = session.CommonAnimators.Detached[index];
				const auto &payload = session.CommonAnimators.DetachedValues[index];
				if (writer.OwnerId != payload.NodeId || writer.Id != payload.Port ||
					writer.Type != ValueType::Boolean || writer.OriginalPort != "pxcx.update_in_trigger" ||
					(payload.Fixed && !std::holds_alternative<bool>(*payload.Fixed)) ||
					std::any_of(
						payload.Keys.begin(),
						payload.Keys.end(),
						[](const auto &key) { return !std::holds_alternative<bool>(key.Data); }
					) ||
					std::any_of(
						session.CommonAnimators.Detached.begin(),
						session.CommonAnimators.Detached.begin() + index,
						[&](const auto &old) { return old.OwnerId == writer.OwnerId && old.Id == writer.Id; }
					))
					return Fail(diagnostic, Status::InvalidValue, "common local writer payload is invalid");
			}
			for (const auto &owner : document.SourceCommonOwners)
				if (std::none_of(
						session.CommonAnimators.Detached.begin(),
						session.CommonAnimators.Detached.end(),
						[&](const auto &writer) {
							return writer.OwnerId == owner.UpdateAnimatorOwnerId &&
								   writer.Id == owner.UpdateAnimatorPort;
						}
					))
					return Fail(diagnostic, Status::InvalidValue, "common local Update writer is absent");
			return Status::Ok;
		}
		template <class T> bool Span(uint64_t &bytes, std::span<const T> rows, size_t maximum) {
			return rows.size() <= maximum && AddArrayBytes(bytes, rows.size(), sizeof(T));
		}
		bool Audio(
			uint64_t &bytes,
			const std::vector<double> &samples,
			const std::vector<std::vector<double>> &channels,
			size_t maximumSamples = Limits::MaximumAudioSamplesPerFrame
		) {
			if (!ValidAudioPlanes(samples, channels, maximumSamples) || !Slots(bytes, samples) ||
				!Slots(bytes, channels))
				return false;
			for (const auto &channel : channels)
				if (!Slots(bytes, channel)) return false;
			return true;
		}
		Status Borrowed(
			const Document &document,
			const EvaluationRequest &request,
			const GroupRenderSession &prior,
			uint64_t &bytes,
			Diagnostic &diagnostic
		) {
			bytes = sizeof(request);
			if ((request.SourceCommon && request.SourceCommon != &prior.Common) ||
				(request.SourceCommonAnimators && request.SourceCommonAnimators != &prior.CommonAnimators))
				return Fail(
					diagnostic,
					Status::InvalidValue,
					"common getter views must use the selected session owner"
				);
			if (!Span(bytes, request.AudioFrames, Limits::MaximumNodes) ||
				!Span(bytes, request.AudioClips, Limits::MaximumNodes) ||
				!Span(bytes, request.ImageSources, Limits::MaximumNodes) ||
				!Span(bytes, request.HostCaptures, Limits::MaximumNodes) ||
				!Span(bytes, request.FontObservations, Limits::MaximumNodes) ||
				!Span(bytes, request.PcxObservations, Limits::MaximumArrayElements) ||
				!Span(bytes, request.RandomEntropy, Limits::MaximumArrayElements) ||
				!Span(bytes, request.SimulationCacheCaptures, Limits::MaximumNodes) ||
				!Span(bytes, request.SourceTunnelRegistryObservations, Limits::MaximumNodes) ||
				request.ProjectName.size() > Limits::MaximumTextBytes ||
				!AddBytes(bytes, request.ProjectName.size()))
				return Fail(
					diagnostic, Status::LimitExceeded, "common borrowed recording count exceeds bounds"
				);
			if (ValidateAudioCaptureFrames(request.AudioFrames, diagnostic) != Status::Ok ||
				!ValidateSourceTunnelRegistryObservations(
					document, request.SourceTunnelRegistryObservations, diagnostic
				))
				return diagnostic.Code;
			for (const auto &row : request.AudioFrames)
				if (!Text(bytes, row.SourceId) || !Audio(bytes, row.Samples, row.Channels))
					return Fail(diagnostic, Status::InvalidValue, "common audio recording is invalid");
			for (const auto &row : request.AudioClips)
				if (!Text(bytes, row.SourceId) ||
					!Audio(bytes, row.Data.Samples, row.Data.Channels, Limits::MaximumAudioClipSamples) ||
					!std::isfinite(row.Data.SampleRate) || row.Data.SampleRate < 0)
					return Fail(diagnostic, Status::InvalidValue, "common audio clip is invalid");
			for (const auto &row : request.ImageSources)
				if (!Text(bytes, row.SourceId) ||
					!ValidSurfaceLayout(
						row.Data, request.MaximumImageDimension, Limits::MaximumOutputBytes
					) ||
					!AddBytes(bytes, row.Data.Pixels.capacity()))
					return Fail(diagnostic, Status::InvalidValue, "common image recording is invalid");
			for (const auto &row : request.HostCaptures) {
				const auto payload = HostCaptureRetainedPayloadBytes(row);
				if (!payload || *payload < sizeof(row) || !AddBytes(bytes, *payload - sizeof(row)))
					return Fail(diagnostic, Status::LimitExceeded, "common host recording exceeds bounds");
			}
			uint64_t builtin = 0;
			if (ValidateBuiltinRandomCaptures(
					request.BuiltinRandomCaptures, Limits::MaximumEvaluationBytes, builtin, diagnostic
				) != Status::Ok)
				return diagnostic.Code;
			if (!AddBytes(bytes, builtin))
				return Fail(diagnostic, Status::LimitExceeded, "common builtin recording bytes overflow");
			if (request.SourceFonts) {
				const auto payload = SourceFontContextRetainedBytes(*request.SourceFonts);
				if (!payload || !AddBytes(bytes, *payload))
					return Fail(diagnostic, Status::LimitExceeded, "common font context exceeds bounds");
			}
			for (const auto &row : request.FontObservations) {
				const auto payload = SourceFontObservationRetainedBytes(row);
				if (!payload || *payload < sizeof(row) || !AddBytes(bytes, *payload - sizeof(row)))
					return Fail(diagnostic, Status::LimitExceeded, "common font observation exceeds bounds");
			}
			for (const auto &row : request.PcxObservations)
				if (!Text(bytes, row.Port) || !ValidRuntimeValue(row.Data) ||
					!AddBytes(bytes, RetainedPayloadBytes(row.Data)))
					return Fail(diagnostic, Status::InvalidValue, "common PCX observation is invalid");
			for (const auto &row : request.RandomEntropy)
				if (!Text(bytes, row.NodeId) || row.Tick > Limits::MaximumTick ||
					!std::isfinite(row.Subframe) || row.Subframe < 0 || row.Subframe >= 1)
					return Fail(diagnostic, Status::InvalidValue, "common entropy observation is invalid");
			for (const auto &row : request.SourceTunnelRegistryObservations)
				if (!Text(bytes, row.NodeId) || !Text(bytes, row.Name))
					return Fail(
						diagnostic, Status::LimitExceeded, "common tunnel observation exceeds bounds"
					);
			for (auto id : request.SimulationCacheCaptures)
				if (!AddBytes(bytes, id.size()))
					return Fail(diagnostic, Status::LimitExceeded, "common cache capture bytes overflow");
			if (request.GroupReplay &&
				(request.GroupReplay->AuthoringRevision() != request.GroupAuthoringRevision ||
				 (!request.GroupReplay->InstancesBound() &&
				  std::any_of(document.Nodes.begin(), document.Nodes.end(), [](const auto &node) {
					  return !node.InstanceBase.empty() || !node.SourceParentInputBase.empty();
				  }))))
				return Fail(diagnostic, Status::InvalidValue, "common animator owner is stale or unbound");
			if (request.GroupReplay && !AddBytes(bytes, request.GroupReplay->RetainedBytes()))
				return Fail(diagnostic, Status::LimitExceeded, "common group animator bytes overflow");
			if (request.GroupRender && request.GroupRender != &prior) {
				if (Journals(*request.GroupRender, diagnostic) != Status::Ok) return diagnostic.Code;
				if (!AddBytes(bytes, RetainedGroupRenderSessionBytes(*request.GroupRender)))
					return Fail(diagnostic, Status::LimitExceeded, "common held owner bytes overflow");
			}
			if (request.SourceTunnelPreviousOutputs &&
				request.SourceTunnelPreviousOutputs != &prior.Outputs &&
				(!request.GroupRender ||
				 request.SourceTunnelPreviousOutputs != &request.GroupRender->Outputs)) {
				if (ValidateCacheGroupReplay(
						*request.SourceTunnelPreviousOutputs, Limits::MaximumEvaluationBytes, diagnostic
					) != Status::Ok)
					return diagnostic.Code;
				if (!AddBytes(bytes, RetainedCacheGroupReplayBytes(*request.SourceTunnelPreviousOutputs)))
					return Fail(diagnostic, Status::LimitExceeded, "common held tunnel bytes overflow");
			}
			if (request.SliceStackReplay) {
				if (ValidateSliceStackReplay(
						*request.SliceStackReplay, Limits::MaximumEvaluationBytes, diagnostic
					) != Status::Ok)
					return diagnostic.Code;
				if (!AddBytes(bytes, RetainedSliceStackReplayBytes(*request.SliceStackReplay)))
					return Fail(diagnostic, Status::LimitExceeded, "common slice history bytes overflow");
			}
			if (request.SourceFrameCacheLoads && request.SourceFrameCacheLoads != &prior.Replay.Data &&
				request.SourceFrameCacheLoads != request.DataReplay &&
				(!request.GroupRender ||
				 request.SourceFrameCacheLoads != &request.GroupRender->Replay.Data)) {
				if (ValidateDataReplay(
						*request.SourceFrameCacheLoads, Limits::MaximumEvaluationBytes, diagnostic
					) != Status::Ok)
					return diagnostic.Code;
				if (!AddBytes(bytes, RetainedDataReplayBytes(*request.SourceFrameCacheLoads)))
					return Fail(diagnostic, Status::LimitExceeded, "common frame-cache bytes overflow");
			}
			const auto journal =
				[&](const auto *incoming, const auto &held, const auto *other, auto validate, auto retained) {
					if (!incoming || incoming == &held || (other && incoming == other)) return Status::Ok;
					if (validate(*incoming, Limits::MaximumEvaluationBytes, diagnostic) != Status::Ok)
						return diagnostic.Code;
					return AddBytes(bytes, retained(*incoming))
							   ? Status::Ok
							   : Fail(
									 diagnostic,
									 Status::LimitExceeded,
									 "common canonical journal bytes overflow"
								 );
				};
			if (journal(
					request.SimulationReplay,
					prior.Replay.Simulation,
					request.GroupRender ? &request.GroupRender->Replay.Simulation : nullptr,
					ValidateSimulationReplay,
					RetainedSimulationReplayBytes
				) != Status::Ok)
				return diagnostic.Code;
			if (journal(
					request.SurfaceReplay,
					prior.Replay.Surfaces,
					request.GroupRender ? &request.GroupRender->Replay.Surfaces : nullptr,
					ValidateSurfaceFrameReplay,
					RetainedSurfaceFrameReplayBytes
				) != Status::Ok)
				return diagnostic.Code;
			if (journal(
					request.RandomReplay,
					prior.Replay.Random,
					request.GroupRender ? &request.GroupRender->Replay.Random : nullptr,
					ValidateRandomReplay,
					RetainedRandomReplayBytes
				) != Status::Ok)
				return diagnostic.Code;
			if (journal(
					request.DataReplay,
					prior.Replay.Data,
					request.GroupRender ? &request.GroupRender->Replay.Data : nullptr,
					ValidateDataReplay,
					RetainedDataReplayBytes
				) != Status::Ok)
				return diagnostic.Code;
			if (journal(
					request.RigidReplay,
					prior.Replay.Rigid,
					request.GroupRender ? &request.GroupRender->Replay.Rigid : nullptr,
					ValidateRigidReplay,
					RetainedRigidReplayBytes
				) != Status::Ok)
				return diagnostic.Code;

			return Status::Ok;
		}
		Status NativeMetadata(
			const Document &document,
			const GroupRenderSession &prior,
			bool cold,
			SourceNodeInitialState newState,
			GroupRenderSession &working,
			EvaluationBudget &budget,
			AllocationReservation &candidateCharge,
			Diagnostic &diagnostic
		) {
			uint64_t metadataBytes = document.Nodes.size() * sizeof(GroupRenderReadiness) +
									 document.Groups.size() * sizeof(GroupRenderPurity);
			for (const auto &node : document.Nodes)
				for (const auto *text :
					 {&node.Id, &node.GroupId, &node.InstanceBase, &node.SourceParentInputBase})
					if (!AddBytes(metadataBytes, std::max(text->size(), std::string{}.capacity())))
						return Fail(diagnostic, Status::LimitExceeded, "common readiness bytes overflow");
			for (const auto &group : document.Groups)
				for (const auto *text : {&group.Id, &group.ParentId, &group.OwnerNodeId, &group.InstanceBase})
					if (!AddBytes(metadataBytes, std::max(text->size(), std::string{}.capacity())))
						return Fail(diagnostic, Status::LimitExceeded, "common purity bytes overflow");
			auto metadata = budget.Reserve(metadataBytes);
			if (!metadata)
				return Fail(diagnostic, Status::LimitExceeded, "common native metadata exceeds live bytes");
			const uint64_t comparisons =
				document.Nodes.size() *
				uint64_t(prior.Nodes.size() + prior.Outputs.Nodes.size() + document.Groups.size());
			if (comparisons > 16'000'000)
				return Fail(
					diagnostic, Status::LimitExceeded, "common native reconciliation work exceeds bounds"
				);
			if (InitializeGroupRenderOutputs(document, {}, working.Outputs, budget.Available(), diagnostic) !=
				Status::Ok)
				return diagnostic.Code;
			auto constructors = budget.Reserve(RetainedCacheGroupReplayBytes(working.Outputs));
			if (!constructors)
				return Fail(
					diagnostic, Status::LimitExceeded, "common native constructors exceed live bytes"
				);
			working.Nodes.reserve(document.Nodes.size());
			std::array<bool, Limits::MaximumNodes> same{};
			std::array<bool, Limits::MaximumGroups> changed{};
			const auto changedScope = [&](std::string_view id) {
				for (size_t index = 0; index < document.Groups.size(); ++index)
					if (document.Groups[index].Id == id) changed[index] = true;
			};
			for (size_t index = 0; index < document.Nodes.size(); ++index) {
				const auto &node = document.Nodes[index];
				const auto old = std::find_if(prior.Nodes.begin(), prior.Nodes.end(), [&](const auto &row) {
					return row.NodeId == node.Id;
				});
				const auto held = std::find_if(
					prior.Outputs.Nodes.begin(), prior.Outputs.Nodes.end(), [&](const auto &row) {
						return row.NodeId == node.Id;
					}
				);
				const bool retained =
					!cold && held != prior.Outputs.Nodes.end() && held->NodeType == node.Type &&
					std::all_of(held->Outputs.begin(), held->Outputs.end(), [&](const auto &port) {
						return GroupHeldPortCompatible(document, node, port);
					});
				if (retained) {
					auto destination = std::find_if(
						working.Outputs.Nodes.begin(), working.Outputs.Nodes.end(), [&](const auto &row) {
							return row.NodeId == node.Id;
						}
					);
					if (destination == working.Outputs.Nodes.end())
						return Fail(
							diagnostic, Status::InvalidValue, "common constructor output owner is absent"
						);
					// Refusal-only sockets are authoritative held state too; copying does not
					// require Data.
					CacheGroupReplayNode replacement = *held;
					*destination = std::move(replacement);
				}
				same[index] = retained && old != prior.Nodes.end() && old->GroupId == node.GroupId &&
							  old->InstanceBase == node.InstanceBase &&
							  old->SourceParentInputBase == node.SourceParentInputBase;
				working.Nodes.push_back(
					{node.Id,
					 retained && old != prior.Nodes.end() && old->Rendered,
					 retained && old != prior.Nodes.end() ? old->FrameActivity
														  : SourceInitialFrameActivity(node.Type, newState),
					 node.GroupId,
					 node.InstanceBase,
					 node.SourceParentInputBase}
				);
				if (!same[index]) {
					changedScope(node.GroupId);
					if (old != prior.Nodes.end()) changedScope(old->GroupId);
				}
			}
			auto indicesCharge =
				budget.Reserve(document.Nodes.size() * sizeof(std::pair<std::string_view, size_t>));
			if (!indicesCharge)
				return Fail(diagnostic, Status::LimitExceeded, "common membership index exceeds live bytes");
			const EvaluationNodeIndices indices(document);
			uint64_t hops = 0;
			for (const auto &node : document.Nodes) {
				const Node *base = &node;
				for (size_t hop = 0; hop < document.Nodes.size(); ++hop) {
					if (++hops > 16'000'000)
						return Fail(
							diagnostic,
							Status::LimitExceeded,
							"common instance reconciliation exceeds work bounds"
						);
					if (!same[indices.at(base->Id)]) {
						changedScope(node.GroupId);
						break;
					}
					const auto alias = !base->InstanceBase.empty()
										   ? std::string_view(base->InstanceBase)
										   : std::string_view(base->SourceParentInputBase);
					if (alias.empty()) break;
					const auto next = indices.find(alias);
					if (!next) {
						changedScope(node.GroupId);
						break;
					}
					base = &document.Nodes[*next];
				}
			}
			for (const auto &old : prior.Nodes)
				if (!indices.find(old.NodeId)) changedScope(old.GroupId);
			for (const auto &group : document.Groups) {
				const auto old =
					std::find_if(prior.Purities.begin(), prior.Purities.end(), [&](const auto &row) {
						return row.GroupId == group.Id;
					});
				if (old == prior.Purities.end() || old->ParentId != group.ParentId) {
					changedScope(group.ParentId);
					if (old != prior.Purities.end()) changedScope(old->ParentId);
				}
				if (!group.OwnerNodeId.empty()) {
					const auto owner = indices.find(group.OwnerNodeId);
					if (!owner || !same[*owner]) changedScope(group.Id);
				}
			}
			for (const auto &old : prior.Purities)
				if (std::none_of(document.Groups.begin(), document.Groups.end(), [&](const auto &group) {
						return group.Id == old.GroupId;
					}))
					changedScope(old.ParentId);
			working.Purities.reserve(document.Groups.size());
			for (size_t index = 0; index < document.Groups.size(); ++index) {
				const auto &group = document.Groups[index];
				const auto old =
					std::find_if(prior.Purities.begin(), prior.Purities.end(), [&](const auto &row) {
						return row.GroupId == group.Id;
					});
				const bool matching = !cold && old != prior.Purities.end() && !changed[index] &&
									  old->ParentId == group.ParentId &&
									  old->OwnerNodeId == group.OwnerNodeId &&
									  old->InstanceBase == group.InstanceBase &&
									  old->AuthoredPureFunction == group.PureFunction;
				const auto state = cold || old == prior.Purities.end()
									   ? SourceGroupPurity::Nonpure
									   : (matching ? old->State : SourceGroupPurity::Unknown);
				working.Purities.push_back(
					{group.Id,
					 group.ParentId,
					 group.OwnerNodeId,
					 group.InstanceBase,
					 group.PureFunction,
					 state}
				);
			}
			if (!candidateCharge.Merge(std::move(*metadata)) ||
				!candidateCharge.Merge(std::move(*constructors)))
				std::terminate();
			return Status::Ok;
		}
		Status NativeMembership(
			const Document &document, const GroupRenderSession &session, Diagnostic &diagnostic
		) {
			if (session.Nodes.size() != document.Nodes.size() ||
				session.Outputs.Nodes.size() != document.Nodes.size() ||
				session.Purities.size() != document.Groups.size())
				return Fail(
					diagnostic,
					Status::InvalidValue,
					"common native membership requires explicit reconciliation"
				);
			for (size_t index = 0; index < document.Nodes.size(); ++index) {
				const auto &node = document.Nodes[index];
				const auto &ready = session.Nodes[index];
				const auto &held = session.Outputs.Nodes[index];
				if (ready.NodeId != node.Id || ready.GroupId != node.GroupId ||
					ready.InstanceBase != node.InstanceBase ||
					ready.SourceParentInputBase != node.SourceParentInputBase || held.NodeId != node.Id ||
					held.NodeType != node.Type ||
					!std::all_of(held.Outputs.begin(), held.Outputs.end(), [&](const auto &port) {
						return GroupHeldPortCompatible(document, node, port);
					}))
					return Fail(
						diagnostic,
						Status::InvalidValue,
						"common native readiness or held output membership is stale"
					);
			}
			for (size_t index = 0; index < document.Groups.size(); ++index) {
				const auto &group = document.Groups[index];
				const auto &row = session.Purities[index];
				if (row.GroupId != group.Id || row.ParentId != group.ParentId ||
					row.OwnerNodeId != group.OwnerNodeId || row.InstanceBase != group.InstanceBase ||
					row.AuthoredPureFunction != group.PureFunction)
					return Fail(diagnostic, Status::InvalidValue, "common cached purity membership is stale");
			}
			return Status::Ok;
		}
		// Validators allocate sorting tables only. Reserve their largest live table on the shared ledger.
		std::optional<uint64_t>
		ValidationWorkspace(const EvaluationRequest &request, const GroupRenderSession &prior) {
			uint64_t largest = 0;
			const auto cache = [&](const CacheGroupReplayState &state) {
				if (state.Nodes.size() > Limits::MaximumNodes || state.Owners.size() > Limits::MaximumNodes)
					return false;
				for (const auto &node : state.Nodes) {
					if (node.Outputs.size() >
						Limits::MaximumDynamicOutputsPerNode + Limits::MaximumGroupPorts)
						return false;
					largest = std::max(largest, uint64_t(node.Outputs.size() * sizeof(size_t)));
				}
				return true;
			};
			const auto session = [&](const GroupRenderSession &state) {
				if (!cache(state.Outputs) ||
					state.Replay.Random.Entries.size() > Limits::MaximumArrayElements)
					return false;
				largest = std::max(largest, uint64_t(state.Replay.Random.Entries.size() * sizeof(size_t)));
				const auto data = DataReplayValidationWorkspaceBytes(state.Replay.Data);
				if (data == UINT64_MAX) return false;
				largest = std::max(largest, data);
				return true;
			};
			if (!session(prior) || (request.GroupRender && !session(*request.GroupRender)) ||
				(request.SourceTunnelPreviousOutputs && !cache(*request.SourceTunnelPreviousOutputs)))
				return std::nullopt;
			for (const auto *data : {request.DataReplay, request.SourceFrameCacheLoads})
				if (data) {
					const auto bytes = DataReplayValidationWorkspaceBytes(*data);
					if (bytes == UINT64_MAX) return std::nullopt;
					largest = std::max(largest, bytes);
				}
			if (request.RandomReplay) {
				if (request.RandomReplay->Entries.size() > Limits::MaximumArrayElements) return std::nullopt;
				largest = std::max(largest, uint64_t(request.RandomReplay->Entries.size() * sizeof(size_t)));
			}
			const auto rigid = [&](const RigidReplayState &state) {
				if (state.Owners.size() > Limits::MaximumNodes) return false;
				uint64_t events = 0;
				for (const auto &owner : state.Owners) {
					if (owner.History.Frames.size() > Limits::MaximumArrayElements) return false;
					for (const auto &frame : owner.History.Frames)
						if (!AddBytes(events, frame.Events.size()) || events > 65536) return false;
				}
				largest = std::max(largest, events * sizeof(size_t));
				return true;
			};
			if (!rigid(prior.Replay.Rigid) || (request.RigidReplay && !rigid(*request.RigidReplay)) ||
				(request.GroupRender && !rigid(request.GroupRender->Replay.Rigid)))
				return std::nullopt;
			if (request.BuiltinRandomCaptures.size() > Limits::MaximumNodes) return std::nullopt;
			for (const auto &capture : request.BuiltinRandomCaptures) {
				if (capture.Inputs.size() > Limits::MaximumLinks ||
					capture.InputImages.size() > Limits::MaximumLinks)
					return std::nullopt;
				largest = std::max(
					largest,
					uint64_t(
						std::max(capture.Inputs.size(), capture.InputImages.size()) * sizeof(std::string_view)
					)
				);
			}
			return largest;
		}
		Status Admit(
			const Document &document,
			const Plan &plan,
			const EvaluationRequest &request,
			const GroupRenderSession &prior,
			bool matchingMembership,
			EvaluationBudget &budget,
			SourceCommonAdmission &admission,
			Diagnostic &diagnostic
		) {
			const auto authoredBytes = DocumentRetainedPayloadBytes(document), planBytes = PlanBytes(plan);
			auto authored = authoredBytes ? budget.Reserve(*authoredBytes) : std::nullopt;
			auto suppliedPlan = planBytes ? budget.Reserve(*planBytes) : std::nullopt;
			if (!authored || !suppliedPlan)
				return Fail(
					diagnostic, Status::LimitExceeded, "common authored document or plan exceeds live bytes"
				);
			admission.Document = std::move(*authored);
			admission.Plan = std::move(*suppliedPlan);
			const auto workspaceBytes = ValidationWorkspace(request, prior);
			auto workspace = workspaceBytes ? budget.Reserve(*workspaceBytes) : std::nullopt;
			if (!workspace)
				return Fail(
					diagnostic, Status::LimitExceeded, "common validation workspace exceeds live bytes"
				);
			if (ValidateEvaluationRequest(request, diagnostic) != Status::Ok ||
				Journals(prior, diagnostic) != Status::Ok)
				return diagnostic.Code;
			{
				AllocationReservation checkedCharge;
				Plan checked;
				if (CompileWithBudget(
						document, checked, diagnostic, budget, checkedCharge, plan.SourceCommonRuntimeOnly
					) != Status::Ok)
					return diagnostic.Code;
				if (checked != plan)
					return Fail(
						diagnostic, Status::InvalidOutput, "common runtime plan does not match document"
					);
			}
			if (matchingMembership && (Membership(document, prior, diagnostic) != Status::Ok ||
									   NativeMembership(document, prior, diagnostic) != Status::Ok))
				return diagnostic.Code;
			uint64_t borrowedBytes = 0, fontBytes = request.SourceFontHostResidentBytes;
			if (Borrowed(document, request, prior, borrowedBytes, diagnostic) != Status::Ok)
				return diagnostic.Code;
			if (request.FontProvider && !AddBytes(fontBytes, request.FontProvider->RetainedBytes()))
				return Fail(diagnostic, Status::LimitExceeded, "common font provider bytes overflow");
			auto borrowed = budget.Reserve(borrowedBytes), fonts = budget.Reserve(fontBytes),
				 observer = budget.Reserve(
					 request.SourceInputObserver ? request.SourceInputObserver->RetainedBytes() : 0
				 );
			if (!borrowed || !fonts || !observer)
				return Fail(diagnostic, Status::LimitExceeded, "common borrowed request exceeds live bytes");
			admission.Borrowed = std::move(*borrowed);
			admission.FontHost = std::move(*fonts);
			admission.Observer = std::move(*observer);
			return Status::Ok;
		}
	} // namespace common_admission
	Status ValidateSourceCommonObservation(
		const Document &document,
		const Plan &plan,
		const EvaluationRequest &request,
		const GroupRenderSession &session,
		EvaluationBudget &budget,
		SourceCommonAdmission &admission,
		Diagnostic &diagnostic
	) try {
		ENGINE_PROFILE("imagegraph.common_observation_admission");
		const auto status =
			common_admission::Admit(document, plan, request, session, true, budget, admission, diagnostic);
		if (status == Status::Ok) diagnostic = {};
		return status;
	} catch (const std::bad_alloc &) {
		return common_admission::Fail(
			diagnostic, Status::LimitExceeded, "common observation allocation failed"
		);
	}

	Status PrepareSourceCommonCandidate(
		const Document &document,
		const Plan &plan,
		const EvaluationRequest &request,
		std::optional<SourceNodeInitialState> initial,
		const GroupRenderSession &prior,
		GroupRenderSession &candidate,
		EvaluationBudget &budget,
		AllocationReservation &candidateCharge,
		SourceCommonAdmission &admission,
		Diagnostic &diagnostic,
		SourceCommonPreparePurpose purpose
	) try {
		ENGINE_PROFILE("imagegraph.common_candidate_admission");
		if (&prior == &candidate || candidateCharge.Bytes() ||
			(purpose != SourceCommonPreparePurpose::Step &&
			 purpose != SourceCommonPreparePurpose::Reconcile) ||
			(initial && *initial != SourceNodeInitialState::Loaded &&
			 *initial != SourceNodeInitialState::Constructed))
			return common_admission::Fail(
				diagnostic,
				Status::InvalidValue,
				"common candidate requires separate storage "
				"and explicit valid constructor intent"
			);
		const bool cold = purpose == SourceCommonPreparePurpose::Step && initial.has_value();
		if (common_admission::Admit(
				document,
				plan,
				request,
				prior,
				purpose == SourceCommonPreparePurpose::Step && !cold,
				budget,
				admission,
				diagnostic
			) != Status::Ok)
			return diagnostic.Code;
		const auto &simulation =
			request.SimulationReplay ? *request.SimulationReplay : prior.Replay.Simulation;
		const auto &surfaces = request.SurfaceReplay ? *request.SurfaceReplay : prior.Replay.Surfaces;
		const auto &random = request.RandomReplay ? *request.RandomReplay : prior.Replay.Random;
		const auto &data = request.DataReplay ? *request.DataReplay : prior.Replay.Data;
		const auto &rigid = request.RigidReplay ? *request.RigidReplay : prior.Replay.Rigid;
		uint64_t bytes = RetainedGroupRenderSessionBytes(prior);
		const auto replace = [&](uint64_t oldBytes, uint64_t newBytes) {
			if (oldBytes > bytes || newBytes == UINT64_MAX) return false;
			bytes -= oldBytes;
			return AddBytes(bytes, newBytes);
		};
		if (bytes == UINT64_MAX ||
			!replace(
				RetainedSimulationReplayBytes(prior.Replay.Simulation),
				RetainedSimulationReplayBytes(simulation)
			) ||
			!replace(
				RetainedSurfaceFrameReplayBytes(prior.Replay.Surfaces),
				RetainedSurfaceFrameReplayBytes(surfaces)
			) ||
			!replace(RetainedRandomReplayBytes(prior.Replay.Random), RetainedRandomReplayBytes(random)) ||
			!replace(RetainedDataReplayBytes(prior.Replay.Data), RetainedDataReplayBytes(data)) ||
			!replace(RetainedRigidReplayBytes(prior.Replay.Rigid), RetainedRigidReplayBytes(rigid)))
			return common_admission::Fail(
				diagnostic, Status::LimitExceeded, "common candidate journal bytes overflow"
			);
		auto copied = budget.Reserve(bytes);
		if (!copied)
			return common_admission::Fail(
				diagnostic, Status::LimitExceeded, "common candidate exceeds live bytes"
			);
		GroupRenderSession working;
		// Canonical temporal journals are imported exactly once, including explicit
		// cold initialization.
		working.Replay.Simulation = simulation;
		working.Replay.Surfaces = surfaces;
		working.Replay.Random = random;
		working.Replay.Data = data;
		working.Replay.Rigid = rigid;
		if (!cold) {
			working.Common = prior.Common;
			working.CommonAnimators = prior.CommonAnimators;
			working.SourceCommonWrites = prior.SourceCommonWrites;
			working.SourceCommonBindings = prior.SourceCommonBindings;
			working.SourceCommonInputs = prior.SourceCommonInputs;
			if (purpose == SourceCommonPreparePurpose::Step) {
				working.Outputs = prior.Outputs;
				working.Nodes = prior.Nodes;
				working.Purities = prior.Purities;
				working.Replay.Outputs = prior.Replay.Outputs;
			} else if (common_admission::NativeMetadata(
						   document,
						   prior,
						   false,
						   initial.value_or(SourceNodeInitialState::Loaded),
						   working,
						   budget,
						   *copied,
						   diagnostic
					   ) != Status::Ok)
				return diagnostic.Code;
		} else {
			if (common_admission::NativeMetadata(
					document, prior, true, *initial, working, budget, *copied, diagnostic
				) != Status::Ok)
				return diagnostic.Code;
			if (ReconcileSourceCommonMembership(document, *initial, working, budget, *copied, diagnostic) !=
				Status::Ok)
				return diagnostic.Code;
		}
		const uint64_t actual = RetainedGroupRenderSessionBytes(working);
		if (actual == UINT64_MAX || actual > copied->Bytes() || !copied->Resize(actual))
			return common_admission::Fail(
				diagnostic, Status::LimitExceeded, "common candidate capacity exceeds admission"
			);
		candidate = std::move(working);
		candidateCharge = std::move(*copied);
		diagnostic = {};
		return Status::Ok;
	} catch (const std::bad_alloc &) {
		return common_admission::Fail(
			diagnostic, Status::LimitExceeded, "common candidate allocation failed"
		);
	}
} // namespace detail
