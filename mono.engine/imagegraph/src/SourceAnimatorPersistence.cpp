#include "SourceAnimatorPersistence.hpp"

#include "GroupReplayInternal.hpp"
#include "ValuePayload.hpp"

#include <engine/core/Profiling.hpp>

#include <charconv>
#include <tuple>

namespace engine::imagegraph::detail {
	struct AnimatorReferenceWorkExceeded {};
	Status ValidateSourceAnimatorState(const Document &document, Diagnostic &diagnostic) try {
		if (!document.SourceAnimators) return Status::Ok;
		const auto fail = [&](Status status,
							  std::string_view message,
							  std::string_view node = {},
							  std::string_view port = {}) {
			diagnostic = {status, std::string(node), std::string(port), std::string(message)};
			return status;
		};
		const auto documentBytes = DocumentRetainedPayloadBytes(document);
		if (!documentBytes || *documentBytes > Limits::MaximumEvaluationBytes)
			return fail(Status::LimitExceeded, "source animator document exceeds public payload bounds");
		const auto &state = *document.SourceAnimators;
		const auto bytes = SourceAnimatorStateBytes(state, false);
		if (document.FormatVersion < 10)
			return fail(Status::UnsupportedVersion, "captured source animators require format 10");
		if (!bytes || *bytes > Limits::MaximumEvaluationBytes)
			return fail(Status::LimitExceeded, "captured source animators exceed payload bounds");
		uint64_t work = 0;
		bool refused = false;
		const auto admit = [&](uint64_t visits) {
			if (visits > 64'000'000 - work) throw AnimatorReferenceWorkExceeded{};
			work += visits;
		};
		const auto matches = [&](std::string_view a, std::string_view b) {
			const uint64_t visits = 1 + std::min(a.size(), b.size());
			admit(visits);
			return a == b;
		};
		const auto nodeById = [&](std::string_view id) -> const Node * {
			for (const auto &node : document.Nodes)
				if (matches(node.Id, id)) return &node;
			return nullptr;
		};
		const auto sourceInput =
			[&](const Node &node, std::string_view port, bool scalarAxes = false) -> const CatalogueInput * {
			const auto *entry = FindCatalogueEntry(node.Type);
			admit((port.size() + 1) * (1 + (entry ? entry->Inputs.size() : 0) + node.DynamicInputs.size()));
			return scalarAxes ? SourceSeparatedVec2Input(node, port) : AliasedSourceInput(node, port);
		};
		const auto detached = [&](std::string_view owner,
								  std::string_view port) -> const DetachedSourceAnimator * {
			for (const auto &record : state.Detached)
				if (matches(record.OwnerId, owner) && matches(record.Id, port)) return &record;
			return nullptr;
		};
		const auto payload = [&](std::string_view owner,
								 std::string_view port) -> const GroupSubtypeOverlay * {
			for (const auto &record : state.DetachedValues)
				if (matches(record.NodeId, owner) && matches(record.Port, port)) return &record;
			return nullptr;
		};
		const auto equivalentKey = [&](const Keyframe &actual, const Keyframe &expected) {
			const auto bytes = KeyframePayloadBytes(expected);
			if (!bytes) return false;
			admit(*bytes);
			return actual.SourceKeyId.empty() && std::tie(
													 actual.Tick,
													 actual.Data,
													 actual.Interpolation,
													 actual.Ease,
													 actual.SineDriver,
													 actual.SourceDriver,
													 actual.Subframe,
													 actual.NegativeFrame,
													 actual.Kind
												 ) ==
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
													 );
		};
		for (size_t index = 0; index < state.Detached.size(); ++index) {
			const auto &record = state.Detached[index];
			constexpr std::string_view prefix = "native:animator:";
			uint64_t generation = 0;
			if (!record.Id.starts_with(prefix))
				return fail(
					Status::InvalidGroup,
					"retained source animator has no native identity",
					record.OwnerId,
					record.Id
				);
			const auto digits = std::string_view(record.Id).substr(prefix.size());
			const auto number = std::from_chars(digits.data(), digits.data() + digits.size(), generation);
			if (number.ec != std::errc{} || number.ptr != digits.data() + digits.size() ||
				generation == UINT64_MAX || digits.empty())
				return fail(
					Status::InvalidGroup,
					"retained source animator identity is invalid",
					record.OwnerId,
					record.Id
				);
			for (size_t prior = 0; prior < index; ++prior)
				if (matches(state.Detached[prior].Id, record.Id))
					return fail(
						Status::DuplicateId,
						"retained source animator identity repeats",
						record.OwnerId,
						record.Id
					);
			const auto *owner = nodeById(record.OwnerId);
			const auto *value = payload(record.OwnerId, record.Id);
			if (!owner || !value || record.OriginalPort.empty())
				return fail(
					Status::InvalidGroup,
					"retained source animator owner or payload is absent",
					record.OwnerId,
					record.Id
				);
			if (record.Track && (record.Track->NodeId != record.OwnerId || record.Track->Port != record.Id ||
								 (record.Track->End != "hold" && record.Track->End != "loop" &&
								  record.Track->End != "ping" && record.Track->End != "wrap") ||
								 record.Track->LoopRange < -1 ||
								 (record.Track->QuaternionMode && *record.Track->QuaternionMode != 0 &&
								  *record.Track->QuaternionMode != 1)))
				return fail(
					Status::InvalidValue,
					"retained source animator track is invalid",
					record.OwnerId,
					record.Id
				);
			bool referenced = false;
			for (const auto &binding : state.Bindings) {
				if ((matches(binding.OwnerId, record.OwnerId) &&
					 matches(BindingAnimatorPort(binding), record.Id)) ||
					(matches(binding.Axes.OwnerId, record.OwnerId) && matches(binding.Axes.Port, record.Id)))
					referenced = true;
			}
			if (!referenced)
				return fail(
					Status::InvalidGroup,
					"retained source animator has no captured user",
					record.OwnerId,
					record.Id
				);
		}
		if (state.DetachedValues.size() != state.Detached.size())
			return fail(Status::InvalidGroup, "retained source animator payload table disagrees");
		for (size_t index = 0; index < state.DetachedValues.size(); ++index) {
			const auto &value = state.DetachedValues[index];
			if (!detached(value.NodeId, value.Port))
				return fail(
					Status::InvalidGroup,
					"retained source animator payload has no identity",
					value.NodeId,
					value.Port
				);
			for (size_t prior = 0; prior < index; ++prior)
				if (matches(state.DetachedValues[prior].NodeId, value.NodeId) &&
					matches(state.DetachedValues[prior].Port, value.Port))
					return fail(
						Status::DuplicateId,
						"retained source animator payload repeats",
						value.NodeId,
						value.Port
					);
		}
		for (size_t index = 0; index < state.Bindings.size(); ++index) {
			const auto &binding = state.Bindings[index];
			const auto *target = nodeById(binding.NodeId);
			const auto *owner = nodeById(binding.OwnerId);
			if (!target || !owner || target->InstanceBase.empty() || !sourceInput(*target, binding.Port) ||
				owner->Type != target->Type)
				return fail(
					Status::InvalidGroup,
					"captured source binding is not an instance input",
					binding.NodeId,
					binding.Port
				);
			for (size_t prior = 0; prior < index; ++prior)
				if (matches(state.Bindings[prior].NodeId, binding.NodeId) &&
					matches(state.Bindings[prior].Port, binding.Port))
					return fail(
						Status::DuplicateId,
						"captured source binding target repeats",
						binding.NodeId,
						binding.Port
					);
			const auto combined = BindingAnimatorPort(binding);
			if (!sourceInput(*owner, combined) && !detached(binding.OwnerId, combined))
				return fail(
					Status::InvalidGroup, "captured combined animator is absent", binding.NodeId, binding.Port
				);
			if (const auto *retired = detached(binding.OwnerId, combined)) {
				const auto *stored = payload(retired->OwnerId, retired->Id);
				if (!stored)
					return fail(
						Status::InvalidGroup,
						"captured retired combined payload is absent",
						binding.NodeId,
						binding.Port
					);
				if (stored->Fixed) {
					const Value *fixed = nullptr;
					for (const auto &input : target->DynamicInputs)
						if (matches(input.Id, binding.Port) && input.Default) fixed = &*input.Default;
					for (const auto &value : target->Values)
						if (matches(value.Port, binding.Port)) fixed = &value.Data;
					admit(RetainedPayloadBytes(*stored->Fixed));
					if (!fixed || *fixed != *stored->Fixed)
						return fail(
							Status::InvalidValue,
							"retired combined projection disagrees with canonical payload",
							binding.NodeId,
							binding.Port
						);
				}
				size_t keyIndex = 0;
				for (const auto &key : document.Keyframes)
					if (matches(key.NodeId, binding.NodeId) && matches(key.Port, binding.Port)) {
						if (keyIndex >= stored->Keys.size() || !equivalentKey(key, stored->Keys[keyIndex++]))
							return fail(
								Status::InvalidValue,
								"retired combined keys disagree with canonical payload",
								binding.NodeId,
								binding.Port
							);
					}
				if (keyIndex != stored->Keys.size())
					return fail(
						Status::InvalidValue,
						"retired combined keys are missing from projection",
						binding.NodeId,
						binding.Port
					);
			}
			const Node *root = target;
			for (size_t hop = 0; root && !root->InstanceBase.empty() && hop < document.Nodes.size(); ++hop)
				root = nodeById(root->InstanceBase);
			if (!root || !root->InstanceBase.empty() || root != owner)
				return fail(
					Status::InvalidGroup,
					"captured combined owner disagrees with instance ancestry",
					binding.NodeId,
					binding.Port
				);
			const auto &axes = binding.Axes;
			if (axes.Storage == GroupAxisStorage::None) {
				if (!axes.OwnerId.empty() || !axes.Port.empty() || !axes.InstanceBase.empty() ||
					sourceInput(*target, binding.Port, true))
					return fail(
						Status::InvalidGroup,
						"captured scalar identity is absent or carries names",
						binding.NodeId,
						binding.Port
					);
			} else {
				if (!sourceInput(*target, binding.Port, true) || axes.InstanceBase != target->InstanceBase)
					return fail(
						Status::InvalidGroup,
						"captured scalar constructor ancestry disagrees",
						binding.NodeId,
						binding.Port
					);
				const auto *physical = nodeById(axes.OwnerId);
				if (!physical || physical->Type != target->Type)
					return fail(
						Status::InvalidGroup, "captured scalar owner is absent", binding.NodeId, binding.Port
					);
				const auto *retained = detached(axes.OwnerId, axes.Port);
				const auto *stored = retained ? payload(axes.OwnerId, axes.Port) : nullptr;
				const auto *array = stored && stored->SeparatedVec2 ? &*stored->SeparatedVec2
																	: FindSeparatedVec2(*physical, axes.Port);
				if (axes.Storage == GroupAxisStorage::Uninitialized) {
					if (axes.OwnerId != binding.NodeId || axes.Port != binding.Port || retained)
						return fail(
							Status::InvalidGroup,
							"captured cold scalar constructor has warm storage",
							binding.NodeId,
							binding.Port
						);
				} else if (!array || !array->Initialized || (retained && !(stored && stored->SeparatedVec2)))
					return fail(
						Status::InvalidGroup,
						"captured warm scalar storage is absent",
						binding.NodeId,
						binding.Port
					);
				if (retained) {
					const auto *projected = FindSeparatedVec2(*target, binding.Port);
					if (!projected || projected->Initialized != array->Initialized)
						return fail(
							Status::InvalidValue,
							"retired scalar projection disagrees with canonical payload",
							binding.NodeId,
							binding.Port
						);
					for (size_t axis = 0; axis < 2; ++axis) {
						if (projected->Axes[axis].Keys.size() != array->Axes[axis].Keys.size())
							return fail(
								Status::InvalidValue,
								"retired scalar projection keys are missing",
								binding.NodeId,
								binding.Port
							);
						for (size_t key = 0; key < array->Axes[axis].Keys.size(); ++key)
							if (!equivalentKey(projected->Axes[axis].Keys[key], array->Axes[axis].Keys[key]))
								return fail(
									Status::InvalidValue,
									"retired scalar keys disagree with canonical payload",
									binding.NodeId,
									binding.Port
								);
					}
				}
				if (axes.Storage == GroupAxisStorage::Local && axes.OwnerId != binding.NodeId)
					return fail(
						Status::InvalidGroup,
						"captured local scalar owner disagrees",
						binding.NodeId,
						binding.Port
					);
				const Node *ancestor = target;
				for (size_t hop = 0; ancestor && ancestor != physical && !ancestor->InstanceBase.empty() &&
									 hop < document.Nodes.size();
					 ++hop)
					ancestor = nodeById(ancestor->InstanceBase);
				if (ancestor != physical)
					return fail(
						Status::InvalidGroup,
						"captured scalar owner is outside instance ancestry",
						binding.NodeId,
						binding.Port
					);
			}
			if (refused)
				return fail(
					Status::LimitExceeded, "captured source animator reference checks exceed work bounds"
				);
		}
		if (refused)
			return fail(
				Status::LimitExceeded, "captured source animator reference checks exceed work bounds"
			);
		diagnostic = {};
		return Status::Ok;
	} catch (const AnimatorReferenceWorkExceeded &) {
		diagnostic = {
			Status::LimitExceeded, {}, {}, "captured source animator reference checks exceed work bounds"
		};
		return diagnostic.Code;
	}
}

namespace engine::imagegraph {
	Status RestoreSourceAnimatorBindings(
		const Document &document,
		const GroupReplayState &declarations,
		uint64_t revision,
		GroupReplayState &result,
		Diagnostic &diagnostic,
		uint64_t maximumBytes
	) try {
		ENGINE_PROFILE("imagegraph.source_animator_restore");
		if (!document.SourceAnimators) {
			diagnostic = {Status::InvalidValue, {}, {}, "document has no captured source animators"};
			return diagnostic.Code;
		}
		if (detail::ValidateSourceAnimatorState(document, diagnostic) != Status::Ok) return diagnostic.Code;
		const auto documentBytes = DocumentRetainedPayloadBytes(document);
		const auto stateBytes = detail::SourceAnimatorStateBytes(*document.SourceAnimators, true);
		const uint64_t destination = &declarations == &result ? 0 : result.RetainedBytes();
		if (!maximumBytes || maximumBytes > Limits::MaximumEvaluationBytes || !documentBytes || !stateBytes ||
			*documentBytes > maximumBytes || *stateBytes > maximumBytes - *documentBytes ||
			declarations.RetainedBytes() > maximumBytes - *documentBytes - *stateBytes ||
			destination > maximumBytes - *documentBytes - *stateBytes - declarations.RetainedBytes()) {
			diagnostic = {
				Status::LimitExceeded, {}, {}, "source animator restore exceeds live operation budget"
			};
			return diagnostic.Code;
		}
		const uint64_t allowance = maximumBytes - *documentBytes - *stateBytes;
		auto owner = detail::CloneGroupReplay(declarations, 0, revision, allowance, destination, diagnostic);
		if (!owner) return diagnostic.Code;
		auto charge = owner->Budget.Reserve(*stateBytes);
		if (!charge) {
			diagnostic = {Status::LimitExceeded, {}, {}, "source animator snapshot clone exceeds bounds"};
			return diagnostic.Code;
		}
		owner->Bindings = document.SourceAnimators->Bindings;
		owner->DetachedAnimators = document.SourceAnimators->Detached;
		owner->SharedSubtypes = document.SourceAnimators->DetachedValues;
		owner->Bound = true;
		for (const auto &record : owner->DetachedAnimators) {
			constexpr std::string_view prefix = "native:animator:";
			uint64_t generation = 0;
			const auto digits = std::string_view(record.Id).substr(prefix.size());
			std::from_chars(digits.data(), digits.data() + digits.size(), generation);
			owner->NextAnimatorId = std::max(owner->NextAnimatorId, generation + 1);
		}
		if (!owner->Charge.Merge(std::move(*charge))) std::terminate();
		GroupReplayState seed;
		detail::GroupReplayAccess::Install(seed, std::move(owner));
		return BindGroupReplay(
			document,
			seed.Bindings(),
			seed,
			revision,
			result,
			diagnostic,
			maximumBytes - declarations.RetainedBytes()
		);
	} catch (const std::bad_alloc &) {
		diagnostic = {Status::LimitExceeded, {}, {}, "source animator restore allocation failed"};
		return diagnostic.Code;
	}
}
