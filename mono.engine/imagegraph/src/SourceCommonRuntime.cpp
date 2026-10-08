#include "SourceCommonAnimatorResetInternal.hpp"
#include "SourceCommonExecution.hpp"
#include "SourceCommonMembership.hpp"
#include "ValuePayload.hpp"

#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/imagegraph/FrameCacheReplay.hpp>
#include <engine/imagegraph/SourceCommonRuntime.hpp>
#include <engine/imagegraph/SourceCommonSockets.hpp>

#include <algorithm>
#include <cmath>
#include <new>

namespace engine::imagegraph {
	namespace {
		Status SourceCommonRuntimeFail(
			Diagnostic &diagnostic, Status status, const char *message, std::string_view owner = {}
		) {
			diagnostic = {status, std::string(owner), {}, message};
			return status;
		}
		bool SourceCommonRuntimeAdd(uint64_t &bytes, uint64_t value) {
			if (value > UINT64_MAX - bytes) return false;
			bytes += value;
			return true;
		}
		EvaluationRequest
		CandidateRequest(const EvaluationRequest &request, const GroupRenderSession &candidate) {
			auto current = request;
			current.GroupRender = &candidate;
			current.SourceCommon = &candidate.Common;
			current.SourceCommonAnimators = &candidate.CommonAnimators;
			current.SimulationReplay = &candidate.Replay.Simulation;
			current.SurfaceReplay = &candidate.Replay.Surfaces;
			current.RandomReplay = &candidate.Replay.Random;
			current.DataReplay = &candidate.Replay.Data;
			current.RigidReplay = &candidate.Replay.Rigid;
			return current;
		}
		Status StepCacheOwner(
			const Document &document,
			const Plan &plan,
			const EvaluationRequest &request,
			size_t ownerIndex,
			GroupRenderSession &candidate,
			detail::EvaluationBudget &budget,
			detail::AllocationReservation &candidateCharge,
			detail::SourceCommonAdmission &admission,
			Diagnostic &diagnostic
		) {
			if (!request.SourceCachePlayback) return Status::Ok;
			if (request.SourceCachePlayback->Loading == SourceCacheLoadMode::CompleteReceipt)
				return Status::Ok;
			if (request.SourceCachePlayback->Loading != SourceCacheLoadMode::SourceStepLoading)
				return SourceCommonRuntimeFail(
					diagnostic, Status::InvalidValue, "source cache loading profile is invalid"
				);
			if (!request.SourceCachePlayback->SynchronousProducer)
				return SourceCommonRuntimeFail(
					diagnostic,
					Status::UnsupportedExecution,
					"source cache loading requires synchronous producer observations"
				);
			const auto &owner = document.SourceCommonOwners[ownerIndex];
			const auto node = std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const auto &n) {
				return n.Id == owner.NativeOwnerId;
			});
			if (node == document.Nodes.end())
				return SourceCommonRuntimeFail(
					diagnostic, Status::InvalidValue, "source cache step owner is absent", owner.SourceOwnerId
				);
			auto &rows = candidate.Replay.Data.Entries;
			const auto identity = SourceFrameCacheIdentity(*node);
			bool cleared = false;
			for (const auto &row : rows)
				if (row.NodeId == node->Id && row.LoadedCacheData == identity)
					cleared |= row.FrameCacheConstructorCleared;
			bool completed = false;
			bool found = false;
			const uint64_t total = document.Timeline ? document.Timeline->Frames : 1;
			const auto advance = [&](const DataReplayEntry &source, DataReplayEntry &output) {
				auto charge = budget.Reserve(budget.Available());
				if (!charge || !charge->Bytes())
					return SourceCommonRuntimeFail(
						diagnostic,
						Status::LimitExceeded,
						"source cache loading has no live byte allowance",
						node->Id
					);
				bool done = false;
				const auto status = StepSourceFrameCacheLoading(
					*node, total, source, output, done, diagnostic, charge->Bytes()
				);
				if (status != Status::Ok) return status;
				if (!charge->Resize(RetainedDataReplayEntryBytes(output)))
					return SourceCommonRuntimeFail(
						diagnostic,
						Status::LimitExceeded,
						"source cache loading output exceeds live bytes",
						node->Id
					);
				completed |= done;
				if (!candidateCharge.Merge(std::move(*charge))) std::terminate();
				return Status::Ok;
			};
			for (auto &row : rows) {
				if (row.NodeId != node->Id || row.LoadedCacheData != identity) continue;
				found = true;
				if (!row.SourceFrameCacheLoading || !row.SourceFrameCacheLoading->Loading) continue;
				const uint64_t oldBytes = RetainedDataReplayEntryBytes(row);
				DataReplayEntry updated;
				if (advance(row, updated) != Status::Ok) return diagnostic.Code;
				row = std::move(updated);
				if (!candidateCharge.Split(oldBytes)) std::terminate();
			}
			if (!found && !cleared && !SourceFrameCacheSavedText(*node).empty()) {
				auto loadingCharge = budget.Reserve(budget.Available());
				if (!loadingCharge || !loadingCharge->Bytes())
					return SourceCommonRuntimeFail(
						diagnostic,
						Status::LimitExceeded,
						"source cache constructor has no live byte allowance",
						node->Id
					);
				const DataReplayEntry *decoded = nullptr;
				if (request.SourceFrameCacheLoads)
					for (const auto &row : request.SourceFrameCacheLoads->Entries)
						if (row.NodeId == node->Id && row.ProcessorRow == 0 &&
							row.LoadedCacheData == identity)
							decoded = &row;
				DataReplayEntry loading;
				DataReplayEntry updated;
				if (decoded && decoded->SourceFrameCacheLoading) {
					loadingCharge.reset();
					if (advance(*decoded, updated) != Status::Ok) return diagnostic.Code;
				} else {
					const auto status = decoded
											? BeginSourceFrameCacheLoading(
												  *node, *decoded, loading, diagnostic, loadingCharge->Bytes()
											  )
											: BeginNativeSourceFrameCacheLoading(
												  *node, loading, diagnostic, loadingCharge->Bytes()
											  );
					if (status != Status::Ok) return status;
					if (!loadingCharge->Resize(RetainedDataReplayEntryBytes(loading)))
						return SourceCommonRuntimeFail(
							diagnostic,
							Status::LimitExceeded,
							"source cache constructor exceeds live bytes",
							node->Id
						);
					if (advance(loading, updated) != Status::Ok) return diagnostic.Code;
				}
				if (rows.size() >= Limits::MaximumArrayElements)
					return SourceCommonRuntimeFail(
						diagnostic, Status::LimitExceeded, "source cache row count exceeds bounds", node->Id
					);
				const auto oldCapacity = rows.capacity();
				auto tableCharge = budget.Reserve(
					rows.size() == oldCapacity ? (rows.size() + 1) * sizeof(DataReplayEntry) : 0
				);
				if (!tableCharge)
					return SourceCommonRuntimeFail(
						diagnostic,
						Status::LimitExceeded,
						"source cache row table exceeds live bytes",
						node->Id
					);
				if (rows.size() == oldCapacity) rows.reserve(rows.size() + 1);
				rows.push_back(std::move(updated));
				if (!tableCharge->Resize(
						rows.capacity() == oldCapacity ? 0 : rows.capacity() * sizeof(DataReplayEntry)
					))
					return SourceCommonRuntimeFail(
						diagnostic,
						Status::LimitExceeded,
						"source cache retained table exceeds live bytes",
						node->Id
					);
				if (!candidateCharge.Merge(std::move(*tableCharge))) std::terminate();
				if (rows.capacity() != oldCapacity &&
					!candidateCharge.Split(oldCapacity * sizeof(DataReplayEntry)))
					std::terminate();
			}
			if (!completed) return Status::Ok;
			const auto current = CandidateRequest(request, candidate);
			return detail::InvokeSourceCommonCallback(
				document,
				plan,
				current,
				ownerIndex,
				detail::SourceCommonInvocationMode::DirectUpdate,
				candidate,
				budget,
				candidateCharge,
				diagnostic,
				&admission
			);
		}
		Status Observations(
			const Document &document,
			const SourceCommonRuntimeObservations &observations,
			Diagnostic &diagnostic,
			uint64_t &bytes
		) {
			if (document.SourceCommonOwners.size() > Limits::MaximumSourceCommonOwners ||
				observations.Metadata.size() > Limits::MaximumSourceCommonOwners ||
				observations.Collections.size() > Limits::MaximumSourceCommonOwners)
				return SourceCommonRuntimeFail(
					diagnostic, Status::LimitExceeded, "source step observation count exceeds bounds"
				);
			const uint64_t count = observations.Metadata.size() + observations.Collections.size();
			if (count * uint64_t(document.SourceCommonOwners.size() + count) > 16'000'000)
				return SourceCommonRuntimeFail(
					diagnostic,
					Status::LimitExceeded,
					"source common observations exceed comparison work bounds"
				);

			bytes = observations.Metadata.size() * sizeof(SourceCommonRuntimeMetadataObservation) +
					observations.Collections.size() * sizeof(SourceCommonRuntimeCollectionStepObservation);
			const auto owner = [&](std::string_view id, std::string_view type) {
				return std::any_of(
					document.SourceCommonOwners.begin(),
					document.SourceCommonOwners.end(),
					[&](const auto &row) { return row.SourceOwnerId == id && row.SourceType == type; }
				);
			};
			const auto identity = [&](std::string_view id, std::string_view type) {
				return !id.empty() && id.size() <= Limits::MaximumTextBytes && !type.empty() &&
					   type.size() <= Limits::MaximumTextBytes && owner(id, type) &&
					   SourceCommonRuntimeAdd(bytes, id.size()) && SourceCommonRuntimeAdd(bytes, type.size());
			};
			for (size_t index = 0; index < observations.Metadata.size(); ++index) {
				const auto &row = observations.Metadata[index];
				if (!identity(row.OwnerId, row.SourceType) ||
					row.RuntimeName.size() > Limits::MaximumTextBytes ||
					!SourceCommonRuntimeAdd(bytes, row.RuntimeName.size()) ||
					std::any_of(
						observations.Metadata.begin(),
						observations.Metadata.begin() + index,
						[&](const auto &prior) { return prior.OwnerId == row.OwnerId; }
					))
					return SourceCommonRuntimeFail(
						diagnostic,
						Status::InvalidValue,
						"source runtime name observation is invalid",
						row.OwnerId
					);
			}
			for (size_t index = 0; index < observations.Collections.size(); ++index) {
				const auto &row = observations.Collections[index];
				if (!identity(row.OwnerId, row.SourceType) ||
					row.RefreshNodesPending != row.Refresh.has_value() ||
					(row.Refresh && row.Refresh->Groups.size() > Limits::MaximumGroups) ||
					std::any_of(
						observations.Collections.begin(),
						observations.Collections.begin() + index,
						[&](const auto &prior) { return prior.OwnerId == row.OwnerId; }
					))
					return SourceCommonRuntimeFail(
						diagnostic,
						Status::InvalidValue,
						"source Collection step observation is invalid",
						row.OwnerId
					);
				if (row.Refresh) {
					if (!SourceCommonRuntimeAdd(bytes, row.Refresh->Groups.size() * sizeof(std::string_view)))
						return SourceCommonRuntimeFail(
							diagnostic, Status::LimitExceeded, "source refresh observation size overflows"
						);
					for (const auto group : row.Refresh->Groups)
						if (group.size() > Limits::MaximumTextBytes ||
							!SourceCommonRuntimeAdd(bytes, group.size()))
							return SourceCommonRuntimeFail(
								diagnostic, Status::LimitExceeded, "source refresh identity exceeds bounds"
							);
				}
			}
			return Status::Ok;
		}
		Status Metadata(
			const Document &document,
			const SourceCommonOwnerRecord &owner,
			const SourceCommonRuntimeObservations &observations,
			SourceCommonSocketState &socket,
			detail::EvaluationBudget &budget,
			detail::AllocationReservation &candidateCharge,
			Diagnostic &diagnostic
		) {
			std::string_view name;
			Vector2 position;
			if (owner.NativeOwnerKind == SourceCommonNativeOwnerKind::Node) {
				const auto node =
					std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const auto &row) {
						return row.Id == owner.NativeOwnerId;
					});
				if (node == document.Nodes.end())
					return SourceCommonRuntimeFail(
						diagnostic, Status::UnknownNode, "source metadata node is absent", owner.SourceOwnerId
					);
				position = node->Position;
				if (owner.DisplayNamePresent) name = node->SourceDisplayName;
			} else {
				const auto group =
					std::find_if(document.Groups.begin(), document.Groups.end(), [&](const auto &row) {
						return row.Id == owner.NativeOwnerId;
					});
				if (group == document.Groups.end())
					return SourceCommonRuntimeFail(
						diagnostic,
						Status::InvalidGroup,
						"source metadata group is absent",
						owner.SourceOwnerId
					);
				position = group->SourcePosition;
				if (owner.DisplayNamePresent) name = group->Name;
			}
			if (!owner.DisplayNamePresent) {
				const auto observation = std::find_if(
					observations.Metadata.begin(), observations.Metadata.end(), [&](const auto &row) {
						return row.OwnerId == owner.SourceOwnerId;
					}
				);
				if (observation == observations.Metadata.end())
					return SourceCommonRuntimeFail(
						diagnostic,
						Status::UnsupportedExecution,
						"source metadata needs an observed runtime name",
						owner.SourceOwnerId
					);
				name = observation->RuntimeName;
			}
			if (name.size() > Limits::MaximumTextBytes || !std::isfinite(position.X) ||
				!std::isfinite(position.Y))
				return SourceCommonRuntimeFail(
					diagnostic,
					Status::InvalidValue,
					"source metadata payload is invalid",
					owner.SourceOwnerId
				);
			auto replacementCharge = budget.Reserve(std::max(name.size(), std::string{}.capacity()));
			if (!replacementCharge)
				return SourceCommonRuntimeFail(
					diagnostic,
					Status::LimitExceeded,
					"source metadata replacement exceeds live bytes",
					owner.SourceOwnerId
				);
			std::string replacement(name);
			if (!replacementCharge->Resize(replacement.capacity()))
				return SourceCommonRuntimeFail(
					diagnostic,
					Status::LimitExceeded,
					"source metadata capacity exceeds live bytes",
					owner.SourceOwnerId
				);
			const auto oldBytes = socket.Name.capacity(), newBytes = replacement.capacity();
			uint64_t nextBytes = candidateCharge.Bytes();
			if (oldBytes > nextBytes || !SourceCommonRuntimeAdd(nextBytes -= oldBytes, newBytes) ||
				(newBytes > oldBytes && !candidateCharge.Resize(nextBytes)))
				return SourceCommonRuntimeFail(
					diagnostic,
					Status::LimitExceeded,
					"source metadata candidate exceeds live bytes",
					owner.SourceOwnerId
				);
			socket.Name = std::move(replacement);
			if (newBytes <= oldBytes) (void)candidateCharge.Resize(nextBytes);
			socket.Position = position;
			return Status::Ok;
		}
		Status Reset(
			const SourceCommonOwnerRecord &owner,
			const EvaluationRequest &request,
			GroupRenderSession &candidate,
			detail::EvaluationBudget &budget,
			detail::AllocationReservation &candidateCharge,
			Diagnostic &diagnostic
		) {
			const auto writer = std::find_if(
				candidate.CommonAnimators.Detached.begin(),
				candidate.CommonAnimators.Detached.end(),
				[&](const auto &row) {
					return row.OwnerId == owner.UpdateAnimatorOwnerId && row.Id == owner.UpdateAnimatorPort;
				}
			);
			const auto payload = std::find_if(
				candidate.CommonAnimators.DetachedValues.begin(),
				candidate.CommonAnimators.DetachedValues.end(),
				[&](const auto &row) {
					return row.NodeId == owner.UpdateAnimatorOwnerId && row.Port == owner.UpdateAnimatorPort;
				}
			);
			if (writer == candidate.CommonAnimators.Detached.end() ||
				payload == candidate.CommonAnimators.DetachedValues.end())
				return SourceCommonRuntimeFail(
					diagnostic,
					Status::InvalidValue,
					"source Update local animator is absent",
					owner.SourceOwnerId
				);
			SourceCommonAnimatorResetReceipt receipt;
			const SourceCommonAnimatorResetOptions options{
				{request.Tick, request.Subframe, request.NegativeFrame}
			};
			const auto status = detail::ResetSourceCommonAnimatorBudgeted(
				*writer, *payload, options, receipt, diagnostic, budget, candidateCharge
			);
			if (status != Status::Ok) return status;
			return detail::RecordSourceCommonAnimatorReset(
				candidate, owner, receipt, budget, candidateCharge, diagnostic
			);
		}
	}

	Status InitializeNativeSourceCommonRuntime(
		const Document &document,
		const Plan &plan,
		const EvaluationRequest &request,
		SourceNodeInitialState initialState,
		GroupRenderSession &session,
		Diagnostic &diagnostic,
		uint64_t maximumBytes
	) try {
		ENGINE_PROFILE("imagegraph.source_common.initialize");
		if (!maximumBytes || maximumBytes > Limits::MaximumEvaluationBytes)
			return SourceCommonRuntimeFail(
				diagnostic, Status::LimitExceeded, "source common initialization cap is invalid"
			);
		detail::EvaluationBudget budget(maximumBytes);
		auto previousCharge = budget.Reserve(RetainedGroupRenderSessionBytes(session));
		if (!previousCharge)
			return SourceCommonRuntimeFail(
				diagnostic, Status::LimitExceeded, "source common prior state exceeds live bytes"
			);
		detail::SourceCommonAdmission admission;
		detail::AllocationReservation candidateCharge;
		GroupRenderSession candidate;
		const auto status = detail::PrepareSourceCommonCandidate(
			document,
			plan,
			request,
			initialState,
			session,
			candidate,
			budget,
			candidateCharge,
			admission,
			diagnostic
		);
		if (status != Status::Ok) return status;
		session = std::move(candidate);
		diagnostic = {};
		return Status::Ok;
	} catch (const std::bad_alloc &) {
		return SourceCommonRuntimeFail(
			diagnostic, Status::LimitExceeded, "source common initialization allocation failed"
		);
	}

	Status ReconcileNativeSourceCommonRuntime(
		const Document &document,
		const Plan &plan,
		const EvaluationRequest &request,
		const SourceCommonRuntimeReconcile &operation,
		GroupRenderSession &session,
		Diagnostic &diagnostic,
		uint64_t maximumBytes
	) try {
		ENGINE_PROFILE("imagegraph.source_common.reconcile");
		if (!maximumBytes || maximumBytes > Limits::MaximumEvaluationBytes)
			return SourceCommonRuntimeFail(
				diagnostic, Status::LimitExceeded, "source common reconciliation cap is invalid"
			);
		detail::EvaluationBudget budget(maximumBytes);
		auto previousCharge = budget.Reserve(RetainedGroupRenderSessionBytes(session));
		if (!previousCharge)
			return SourceCommonRuntimeFail(
				diagnostic, Status::LimitExceeded, "source common prior state exceeds live bytes"
			);
		detail::SourceCommonAdmission admission;
		detail::AllocationReservation candidateCharge;
		GroupRenderSession candidate;
		const auto status = detail::PrepareSourceCommonCandidate(
			document,
			plan,
			request,
			operation.NewOwnerState,
			session,
			candidate,
			budget,
			candidateCharge,
			admission,
			diagnostic,
			detail::SourceCommonPreparePurpose::Reconcile
		);
		if (status != Status::Ok) return status;
		if (detail::ReconcileSourceCommonMembership(
				document,
				operation.NewOwnerState,
				candidate,
				budget,
				candidateCharge,
				diagnostic,
				operation.ResetWriters
			) != Status::Ok)
			return diagnostic.Code;

		const auto observedRequest = CandidateRequest(request, candidate);
		if (detail::ObserveSourceCommonCandidateOutputs(
				document, plan, observedRequest, candidate, budget, candidateCharge, diagnostic
			) != Status::Ok)
			return diagnostic.Code;
		if (!candidateCharge.Resize(RetainedGroupRenderSessionBytes(candidate)))
			return SourceCommonRuntimeFail(
				diagnostic,
				Status::LimitExceeded,
				"source common reconciliation publication exceeds live bytes"
			);
		core::Metrics::SetGauge(
			"imagegraph.source_common.retained_bytes", double(RetainedGroupRenderSessionBytes(candidate))
		);

		session = std::move(candidate);
		diagnostic = {};
		return Status::Ok;
	} catch (const std::bad_alloc &) {
		return SourceCommonRuntimeFail(
			diagnostic, Status::LimitExceeded, "source common reconciliation allocation failed"
		);
	}

	Status NativeSourceStepBounded(
		const Document &document,
		const Plan &plan,
		const EvaluationRequest &request,
		const SourceCommonRuntimeObservations &observations,
		GroupRenderSession &session,
		Diagnostic &diagnostic,
		uint64_t maximumBytes
	) try {
		ENGINE_PROFILE("imagegraph.source_common.step");
		if (request.Scope != ComposerScope::Unrestricted && request.Scope != ComposerScope::ImageOnly)
			return SourceCommonRuntimeFail(diagnostic, Status::InvalidValue, "Composer scope is invalid");
		if (!maximumBytes || maximumBytes > Limits::MaximumEvaluationBytes)
			return SourceCommonRuntimeFail(
				diagnostic, Status::LimitExceeded, "source common step cap is invalid"
			);
		uint64_t observationBytes = 0;
		if (Observations(document, observations, diagnostic, observationBytes) != Status::Ok)
			return diagnostic.Code;
		detail::EvaluationBudget budget(maximumBytes);
		auto previousCharge = budget.Reserve(RetainedGroupRenderSessionBytes(session));
		auto observedCharge = budget.Reserve(observationBytes);
		if (!previousCharge || !observedCharge)
			return SourceCommonRuntimeFail(
				diagnostic,
				Status::LimitExceeded,
				"source common prior state or observations exceed live bytes"
			);
		detail::SourceCommonAdmission admission;
		detail::AllocationReservation candidateCharge;
		GroupRenderSession candidate;
		if (detail::PrepareSourceCommonCandidate(
				document,
				plan,
				request,
				std::nullopt,
				session,
				candidate,
				budget,
				candidateCharge,
				admission,
				diagnostic
			) != Status::Ok)
			return diagnostic.Code;
		for (size_t index = 0; index < document.SourceCommonOwners.size(); ++index) {
			const auto &owner = document.SourceCommonOwners[index];
			if (!owner.Active) continue;
			if (request.Scope == ComposerScope::ImageOnly &&
				owner.NativeOwnerKind == SourceCommonNativeOwnerKind::Node) {
				const auto node =
					std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const auto &entry) {
						return entry.Id == owner.NativeOwnerId;
					});
				if (node != document.Nodes.end() && !ComposerNodeEnabled(*node, request.Scope)) continue;
			}
			const auto dispatch = detail::SourceCommonOwnerDispatch(document, request, index);
			auto current = CandidateRequest(request, candidate);
			if (dispatch.Step == SourceCommonStepKind::CacheOverride) {
				if (StepCacheOwner(
						document,
						plan,
						current,
						index,
						candidate,
						budget,
						candidateCharge,
						admission,
						diagnostic
					) != Status::Ok)
					return diagnostic.Code;
				continue;
			}
			if (dispatch.Step == SourceCommonStepKind::CollectionOverride) {
				const auto observed = std::find_if(
					observations.Collections.begin(), observations.Collections.end(), [&](const auto &row) {
						return row.OwnerId == owner.SourceOwnerId;
					}
				);
				if (observed == observations.Collections.end())
					return SourceCommonRuntimeFail(
						diagnostic,
						Status::UnsupportedExecution,
						"source Collection pending flags were not observed",
						owner.SourceOwnerId
					);
				if (observed->RefreshNodeDisplayPending && !observed->NodeDisplayRefreshHandled)
					return SourceCommonRuntimeFail(
						diagnostic,
						Status::UnsupportedExecution,
						"source Collection display refresh requires its host",
						owner.SourceOwnerId
					);
				if (observed->RefreshNodesPending && detail::RefreshSourceCommonCollection(
														 document,
														 plan,
														 current,
														 index,
														 *observed->Refresh,
														 candidate,
														 budget,
														 candidateCharge,
														 diagnostic
													 ) != Status::Ok)
					return diagnostic.Code;
				continue;
			}
			if (dispatch.Step != SourceCommonStepKind::NodeDataCommon) {
				if (owner.ShowUpdateTrigger || owner.OutMeta)
					return SourceCommonRuntimeFail(
						diagnostic,
						Status::UnsupportedExecution,
						"source common step dispatch is not verified",
						owner.SourceOwnerId
					);
				continue;
			}
			if (owner.ShowUpdateTrigger) {
				detail::AllocationReservation getterCharge;
				Value value;
				if (detail::ReadSourceCommonGetter(
						document,
						plan,
						owner.SourceOwnerId,
						SourceCommonSelector::Update,
						current,
						budget,
						value,
						getterCharge,
						diagnostic
					) != Status::Ok)
					return diagnostic.Code;
				const auto requested = std::get_if<bool>(&value);
				if (!requested)
					return SourceCommonRuntimeFail(
						diagnostic,
						Status::TypeMismatch,
						"source Update getter is not a Trigger",
						owner.SourceOwnerId
					);
				if (*requested) {
					if (detail::InvokeSourceCommonCallback(
							document,
							plan,
							current,
							index,
							detail::SourceCommonInvocationMode::DirectUpdate,
							candidate,
							budget,
							candidateCharge,
							diagnostic,
							&admission
						) != Status::Ok)
						return diagnostic.Code;
					if (Reset(owner, current, candidate, budget, candidateCharge, diagnostic) != Status::Ok)
						return diagnostic.Code;
				}
				candidate.Common.Owners[index].Updated = false;
			}
			if (owner.OutMeta && Metadata(
									 document,
									 owner,
									 observations,
									 candidate.Common.Owners[index],
									 budget,
									 candidateCharge,
									 diagnostic
								 ) != Status::Ok)
				return diagnostic.Code;
		}
		const auto finalRequest = CandidateRequest(request, candidate);
		if (detail::ObserveSourceCommonCandidateOutputs(
				document, plan, finalRequest, candidate, budget, candidateCharge, diagnostic
			) != Status::Ok)
			return diagnostic.Code;

		if (!candidateCharge.Resize(RetainedGroupRenderSessionBytes(candidate)))
			return SourceCommonRuntimeFail(
				diagnostic, Status::LimitExceeded, "source common publication exceeds live bytes"
			);
		core::Metrics::Count("imagegraph.source_common.step_owners", document.SourceCommonOwners.size());
		core::Metrics::SetGauge(
			"imagegraph.source_common.retained_bytes", double(RetainedGroupRenderSessionBytes(candidate))
		);
		session = std::move(candidate);
		diagnostic = {};
		return Status::Ok;
	} catch (const std::bad_alloc &) {
		return SourceCommonRuntimeFail(
			diagnostic, Status::LimitExceeded, "source common step allocation failed"
		);
	}

	Status ReadNativeSourceCommonGetter(
		const Document &document,
		const Plan &plan,
		std::string_view ownerId,
		SourceCommonSelector selector,
		const EvaluationRequest &request,
		const GroupRenderSession &session,
		EvaluatedValue &result,
		Diagnostic &diagnostic,
		uint64_t maximumBytes
	) try {
		ENGINE_PROFILE("imagegraph.source_common.observe");
		if (!maximumBytes || maximumBytes > Limits::MaximumEvaluationBytes ||
			ownerId.size() > Limits::MaximumTextBytes)
			return SourceCommonRuntimeFail(
				diagnostic, Status::LimitExceeded, "source common observation bounds are invalid"
			);
		std::string_view port;
		switch (selector) {
		case SourceCommonSelector::Update:
			port = "pxcx.update_in_trigger";
			break;
		case SourceCommonSelector::Updated:
			port = "pxcx.updated_out_trigger";
			break;
		case SourceCommonSelector::Name:
			port = "pxcx.metadata.0";
			break;
		case SourceCommonSelector::Position:
			port = "pxcx.metadata.1";
			break;
		default:
			return SourceCommonRuntimeFail(
				diagnostic, Status::UnknownPort, "source common selector is invalid", ownerId
			);
		}
		detail::EvaluationBudget budget(maximumBytes);
		auto sessionCharge = budget.Reserve(RetainedGroupRenderSessionBytes(session));
		uint64_t oldBytes = sizeof(result);
		if (!SourceCommonRuntimeAdd(oldBytes, result.Port.capacity()) ||
			!SourceCommonRuntimeAdd(oldBytes, detail::RetainedPayloadBytes(result.Data)))
			return SourceCommonRuntimeFail(
				diagnostic, Status::LimitExceeded, "source common prior observation overflows"
			);
		auto priorResultCharge = budget.Reserve(oldBytes);
		if (!sessionCharge || !priorResultCharge)
			return SourceCommonRuntimeFail(
				diagnostic, Status::LimitExceeded, "source common observation prior values exceed live bytes"
			);
		detail::SourceCommonAdmission admission;
		auto current = CandidateRequest(request, session);
		// Read-only hosts keep canonical replay journals outside the held-output session.
		if (request.SimulationReplay) current.SimulationReplay = request.SimulationReplay;
		if (request.SurfaceReplay) current.SurfaceReplay = request.SurfaceReplay;
		if (request.RandomReplay) current.RandomReplay = request.RandomReplay;
		if (request.DataReplay) current.DataReplay = request.DataReplay;
		if (request.RigidReplay) current.RigidReplay = request.RigidReplay;
		if (detail::ValidateSourceCommonObservation(
				document, plan, current, session, budget, admission, diagnostic
			) != Status::Ok)
			return diagnostic.Code;
		detail::AllocationReservation valueCharge;
		Value value;
		if (detail::ReadSourceCommonGetter(
				document, plan, ownerId, selector, current, budget, value, valueCharge, diagnostic
			) != Status::Ok)
			return diagnostic.Code;
		auto portCharge =
			budget.Reserve(sizeof(EvaluatedValue) + std::max(port.size(), std::string{}.capacity()));
		if (!portCharge)
			return SourceCommonRuntimeFail(
				diagnostic, Status::LimitExceeded, "source common observation port exceeds live bytes"
			);
		EvaluatedValue candidate{
			std::string(port), std::move(value), detail::SourceCommonGetterDomain(selector)
		};
		if (!portCharge->Resize(sizeof(candidate) + candidate.Port.capacity()))
			return SourceCommonRuntimeFail(
				diagnostic, Status::LimitExceeded, "source common observation capacity exceeds live bytes"
			);
		result = std::move(candidate);
		diagnostic = {};
		return Status::Ok;
	} catch (const std::bad_alloc &) {
		return SourceCommonRuntimeFail(
			diagnostic, Status::LimitExceeded, "source common observation allocation failed"
		);
	}
}
