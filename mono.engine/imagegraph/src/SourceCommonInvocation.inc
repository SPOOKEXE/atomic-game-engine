namespace detail {
	SourceCommonDispatch
	SourceCommonOwnerDispatch(const Document &document, const EvaluationRequest &, size_t ownerIndex) {
		if (ownerIndex >= document.SourceCommonOwners.size()) return {};
		const auto &owner = document.SourceCommonOwners[ownerIndex];
		if (owner.NativeOwnerKind == SourceCommonNativeOwnerKind::Group) {
			if (owner.SourceType != "Node_Group" && owner.SourceType != "Node_Collection") return {};
			if (std::none_of(document.Groups.begin(), document.Groups.end(), [&](const auto &group) {
					return group.Id == owner.NativeOwnerId;
				}))
				return {};
			return SourceCommonDispatchProfile(owner.SourceType, true);
		}
		if (owner.NativeOwnerKind != SourceCommonNativeOwnerKind::Node) return {};
		const auto node = std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const auto &n) {
			return n.Id == owner.NativeOwnerId;
		});
		if (node == document.Nodes.end()) return {};
		const auto *entry = FindCatalogueEntry(node->Type);
		const bool executable = entry && entry->SourceNode == owner.SourceType && FindExecutor(node->Type);
		auto profile = SourceCommonDispatchProfile(owner.SourceType, true);
		if (profile.Wrapper == SourceCommonWrapperKind::Empty) {
			if (!SourceCommonEmptyOwnerMatches(*node, owner.SourceType)) return {};
			return profile;
		}
		if (!executable) profile.Wrapper = SourceCommonWrapperKind::Unsupported;
		return profile;
	}
	Status RecordSourceCommonAnimatorReset(
		GroupRenderSession &candidate,
		const SourceCommonOwnerRecord &owner,
		const SourceCommonAnimatorResetReceipt &receipt,
		EvaluationBudget &budget,
		AllocationReservation &candidateCharge,
		Diagnostic &diagnostic
	) {
		auto found = std::find_if(
			candidate.SourceCommonWrites.begin(), candidate.SourceCommonWrites.end(), [&](const auto &row) {
				return row.OwnerId == owner.UpdateAnimatorOwnerId && row.Port == owner.UpdateAnimatorPort;
			}
		);
		if (found == candidate.SourceCommonWrites.end()) {
			if (candidate.SourceCommonWrites.size() >= Limits::MaximumSourceCommonOwners) {
				diagnostic = {
					Status::LimitExceeded, owner.SourceOwnerId, {}, "common writer count exceeds bounds"
				};
				return diagnostic.Code;
			}
			const size_t oldCapacity = candidate.SourceCommonWrites.capacity();
			const size_t capacity = std::max(oldCapacity, candidate.SourceCommonWrites.size() + 1);
			const uint64_t bytes = (capacity > oldCapacity ? capacity * sizeof(SourceCommonWriterState) : 0) +
								   std::max(owner.UpdateAnimatorOwnerId.size(), std::string{}.capacity()) +
								   std::max(owner.UpdateAnimatorPort.size(), std::string{}.capacity()) + 2;
			auto storage = budget.Reserve(bytes);
			if (!storage) {
				diagnostic = {
					Status::LimitExceeded, owner.SourceOwnerId, {}, "common writer effects exceed live bytes"
				};
				return diagnostic.Code;
			}
			if (capacity > oldCapacity) candidate.SourceCommonWrites.reserve(capacity);
			candidate.SourceCommonWrites.push_back({owner.UpdateAnimatorOwnerId, owner.UpdateAnimatorPort});
			const auto &written = candidate.SourceCommonWrites.back();
			const uint64_t actual = (capacity > oldCapacity ? candidate.SourceCommonWrites.capacity() *
																  sizeof(SourceCommonWriterState)
															: 0) +
									written.OwnerId.capacity() + written.Port.capacity() + 2;
			if (!storage->Resize(actual)) {
				SetDiagnostic(
					diagnostic,
					Status::LimitExceeded,
					"common writer retained storage exceeds live bytes",
					owner.SourceOwnerId
				);
				return diagnostic.Code;
			}
			if (!candidateCharge.Merge(std::move(*storage))) std::terminate();
			if (capacity > oldCapacity) {
				auto replaced = candidateCharge.Split(oldCapacity * sizeof(SourceCommonWriterState));
				if (!replaced) std::terminate();
			}
			found = std::prev(candidate.SourceCommonWrites.end());
		}
		found->Modified |= receipt.Modified;
		found->ForceDynamic |= receipt.ForceDynamic;
		found->Edited |= receipt.Edited;
		return Status::Ok;
	}
	Status InvokeSourceCommonCallback(
		const Document &document,
		const Plan &plan,
		const EvaluationRequest &request,
		size_t ownerIndex,
		SourceCommonInvocationMode invocation,
		GroupRenderSession &candidate,
		EvaluationBudget &budget,
		AllocationReservation &candidateCharge,
		Diagnostic &diagnostic,
		SourceCommonAdmission *admission
	) {
		if (ownerIndex >= document.SourceCommonOwners.size()) {
			diagnostic = {Status::InvalidValue, {}, {}, "common callback owner index exceeds registry"};
			return diagnostic.Code;
		}
		const auto &owner = document.SourceCommonOwners[ownerIndex];
		const auto dispatch = SourceCommonOwnerDispatch(document, request, ownerIndex);
		if (dispatch.Wrapper == SourceCommonWrapperKind::Unsupported) {
			diagnostic = {
				Status::UnsupportedExecution,
				owner.SourceOwnerId,
				{},
				"common callback has no audited native wrapper"
			};
			return diagnostic.Code;
		}
		if (dispatch.Wrapper == SourceCommonWrapperKind::Empty) {
			if (invocation != SourceCommonInvocationMode::DirectUpdate &&
				invocation != SourceCommonInvocationMode::SourceDoUpdate) {
				diagnostic = {
					Status::InvalidValue,
					owner.SourceOwnerId,
					{},
					"empty source callback invocation is invalid"
				};
				return diagnostic.Code;
			}
			if (candidate.Nodes.size() != document.Nodes.size() ||
				candidate.Outputs.Nodes.size() != document.Nodes.size()) {
				diagnostic = {
					Status::InvalidValue,
					owner.SourceOwnerId,
					{},
					"empty source callback requires initialized owner journals"
				};
				return diagnostic.Code;
			}
			diagnostic = {};
			return Status::Ok;
		}
		if (invocation == SourceCommonInvocationMode::SourceDoUpdate) {
			if (!request.SourceSafeMode) {
				diagnostic = {
					Status::UnsupportedExecution,
					owner.SourceOwnerId,
					{},
					"source wrapper needs an observed safe mode"
				};
				return diagnostic.Code;
			}
			if (*request.SourceSafeMode) return Status::Ok;
		}
		EvaluationVector<uint8_t> run(document.Nodes.size(), 0, EvaluationAllocator<uint8_t>{budget});
		std::optional<size_t> directTarget;
		if (owner.NativeOwnerKind == SourceCommonNativeOwnerKind::Node) {
			const auto node = std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const auto &n) {
				return n.Id == owner.NativeOwnerId;
			});
			if (node == document.Nodes.end() || !FindExecutor(node->Type)) {
				diagnostic = {
					Status::UnsupportedExecution,
					owner.SourceOwnerId,
					{},
					"common direct callback lacks a native executor"
				};
				return diagnostic.Code;
			}
			const size_t index = size_t(node - document.Nodes.begin());
			run[index] = 1;
			if (invocation == SourceCommonInvocationMode::DirectUpdate) directTarget = index;
		} else {
			const auto purity =
				std::find_if(candidate.Purities.begin(), candidate.Purities.end(), [&](const auto &row) {
					return row.GroupId == owner.NativeOwnerId;
				});
			if (purity == candidate.Purities.end() || purity->State == SourceGroupPurity::Unknown) {
				diagnostic = {
					Status::UnsupportedExecution,
					owner.SourceOwnerId,
					{},
					"Collection callback needs observed cached purity"
				};
				return diagnostic.Code;
			}
			if (purity->State == SourceGroupPurity::Nonpure) return Status::Ok;
			for (size_t index = 0; index < document.Nodes.size(); ++index)
				if (document.Nodes[index].GroupId == owner.NativeOwnerId) {
					const auto *child = CommonOwner(document, document.Nodes[index].Id);
					if (child && SourceCommonEmptyOwnerMatches(document.Nodes[index], child->SourceType))
						continue;
					run[index] = 1;
				}
		}
		if (owner.NativeOwnerKind == SourceCommonNativeOwnerKind::Group) {
			if (!request.SourceSafeMode) {
				SetDiagnostic(
					diagnostic,
					Status::UnsupportedExecution,
					"Collection children need observed safe mode",
					owner.SourceOwnerId
				);
				return diagnostic.Code;
			}
			if (*request.SourceSafeMode) std::fill(run.begin(), run.end(), uint8_t{0});
		}
		if (candidate.Nodes.size() != document.Nodes.size() ||
			candidate.Outputs.Nodes.size() != document.Nodes.size()) {
			diagnostic = {
				Status::InvalidValue,
				owner.SourceOwnerId,
				{},
				"common callback journals require explicit initialization"
			};
			return diagnostic.Code;
		}
		EvaluationRequest processing = request;
		processing.GroupRender = &candidate;
		processing.ForceGroupRender = true;
		processing.SourceCommon = &candidate.Common;
		processing.SourceCommonAnimators = &candidate.CommonAnimators;
		processing.SourceTunnelPreviousOutputs = &candidate.Outputs;
		processing.SimulationReplay = &candidate.Replay.Simulation;
		processing.SurfaceReplay = &candidate.Replay.Surfaces;
		processing.RandomReplay = &candidate.Replay.Random;
		processing.DataReplay = &candidate.Replay.Data;
		processing.RigidReplay = &candidate.Replay.Rigid;
		GroupProcessCapture capture{run, &candidate.Outputs, &candidate.Nodes};
		capture.Charge = &candidateCharge;
		capture.Invocation = invocation;
		capture.DirectTarget = directTarget;
		capture.Common = &candidate.Common;
		capture.Session = &candidate;
		capture.CommonAdmission = admission;
		SimulationCapture replay{
			&candidate.Replay.Simulation,
			&candidateCharge,
			&candidate.Replay.Surfaces,
			&candidate.Replay.Random,
			&candidate.Replay.Data,
			&candidate.Replay.Rigid
		};
		NodeResult ignored;
		AllocationReservation outputCharge;
		return EvaluateGraph(
			document,
			plan,
			{},
			processing,
			ignored,
			diagnostic,
			budget,
			outputCharge,
			nullptr,
			nullptr,
			nullptr,
			nullptr,
			{},
			{},
			true,
			nullptr,
			&replay,
			nullptr,
			&capture
		);
	}
}
namespace detail {
	Status RefreshSourceCommonCollection(
		const Document &document,
		const Plan &,
		const EvaluationRequest &request,
		size_t ownerIndex,
		const SourcePurityRefresh &refresh,
		GroupRenderSession &candidate,
		EvaluationBudget &,
		AllocationReservation &,
		Diagnostic &diagnostic
	) {
		if (ownerIndex >= document.SourceCommonOwners.size()) {
			SetDiagnostic(diagnostic, Status::InvalidValue, "Collection refresh owner is absent");
			return diagnostic.Code;
		}
		const auto &owner = document.SourceCommonOwners[ownerIndex];
		const auto dispatch = SourceCommonOwnerDispatch(document, request, ownerIndex);
		if (dispatch.Step != SourceCommonStepKind::CollectionOverride) {
			SetDiagnostic(
				diagnostic,
				Status::UnsupportedExecution,
				"owner has no source Collection refresh",
				owner.SourceOwnerId
			);
			return diagnostic.Code;
		}
		if (refresh.Event != SourcePurityRefreshEvent::LoadTopology &&
			refresh.Event != SourcePurityRefreshEvent::Membership &&
			refresh.Event != SourcePurityRefreshEvent::AnimationMode &&
			refresh.Event != SourcePurityRefreshEvent::PureFunction &&
			refresh.Event != SourcePurityRefreshEvent::InputOutput) {
			SetDiagnostic(
				diagnostic, Status::InvalidValue, "Collection refresh event is invalid", owner.SourceOwnerId
			);
			return diagnostic.Code;
		}
		std::string_view groupId = owner.NativeOwnerId;
		if (owner.NativeOwnerKind == SourceCommonNativeOwnerKind::Node) {
			const auto group =
				std::find_if(document.Groups.begin(), document.Groups.end(), [&](const auto &g) {
					return g.OwnerNodeId == owner.NativeOwnerId;
				});
			if (group == document.Groups.end()) {
				SetDiagnostic(
					diagnostic,
					Status::UnsupportedExecution,
					"Collection owner has no retained child group",
					owner.SourceOwnerId
				);
				return diagnostic.Code;
			}
			groupId = group->Id;
		}
		if (refresh.Groups.size() > Limits::MaximumGroups ||
			(!refresh.Groups.empty() && (refresh.Groups.size() != 1 || refresh.Groups.front() != groupId))) {
			SetDiagnostic(
				diagnostic,
				Status::InvalidGroup,
				"Collection refresh must select its own group",
				owner.SourceOwnerId
			);
			return diagnostic.Code;
		}
		const auto row =
			std::find_if(candidate.Purities.begin(), candidate.Purities.end(), [&](const auto &p) {
				return p.GroupId == groupId;
			});
		if (row == candidate.Purities.end()) {
			SetDiagnostic(
				diagnostic,
				Status::InvalidValue,
				"Collection purity needs explicit initialization",
				owner.SourceOwnerId
			);
			return diagnostic.Code;
		}
		const uint64_t work = document.Nodes.size() *
							  uint64_t(
								  document.Nodes.size() + document.Keyframes.size() + document.Tracks.size() +
								  (request.GroupReplay ? request.GroupReplay->Bindings().size() : 0)
							  );
		if (work > 16'000'000) {
			SetDiagnostic(
				diagnostic,
				Status::LimitExceeded,
				"Collection refresh exceeds comparison bounds",
				owner.SourceOwnerId
			);
			return diagnostic.Code;
		}
		const auto pure = PureSourceGroup(document, groupId, request.GroupReplay, candidate.Nodes);
		row->State = !pure	 ? SourceGroupPurity::Unknown
					 : *pure ? SourceGroupPurity::Pure
							 : SourceGroupPurity::Nonpure;
		diagnostic = {};
		return Status::Ok;
	}
	Status ObserveSourceCommonCandidateOutputs(
		const Document &document,
		const Plan &plan,
		const EvaluationRequest &request,
		GroupRenderSession &candidate,
		EvaluationBudget &budget,
		AllocationReservation &candidateCharge,
		Diagnostic &diagnostic
	) {
		StatefulOutputEvaluationResult observed;
		auto namedCharge = budget.Reserve(document.Outputs.size() * sizeof(StatefulNamedOutput));
		if (!namedCharge) {
			SetDiagnostic(diagnostic, Status::LimitExceeded, "held named outputs exceed live bytes");
			return diagnostic.Code;
		}
		observed.Outputs.reserve(document.Outputs.size());
		for (const auto &output : document.Outputs) {
			StatefulNamedOutput named;
			AllocationReservation valueCharge;
			if (const auto selector = CommonSelector(output.Port);
				selector && CommonOwner(document, output.NodeId)) {
				Value value;
				if (ReadSourceCommonGetter(
						document,
						plan,
						CommonOwner(document, output.NodeId)->SourceOwnerId,
						*selector,
						request,
						budget,
						value,
						valueCharge,
						diagnostic
					) != Status::Ok)
					return diagnostic.Code;
				auto portCharge = budget.Reserve(std::max(output.Port.size(), std::string{}.capacity()));
				if (!portCharge || !valueCharge.Merge(std::move(*portCharge))) {
					SetDiagnostic(diagnostic, Status::LimitExceeded, "held common port exceeds live bytes");
					return diagnostic.Code;
				}
				named.Output = EvaluatedValue{output.Port, std::move(value), CommonSelectorDomain(*selector)};
			} else {
				const auto held = std::find_if(
					candidate.Outputs.Nodes.begin(), candidate.Outputs.Nodes.end(), [&](const auto &n) {
						return n.NodeId == output.NodeId;
					}
				);
				if (held == candidate.Outputs.Nodes.end()) {
					SetDiagnostic(
						diagnostic,
						Status::InvalidOutput,
						"held output owner is absent",
						output.NodeId,
						output.Port
					);
					return diagnostic.Code;
				}
				const auto port =
					std::find_if(held->Outputs.begin(), held->Outputs.end(), [&](const auto &p) {
						return p.Port == output.Port;
					});
				if (port == held->Outputs.end()) {
					SetDiagnostic(
						diagnostic,
						Status::InvalidOutput,
						"held output port is absent",
						output.NodeId,
						output.Port
					);
					return diagnostic.Code;
				}
				if (port->Refusal || !port->Data) continue;
				const auto node =
					std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const auto &n) {
						return n.Id == output.NodeId;
					});
				if (node == document.Nodes.end()) {
					SetDiagnostic(
						diagnostic, Status::InvalidOutput, "held output node is absent", output.NodeId
					);
					return diagnostic.Code;
				}
				CatalogueOutputs restored;
				AllocationReservation restoreCharge;
				if (!RestoreFrozenCacheGroupOutputs(
						*node,
						*held,
						request.MaximumImageDimension,
						restored,
						budget,
						restoreCharge,
						diagnostic
					))
					return diagnostic.Code;
				if (const auto found = std::find_if(
						restored.Images.begin(),
						restored.Images.end(),
						[&](const auto &v) { return v.first == output.Port; }
					);
					found != restored.Images.end())
					named.Output = std::move(found->second);
				else if (const auto found = std::find_if(
							 restored.ImageArrays.begin(),
							 restored.ImageArrays.end(),
							 [&](const auto &v) { return v.first == output.Port; }
						 );
						 found != restored.ImageArrays.end())
					named.Output = std::move(found->second);
				else {
					const auto value =
						std::find_if(restored.Values.begin(), restored.Values.end(), [&](const auto &v) {
							return v.Port == output.Port;
						});
					if (value == restored.Values.end()) {
						SetDiagnostic(
							diagnostic,
							Status::InvalidOutput,
							"held value cannot be restored",
							output.NodeId,
							output.Port
						);
						return diagnostic.Code;
					}
					named.Output =
						EvaluatedValue{std::move(value->Port), std::move(value->Data), port->Domain};
				}
				// The restored snapshot still owns the other ports until this iteration ends.
				auto retained = budget.Reserve(RetainedStatefulValueBytes(named.Output));
				if (!retained) {
					SetDiagnostic(
						diagnostic,
						Status::LimitExceeded,
						"held named payload exceeds live bytes",
						output.NodeId,
						output.Port
					);
					return diagnostic.Code;
				}
				valueCharge = std::move(*retained);
			}
			auto idCharge = budget.Reserve(std::max(output.Id.size(), std::string{}.capacity()));
			if (!idCharge) {
				SetDiagnostic(diagnostic, Status::LimitExceeded, "held named identity exceeds live bytes");
				return diagnostic.Code;
			}
			named.Id = output.Id;
			observed.Outputs.push_back(std::move(named));
			if (!namedCharge->Merge(std::move(valueCharge)) || !namedCharge->Merge(std::move(*idCharge)))
				std::terminate();
		}
		if (!namedCharge->Resize(RetainedStatefulOutputBytes(observed))) {
			SetDiagnostic(diagnostic, Status::LimitExceeded, "held named output copy exceeds live bytes");
			return diagnostic.Code;
		}
		const uint64_t previous = RetainedStatefulOutputBytes(candidate.Replay);
		candidate.Replay.Outputs = std::move(observed.Outputs);
		auto released = candidateCharge.Split(previous);
		if (!released || !candidateCharge.Merge(std::move(*namedCharge))) std::terminate();
		diagnostic = {};
		return Status::Ok;
	}
}
