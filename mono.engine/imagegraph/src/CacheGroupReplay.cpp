#include "EvaluationAllocator.hpp"
#include "MeshPayload.hpp"
#include "ValuePayload.hpp"
#include "ValueText.hpp"

#include <engine/core/Profiling.hpp>
#include <engine/imagegraph/CacheGroupReplay.hpp>
#include <engine/imagegraph/Catalogue.hpp>
#include <engine/imagegraph/SourceFrameCacheProject.hpp>

#include <algorithm>
#include <cmath>
#include <new>
#include <numeric>

namespace engine::imagegraph {
	namespace {
		using detail::MeshAddBytes;
		constexpr size_t MAXIMUM_OUTPUTS_PER_NODE =
			Limits::MaximumDynamicOutputsPerNode + Limits::MaximumGroupPorts;
		constexpr uint64_t COMPARISON_WORK_LIMIT = 64ull * 1024 * 1024;
		uint64_t WorkProduct(uint64_t count, uint64_t bytes) {
			return count && bytes > UINT64_MAX / count ? UINT64_MAX : count * bytes;
		}
		uint64_t OutputWork(std::span<const CacheGroupReplayOutput> outputs) {
			if (outputs.size() > MAXIMUM_OUTPUTS_PER_NODE) return UINT64_MAX;
			uint64_t longest = 1;
			for (const auto &output : outputs)
				longest = std::max(longest, uint64_t(output.Port.size() + 1));
			size_t levels = 0;
			for (auto count = outputs.size(); count > 1; count = (count + 1) / 2)
				++levels;
			return WorkProduct(outputs.size() * (4 * levels + 1), longest);
		}
		uint64_t ValidationWorkspace(const CacheGroupReplayState &state) {
			size_t largest = 0;
			for (const auto &node : state.Nodes)
				largest = std::max(largest, node.Outputs.size());
			return largest * sizeof(size_t);
		}
		uint64_t ComparisonWork(const CacheGroupReplayState &state) {
			if (state.Nodes.size() > Limits::MaximumNodes || state.Owners.size() > Limits::MaximumNodes)
				return UINT64_MAX;
			uint64_t longest = 1, memberships = 0, outputWork = 0;
			for (const auto &node : state.Nodes) {
				longest =
					std::max({longest, uint64_t(node.NodeId.size() + 1), uint64_t(node.OwnerId.size() + 1)});
				outputWork = MeshAddBytes(outputWork, OutputWork(node.Outputs));
			}
			for (const auto &owner : state.Owners) {
				if (owner.Members.size() > Limits::MaximumLinks - memberships) return UINT64_MAX;
				memberships += owner.Members.size();
				longest = std::max(longest, uint64_t(owner.NodeId.size() + 1));
				for (const auto &member : owner.Members)
					longest = std::max(longest, uint64_t(member.size() + 1));
			}
			const uint64_t nodes = state.Nodes.size(), owners = state.Owners.size();
			return MeshAddBytes(
				outputWork,
				WorkProduct(
					nodes * nodes + owners * owners + 2 * nodes * owners + memberships * nodes, longest
				)
			);
		}
		bool ValidText(std::string_view text, bool allowEmpty = false) {
			return (allowEmpty || !text.empty()) && text.size() <= Limits::MaximumTextBytes;
		}
		uint64_t StringBytes(const std::string &text, bool retained) {
			return retained ? text.capacity() : std::max(text.size(), std::string{}.capacity());
		}
		template <class Records> auto Find(Records &records, std::string_view nodeId) {
			return std::find_if(records.begin(), records.end(), [&](const auto &record) {
				return record.NodeId == nodeId;
			});
		}
		CacheGroupReplayChange Refuse(Status code, std::string_view message, std::string_view nodeId = {}) {
			return {code, false, {code, std::string(nodeId), {}, std::string(message)}};
		}
		// Root surfaces use the native output cap; nested typed arrays retain their recursive payload cap.
		std::optional<uint64_t> ReplayValueCloneBytes(const Value &value) {
			if (const auto *surface = std::get_if<SurfaceValue>(&value)) {
				if (!ValidSurfaceLayout(
						surface->Data, Limits::MaximumDimension, Limits::MaximumOutputBytes
					) ||
					!FiniteSurfaceSamples(surface->Data))
					return std::nullopt;
				return sizeof(Value) + surface->Data.Pixels.size();
			}
			return ValueClonePayloadBytes(value);
		}
		uint64_t OutputBytes(const CacheGroupReplayOutput &output, bool retained) {
			uint64_t bytes = StringBytes(output.Port, retained);
			if (output.Data) {
				const auto payload = retained
										 ? std::optional<uint64_t>{detail::RetainedPayloadBytes(*output.Data)}
										 : ReplayValueCloneBytes(*output.Data);
				bytes = MeshAddBytes(bytes, payload.value_or(UINT64_MAX));
			}
			if (output.Refusal)
				for (const auto *text :
					 {&output.Refusal->NodeId, &output.Refusal->Port, &output.Refusal->Message})
					bytes = MeshAddBytes(bytes, StringBytes(*text, retained));
			return bytes;
		}
		uint64_t StateBytes(const CacheGroupReplayState &state, bool retained) {
			uint64_t bytes = sizeof(state);
			bytes = MeshAddBytes(
				bytes, (retained ? state.Nodes.capacity() : state.Nodes.size()) * sizeof(CacheGroupReplayNode)
			);
			bytes = MeshAddBytes(
				bytes,
				(retained ? state.Owners.capacity() : state.Owners.size()) * sizeof(CacheGroupReplayOwner)
			);
			for (const auto &node : state.Nodes) {
				for (const auto *text : {&node.NodeId, &node.NodeType, &node.OwnerId})
					bytes = MeshAddBytes(bytes, StringBytes(*text, retained));
				bytes = MeshAddBytes(
					bytes,
					(retained ? node.Outputs.capacity() : node.Outputs.size()) *
						sizeof(CacheGroupReplayOutput)
				);
				for (const auto &output : node.Outputs)
					bytes = MeshAddBytes(bytes, OutputBytes(output, retained));
			}
			for (const auto &owner : state.Owners) {
				bytes = MeshAddBytes(bytes, StringBytes(owner.NodeId, retained));
				bytes = MeshAddBytes(
					bytes, (retained ? owner.Members.capacity() : owner.Members.size()) * sizeof(std::string)
				);
				for (const auto &member : owner.Members)
					bytes = MeshAddBytes(bytes, StringBytes(member, retained));
			}
			return bytes;
		}
		CacheGroupReplayChange
		ValidateOutputs(std::span<const CacheGroupReplayOutput> outputs, std::string_view nodeId) {
			if (outputs.size() > MAXIMUM_OUTPUTS_PER_NODE)
				return Refuse(Status::LimitExceeded, "cache-group producer exceeds output bounds", nodeId);
			for (size_t index = 0; index < outputs.size(); ++index) {
				const auto &output = outputs[index];
				if (!ValidText(output.Port))
					return Refuse(Status::InvalidValue, "cache-group output port is invalid", nodeId);
				if (output.Data && !ReplayValueCloneBytes(*output.Data))
					return Refuse(Status::InvalidValue, "cache-group output payload is invalid", nodeId);
				if (output.Domain &&
					(output.Domain->Type > ValueType::Font ||
					 (output.Domain->Display && *output.Domain->Display > SourceValueDisplay::Curve) ||
					 (output.Domain->Kind && *output.Domain->Kind > SourceSocketKind::Font)))
					return Refuse(Status::InvalidValue, "cache-group output domain is invalid", nodeId);
				if (output.Refusal &&
					(output.Refusal->Code == Status::Ok ||
					 output.Refusal->Code > Status::UnsupportedExecution ||
					 !ValidText(output.Refusal->NodeId, true) || !ValidText(output.Refusal->Port, true) ||
					 !ValidText(output.Refusal->Message, true)))
					return Refuse(Status::InvalidValue, "cache-group output refusal is invalid", nodeId);
			}
			std::vector<size_t> order(outputs.size());
			std::iota(order.begin(), order.end(), size_t{0});
			std::sort(order.begin(), order.end(), [&](size_t first, size_t second) {
				return outputs[first].Port < outputs[second].Port;
			});
			for (size_t index = 1; index < order.size(); ++index)
				if (outputs[order[index - 1]].Port == outputs[order[index]].Port)
					return Refuse(Status::DuplicateId, "cache-group output repeats a port", nodeId);
			return {};
		}
		bool EnableAllowed(const CacheGroupReplayOwner &owner, const CacheGroupReplayOperation &operation) {
			return owner.Serialize && !operation.Project.ProjectLoading &&
				   !operation.Project.ProjectAppending;
		}
		void SetActivity(CacheGroupReplayState &state, const CacheGroupReplayOwner &owner, bool active) {
			for (const auto &member : owner.Members) {
				auto found = Find(state.Nodes, member);
				if (found != state.Nodes.end()) found->RenderActive = active;
			}
		}
		CacheGroupReplayChange Publish(
			CacheGroupReplayState &state,
			CacheGroupReplayState candidate,
			uint64_t maximumBytes,
			bool clear = false
		) {
			Diagnostic diagnostic;
			const auto code = ValidateCacheGroupReplay(candidate, maximumBytes, diagnostic);
			if (code != Status::Ok) return {code, false, std::move(diagnostic)};
			if (MeshAddBytes(RetainedCacheGroupReplayBytes(state), RetainedCacheGroupReplayBytes(candidate)) >
				maximumBytes)
				return Refuse(Status::LimitExceeded, "cache-group candidate overlap exceeds byte budget");
			state = std::move(candidate);
			return {Status::Ok, clear, {}};
		}
	}
	uint64_t RetainedCacheGroupReplayBytes(const CacheGroupReplayState &state) {
		return StateBytes(state, true);
	}
	Status ValidateCacheGroupReplay(
		const CacheGroupReplayState &state, uint64_t maximumBytes, Diagnostic &diagnostic
	) try {
		diagnostic = {};
		const auto refuse = [&](CacheGroupReplayChange result) {
			diagnostic = std::move(result.Error);
			return result.Code;
		};
		maximumBytes = std::min(maximumBytes, Limits::MaximumEvaluationBytes);
		if (ComparisonWork(state) > COMPARISON_WORK_LIMIT)
			return refuse(
				Refuse(Status::LimitExceeded, "cache-group validation exceeds comparison work bounds")
			);
		if (state.Nodes.size() > Limits::MaximumNodes || state.Owners.size() > Limits::MaximumNodes ||
			MeshAddBytes(RetainedCacheGroupReplayBytes(state), ValidationWorkspace(state)) > maximumBytes)
			return refuse(Refuse(Status::LimitExceeded, "cache-group journal exceeds node or byte budget"));
		size_t memberships = 0;
		for (size_t index = 0; index < state.Nodes.size(); ++index) {
			const auto &node = state.Nodes[index];
			if (!ValidText(node.NodeId) || !ValidText(node.NodeType) || !ValidText(node.OwnerId, true))
				return refuse(Refuse(Status::InvalidValue, "cache-group producer identity is invalid"));
			for (size_t previous = 0; previous < index; ++previous)
				if (state.Nodes[previous].NodeId == node.NodeId)
					return refuse(
						Refuse(Status::DuplicateId, "cache-group producer identity repeats", node.NodeId)
					);
			if (!node.OwnerId.empty() && Find(state.Owners, node.OwnerId) == state.Owners.end())
				return refuse(
					Refuse(Status::InvalidValue, "cache-group member owner is absent", node.NodeId)
				);
			auto outputs = ValidateOutputs(node.Outputs, node.NodeId);
			if (outputs.Code != Status::Ok) return refuse(std::move(outputs));
		}
		for (size_t index = 0; index < state.Owners.size(); ++index) {
			const auto &owner = state.Owners[index];
			if (!ValidText(owner.NodeId) || Find(state.Nodes, owner.NodeId) == state.Nodes.end())
				return refuse(Refuse(Status::InvalidValue, "cache-group owner producer is absent"));
			for (size_t previous = 0; previous < index; ++previous)
				if (state.Owners[previous].NodeId == owner.NodeId)
					return refuse(
						Refuse(Status::DuplicateId, "cache-group owner identity repeats", owner.NodeId)
					);
			if (owner.Members.size() > Limits::MaximumLinks - memberships)
				return refuse(
					Refuse(Status::LimitExceeded, "cache-group memberships exceed bounds", owner.NodeId)
				);
			memberships += owner.Members.size();
			for (const auto &member : owner.Members)
				if (!ValidText(member) || Find(state.Nodes, member) == state.Nodes.end())
					return refuse(
						Refuse(Status::InvalidValue, "cache-group member producer is absent", owner.NodeId)
					);
		}
		return Status::Ok;
	} catch (const std::bad_alloc &) {
		diagnostic = {Status::LimitExceeded, {}, {}, "cache-group validation allocation refused"};
		return diagnostic.Code;
	}
	CacheGroupReplayChange RetainCacheGroupReplayNode(
		CacheGroupReplayState &state,
		std::string_view nodeId,
		std::string_view nodeType,
		std::span<const CacheGroupReplayOutput> outputs,
		uint64_t maximumBytes
	) try {
		ENGINE_PROFILE("imagegraph.cache_group.retain");
		maximumBytes = std::min(maximumBytes, Limits::MaximumEvaluationBytes);
		const auto work = MeshAddBytes(
			WorkProduct(ComparisonWork(state), 3),
			MeshAddBytes(
				OutputWork(outputs),
				WorkProduct(state.Nodes.size() * 3, std::max(nodeId.size(), nodeType.size()) + 1)
			)
		);
		if (work > COMPARISON_WORK_LIMIT)
			return Refuse(Status::LimitExceeded, "cache-group snapshot exceeds comparison work bounds");
		Diagnostic diagnostic;
		const auto valid = ValidateCacheGroupReplay(state, maximumBytes, diagnostic);
		if (valid != Status::Ok) return {valid, false, std::move(diagnostic)};
		if (!ValidText(nodeId) || !ValidText(nodeType))
			return Refuse(Status::InvalidValue, "cache-group producer identity is invalid");
		if (outputs.size() > MAXIMUM_OUTPUTS_PER_NODE)
			return Refuse(Status::LimitExceeded, "cache-group producer exceeds output bounds", nodeId);
		auto existing = Find(state.Nodes, nodeId);
		if (existing != state.Nodes.end() && existing->NodeType != nodeType)
			return Refuse(Status::InvalidValue, "cache-group producer type changed", nodeId);
		if (existing == state.Nodes.end() && state.Nodes.size() == Limits::MaximumNodes)
			return Refuse(Status::LimitExceeded, "cache-group producer count exceeds bounds", nodeId);
		uint64_t extra = sizeof(CacheGroupReplayNode) + std::max(nodeId.size(), std::string{}.capacity()) +
						 std::max(nodeType.size(), std::string{}.capacity()) + std::string{}.capacity() +
						 outputs.size() * sizeof(CacheGroupReplayOutput);
		if (existing == state.Nodes.end())
			extra = MeshAddBytes(extra, state.Nodes.size() * sizeof(CacheGroupReplayNode));
		for (const auto &output : outputs) {
			if (output.Data && !ReplayValueCloneBytes(*output.Data))
				return Refuse(Status::InvalidValue, "cache-group output payload is invalid", nodeId);
			extra = MeshAddBytes(extra, OutputBytes(output, false));
		}
		extra = MeshAddBytes(
			extra, std::max(ValidationWorkspace(state), uint64_t(outputs.size() * sizeof(size_t)))
		);
		const auto overlap =
			MeshAddBytes(RetainedCacheGroupReplayBytes(state), MeshAddBytes(StateBytes(state, false), extra));
		if (overlap > maximumBytes)
			return Refuse(Status::LimitExceeded, "cache-group snapshot overlap exceeds byte budget", nodeId);
		auto checked = ValidateOutputs(outputs, nodeId);
		if (checked.Code != Status::Ok) return checked;
		CacheGroupReplayState candidate = state;
		if (existing == state.Nodes.end()) candidate.Nodes.reserve(state.Nodes.size() + 1);
		auto found = Find(candidate.Nodes, nodeId);
		if (found == candidate.Nodes.end()) {
			candidate.Nodes.push_back(
				{std::string(nodeId), std::string(nodeType), {}, true, {outputs.begin(), outputs.end()}}
			);
		} else
			found->Outputs.assign(outputs.begin(), outputs.end());
		return Publish(state, std::move(candidate), maximumBytes);
	} catch (const std::bad_alloc &) {
		return Refuse(Status::LimitExceeded, "cache-group snapshot allocation refused");
	}
	CacheGroupReplayChange ApplyCacheGroupReplay(
		CacheGroupReplayState &state, const CacheGroupReplayOperation &operation, uint64_t maximumBytes
	) try {
		ENGINE_PROFILE("imagegraph.cache_group.membership");
		maximumBytes = std::min(maximumBytes, Limits::MaximumEvaluationBytes);
		if (operation.Members.size() > Limits::MaximumLinks)
			return Refuse(Status::LimitExceeded, "cache-group operation exceeds membership bounds");
		const auto baseWork = WorkProduct(ComparisonWork(state), 3);
		if (baseWork > COMPARISON_WORK_LIMIT)
			return Refuse(Status::LimitExceeded, "cache-group mutation exceeds comparison work bounds");
		uint64_t longest = std::max(operation.OwnerId.size(), operation.MemberId.size()) + 1;
		for (const auto &member : operation.Members)
			longest = std::max(longest, uint64_t(member.size() + 1));
		for (const auto &node : state.Nodes)
			longest = std::max(longest, uint64_t(node.NodeId.size() + 1));
		uint64_t memberScans = 0;
		for (const auto &owner : state.Owners) {
			memberScans += owner.Members.size();
			longest = std::max(longest, uint64_t(owner.NodeId.size() + 1));
			for (const auto &member : owner.Members)
				longest = std::max(longest, uint64_t(member.size() + 1));
		}
		const auto scans = state.Nodes.size() * (6ull + 3ull * operation.Members.size() + memberScans) +
						   state.Owners.size() * 4ull + memberScans;
		if (MeshAddBytes(baseWork, WorkProduct(scans, longest)) > COMPARISON_WORK_LIMIT)
			return Refuse(Status::LimitExceeded, "cache-group mutation exceeds comparison work bounds");
		Diagnostic diagnostic;
		const auto valid = ValidateCacheGroupReplay(state, maximumBytes, diagnostic);
		if (valid != Status::Ok) return {valid, false, std::move(diagnostic)};
		if (operation.Action > CacheGroupReplayAction::DestroyOwner || !ValidText(operation.OwnerId) ||
			!ValidText(operation.MemberId, true) || operation.Members.size() > Limits::MaximumLinks)
			return Refuse(Status::InvalidValue, "cache-group operation is invalid");
		const bool memberEdit = operation.Action == CacheGroupReplayAction::TransferMember ||
								operation.Action == CacheGroupReplayAction::RemoveMember;
		if (memberEdit &&
			(!ValidText(operation.MemberId) || Find(state.Nodes, operation.MemberId) == state.Nodes.end()))
			return Refuse(Status::InvalidValue, "cache-group operation member is absent");
		auto priorOwner = Find(state.Owners, operation.OwnerId);
		if ((priorOwner == state.Owners.end() && operation.Action != CacheGroupReplayAction::RefreshOwner) ||
			Find(state.Nodes, operation.OwnerId) == state.Nodes.end())
			return Refuse(Status::InvalidValue, "cache-group operation owner is absent");
		if (operation.Action == CacheGroupReplayAction::Disable &&
			(!ValidFrameTime(operation.Project.ProjectFrame) ||
			 !std::isfinite(operation.Project.ProjectLastFrame)))
			return Refuse(Status::InvalidValue, "cache-group disable project clock is invalid");
		size_t memberships = 0;
		for (const auto &owner : state.Owners)
			memberships += owner.Members.size();
		if (operation.Action == CacheGroupReplayAction::RefreshOwner) {
			if (priorOwner != state.Owners.end()) memberships -= priorOwner->Members.size();
			for (const auto &member : operation.Members)
				if (Find(state.Nodes, member) != state.Nodes.end() && ++memberships > Limits::MaximumLinks)
					return Refuse(Status::LimitExceeded, "cache-group memberships exceed bounds");
		} else if (operation.Action == CacheGroupReplayAction::TransferMember &&
				   memberships == Limits::MaximumLinks) {
			const auto node = Find(state.Nodes, operation.MemberId);
			const auto oldOwner = Find(state.Owners, node->OwnerId);
			if (node->OwnerId != operation.OwnerId &&
				(oldOwner == state.Owners.end() ||
				 std::count(oldOwner->Members.begin(), oldOwner->Members.end(), operation.MemberId) == 0))
				return Refuse(Status::LimitExceeded, "cache-group memberships exceed bounds");
		}
		uint64_t extra = ValidationWorkspace(state) + sizeof(CacheGroupReplayOwner) +
						 std::max(operation.OwnerId.size(), std::string{}.capacity()) +
						 std::max(operation.MemberId.size(), std::string{}.capacity()) + sizeof(std::string);
		if (priorOwner == state.Owners.end())
			extra = MeshAddBytes(extra, state.Owners.size() * sizeof(CacheGroupReplayOwner));
		if (operation.Action == CacheGroupReplayAction::TransferMember)
			extra = MeshAddBytes(extra, priorOwner->Members.size() * sizeof(std::string));
		for (const auto &member : operation.Members) {
			if (!ValidText(member))
				return Refuse(Status::InvalidValue, "cache-group refresh member identity is invalid");
			extra = MeshAddBytes(extra, sizeof(std::string) + StringBytes(member, false));
			// Refresh also copies one owner pointer for each extant member.
			extra = MeshAddBytes(extra, std::max(operation.OwnerId.size(), std::string{}.capacity()));
		}
		if (MeshAddBytes(
				RetainedCacheGroupReplayBytes(state), MeshAddBytes(StateBytes(state, false), extra)
			) > maximumBytes)
			return Refuse(Status::LimitExceeded, "cache-group mutation overlap exceeds byte budget");
		CacheGroupReplayState candidate = state;
		if (priorOwner == state.Owners.end()) candidate.Owners.reserve(state.Owners.size() + 1);
		auto owner = Find(candidate.Owners, operation.OwnerId);
		bool clear = false;
		switch (operation.Action) {
		case CacheGroupReplayAction::RefreshOwner:
			if (owner == candidate.Owners.end()) {
				candidate.Owners.push_back({std::string(operation.OwnerId), operation.Serialize, {}});
				owner = std::prev(candidate.Owners.end());
			}
			owner->Serialize = operation.Serialize;
			owner->Members.clear();
			owner->Members.reserve(operation.Members.size());
			for (const auto &member : operation.Members) {
				auto node = Find(candidate.Nodes, member);
				if (node == candidate.Nodes.end()) continue;
				owner->Members.push_back(member);
				node->OwnerId = operation.OwnerId;
			}
			break;
		case CacheGroupReplayAction::TransferMember: {
			auto node = Find(candidate.Nodes, operation.MemberId);
			if (node->OwnerId == operation.OwnerId) break;
			if (!node->OwnerId.empty()) {
				auto oldOwner = Find(candidate.Owners, node->OwnerId);
				std::erase(oldOwner->Members, std::string(operation.MemberId));
				node->RenderActive = true;
			}
			owner->Members.reserve(owner->Members.size() + 1);
			owner->Members.emplace_back(operation.MemberId);
			node->OwnerId = operation.OwnerId;
			break;
		}
		case CacheGroupReplayAction::RemoveMember: {
			auto node = Find(candidate.Nodes, operation.MemberId);
			if (node->OwnerId != operation.OwnerId) break;
			std::erase(owner->Members, std::string(operation.MemberId));
			node->OwnerId.clear();
			node->RenderActive = true;
			break;
		}
		case CacheGroupReplayAction::SetSerialize:
			owner->Serialize = operation.Serialize;
			break;
		case CacheGroupReplayAction::Enable:
			if (EnableAllowed(*owner, operation)) {
				SetActivity(candidate, *owner, true);
				clear = true;
			}
			break;
		case CacheGroupReplayAction::Disable:
			if (EnableAllowed(*owner, operation) && operation.Playing &&
				SourceFrameCacheIsLastProjectFrame(operation.Project))
				SetActivity(candidate, *owner, false);
			break;
		case CacheGroupReplayAction::DestroyOwner:
			if (EnableAllowed(*owner, operation)) {
				SetActivity(candidate, *owner, true);
				clear = true;
			}
			for (auto &node : candidate.Nodes)
				if (node.OwnerId == operation.OwnerId) node.OwnerId.clear();
			candidate.Owners.erase(owner);
			break;
		}
		return Publish(state, std::move(candidate), maximumBytes, clear);
	} catch (const std::bad_alloc &) {
		return Refuse(Status::LimitExceeded, "cache-group mutation allocation refused");
	}
	bool CacheGroupReplayShouldRun(
		const CacheGroupReplayState &state, std::string_view nodeId, bool explicitRenderList
	) {
		if (explicitRenderList) return true;
		const auto found = Find(state.Nodes, nodeId);
		return found == state.Nodes.end() || found->RenderActive;
	}
	Status InitializeAuthoredCacheGroupReplay(
		const Document &document,
		const CacheGroupReplayState &source,
		CacheGroupReplayState &output,
		uint64_t maximumBytes,
		Diagnostic &diagnostic
	) try {
		ENGINE_PROFILE("imagegraph.cache_group.initialize");
		diagnostic = {};
		const auto refuse = [&](Status code,
								std::string_view message,
								std::string_view node = {},
								std::string_view port = {}) {
			diagnostic = {code, std::string(node), std::string(port), std::string(message)};
			return code;
		};
		if (!maximumBytes || maximumBytes > Limits::MaximumEvaluationBytes ||
			document.Nodes.size() > Limits::MaximumNodes)
			return refuse(Status::LimitExceeded, "cache-group initialization cap is outside bounds");
		uint64_t work = ComparisonWork(source);
		if (work > COMPARISON_WORK_LIMIT || output.Nodes.size() > Limits::MaximumNodes ||
			output.Owners.size() > Limits::MaximumNodes)
			return refuse(
				Status::LimitExceeded, "cache-group initialization prior exceeds record or work bounds"
			);
		detail::EvaluationBudget budget(maximumBytes);
		auto prior = budget.Reserve(MeshAddBytes(
			RetainedCacheGroupReplayBytes(source),
			&source == &output ? 0 : RetainedCacheGroupReplayBytes(output)
		));
		if (!prior)
			return refuse(Status::LimitExceeded, "cache-group initialization prior exceeds live bytes");
		{
			auto workspace = budget.Reserve(ValidationWorkspace(source));
			if (!workspace)
				return refuse(
					Status::LimitExceeded, "cache-group initialization source validation exceeds live bytes"
				);
			if (ValidateCacheGroupReplay(source, maximumBytes, diagnostic) != Status::Ok)
				return diagnostic.Code;
		}
		using Indices = detail::EvaluationVector<size_t>;
		Indices sorted{detail::EvaluationAllocator<size_t>(budget)};
		Indices records{detail::EvaluationAllocator<size_t>(budget)};
		Indices ownerRecords{detail::EvaluationAllocator<size_t>(budget)};
		Indices needed{detail::EvaluationAllocator<size_t>(budget)};
		sorted.resize(document.Nodes.size());
		records.resize(document.Nodes.size(), SIZE_MAX);
		ownerRecords.resize(document.Nodes.size(), SIZE_MAX);
		needed.resize(document.Nodes.size(), 0);
		uint64_t longest = 1;
		for (size_t index = 0; index < document.Nodes.size(); ++index) {
			const auto &node = document.Nodes[index];
			if (!ValidText(node.Id) || !ValidText(node.Type) ||
				node.SourceProperties.size() > Limits::MaximumPropertiesPerNode)
				return refuse(
					Status::InvalidValue, "cache-group authored identity or metadata is invalid", node.Id
				);
			longest = std::max(longest, uint64_t(node.Id.size() + 1));
			sorted[index] = index;
		}
		size_t levels = 1;
		for (size_t count = sorted.size(); count > 1; count = (count + 1) / 2)
			++levels;
		const auto spend = [&](uint64_t bytes) {
			if (bytes > COMPARISON_WORK_LIMIT - std::min(work, COMPARISON_WORK_LIMIT)) return false;
			work += bytes;
			return true;
		};
		if (!spend(WorkProduct(sorted.size() * (4 * levels + 1), longest)))
			return refuse(Status::LimitExceeded, "cache-group document index exceeds comparison work");
		std::sort(sorted.begin(), sorted.end(), [&](size_t a, size_t b) {
			return document.Nodes[a].Id < document.Nodes[b].Id;
		});
		for (size_t index = 1; index < sorted.size(); ++index)
			if (document.Nodes[sorted[index - 1]].Id == document.Nodes[sorted[index]].Id)
				return refuse(
					Status::DuplicateId,
					"cache-group document has duplicate node IDs",
					document.Nodes[sorted[index]].Id
				);
		bool exhausted = false;
		const auto lookup = [&](std::string_view id) -> size_t {
			if (!spend(WorkProduct(4 * levels, std::max(longest, uint64_t(id.size() + 1))))) {
				exhausted = true;
				return SIZE_MAX;
			}
			const auto found =
				std::lower_bound(sorted.begin(), sorted.end(), id, [&](size_t index, std::string_view text) {
					return document.Nodes[index].Id < text;
				});
			return found != sorted.end() && document.Nodes[*found].Id == id ? *found : SIZE_MAX;
		};
		for (size_t index = 0; index < source.Nodes.size(); ++index) {
			const auto found = lookup(source.Nodes[index].NodeId);
			if (exhausted)
				return refuse(Status::LimitExceeded, "cache-group source index exceeds comparison work");
			if (found != SIZE_MAX) records[found] = index;
		}
		for (size_t index = 0; index < source.Owners.size(); ++index) {
			const auto found = lookup(source.Owners[index].NodeId);
			if (exhausted)
				return refuse(Status::LimitExceeded, "cache-group owner index exceeds comparison work");
			if (found != SIZE_MAX) ownerRecords[found] = index;
		}
		struct OwnerPlan {
			size_t Index;
			const ArrayValue *Members;
			bool Serialize;
		};
		detail::EvaluationVector<OwnerPlan> owners{detail::EvaluationAllocator<OwnerPlan>(budget)};
		size_t totalMembers = 0;
		for (size_t index = 0; index < document.Nodes.size(); ++index) {
			const auto &node = document.Nodes[index];
			if (node.Type != "pc.cache" && node.Type != "pc.cache_array") continue;
			const ArrayValue *members = nullptr;
			bool serialize = true, seenSerialize = false, seenGroup = false;
			for (const auto &property : node.SourceProperties) {
				if (property.Port == "serialize") {
					const auto *flag = std::get_if<bool>(&property.Data);
					if (seenSerialize || !flag)
						return refuse(
							Status::InvalidValue,
							"Serialize must be one source boolean",
							node.Id,
							property.Port
						);
					serialize = *flag;
					seenSerialize = true;
				} else if (property.Port == "cache_group") {
					members = std::get_if<ArrayValue>(&property.Data);
					if (seenGroup || !members || members->ElementType != ValueType::Text ||
						!members->Items.empty() || !members->Nested.empty() ||
						members->Elements.size() > Limits::MaximumNodes ||
						!detail::ValidRuntimeValue(property.Data))
						return refuse(
							Status::InvalidValue,
							"Cache Group must be one bounded string array",
							node.Id,
							property.Port
						);
					seenGroup = true;
				}
			}
			needed[index] = 1;
			owners.push_back({index, members, serialize});
			if (members)
				for (const auto &element : members->Elements) {
					const auto *id = std::get_if<std::string>(&element);
					if (!id || !ValidText(*id))
						return refuse(
							Status::InvalidValue, "Cache Group member ID is invalid", node.Id, "cache_group"
						);
					const auto found = lookup(*id);
					if (exhausted)
						return refuse(
							Status::LimitExceeded, "cache-group membership exceeds comparison work"
						);
					if (found == SIZE_MAX) continue;
					if (++totalMembers > Limits::MaximumLinks)
						return refuse(
							Status::LimitExceeded, "authored cache-group memberships exceed bounds"
						);
					needed[found] = 1;
				}
		}
		size_t newNodes = 0, newOwners = 0;
		uint64_t extra = 0;
		for (size_t index = 0; index < needed.size(); ++index) {
			if (!needed[index]) continue;
			const auto &node = document.Nodes[index];
			if (records[index] != SIZE_MAX) {
				if (source.Nodes[records[index]].NodeType != node.Type)
					return refuse(
						Status::InvalidValue,
						"authored cache-group producer type changed; reconcile before loading",
						node.Id
					);
				continue;
			}
			++newNodes;
			const auto *entry = FindCatalogueEntry(node.Type);
			const size_t ports = (entry ? entry->Outputs.size() : 0) + node.DynamicOutputs.size();
			if (ports > MAXIMUM_OUTPUTS_PER_NODE)
				return refuse(Status::LimitExceeded, "cold producer exceeds output bounds", node.Id);
			extra = MeshAddBytes(
				extra,
				sizeof(CacheGroupReplayNode) + std::max(node.Id.size(), std::string{}.capacity()) +
					std::max(node.Type.size(), std::string{}.capacity()) + std::string{}.capacity() +
					ports * sizeof(CacheGroupReplayOutput)
			);
			const auto portBytes = [&](std::string_view id, std::string_view expression) {
				return uint64_t(
					std::max(id.size(), std::string{}.capacity()) + node.Id.size() + id.size() +
					expression.size() + 128
				);
			};
			if (entry)
				for (const auto &port : entry->Outputs)
					extra = MeshAddBytes(extra, portBytes(port.Id, port.ConstructorExpression));
			for (const auto &port : node.DynamicOutputs)
				extra = MeshAddBytes(extra, portBytes(port.Id, {}));
		}
		for (const auto &owner : owners) {
			const auto &node = document.Nodes[owner.Index];
			if (ownerRecords[owner.Index] == SIZE_MAX) {
				++newOwners;
				extra = MeshAddBytes(
					extra, sizeof(CacheGroupReplayOwner) + std::max(node.Id.size(), std::string{}.capacity())
				);
			}
			if (owner.Members)
				for (const auto &element : owner.Members->Elements) {
					const auto &id = std::get<std::string>(element);
					extra = MeshAddBytes(
						extra,
						sizeof(std::string) + std::max(id.size(), std::string{}.capacity()) +
							std::max(node.Id.size(), std::string{}.capacity())
					);
				}
		}
		if (newNodes > Limits::MaximumNodes - source.Nodes.size() ||
			newOwners > Limits::MaximumNodes - source.Owners.size())
			return refuse(Status::LimitExceeded, "authored cache-group record count exceeds bounds");
		auto clone = budget.Reserve(MeshAddBytes(StateBytes(source, false), extra));
		if (!clone) return refuse(Status::LimitExceeded, "authored cache-group clone exceeds live bytes");
		CacheGroupReplayState candidate;
		candidate.Nodes.reserve(source.Nodes.size() + newNodes);
		candidate.Owners.reserve(source.Owners.size() + newOwners);
		candidate.Nodes.assign(source.Nodes.begin(), source.Nodes.end());
		candidate.Owners.assign(source.Owners.begin(), source.Owners.end());
		detail::AllocationReservation constructors;
		for (size_t index = 0; index < needed.size(); ++index) {
			if (!needed[index] || records[index] != SIZE_MAX) continue;
			const auto &node = document.Nodes[index];
			CacheGroupReplayNode record;
			record.NodeId = std::string(node.Id);
			record.NodeType = std::string(node.Type);
			const auto *entry = FindCatalogueEntry(node.Type);
			record.Outputs.reserve((entry ? entry->Outputs.size() : 0) + node.DynamicOutputs.size());
			const auto append =
				[&](std::string_view id, std::string_view initial, std::string_view expression) -> Status {
				CacheGroupReplayOutput port;
				port.Port = std::string(id);
				if (!initial.empty()) {
					Value value;
					const auto parsed = detail::ReadValueText(initial, value, budget, constructors);
					if (parsed != Status::Ok)
						return refuse(
							parsed, "source cold constructor literal could not be admitted", node.Id, id
						);
					port.Data = std::move(value);
				} else {
					constexpr std::string_view prefix =
						"source cold constructor requires runtime execution: ";
					std::string message;
					message.reserve(prefix.size() + expression.size());
					message.append(prefix).append(expression);
					port.Refusal = Diagnostic{
						Status::UnsupportedExecution, node.Id, std::string(id), std::move(message)
					};
				}
				record.Outputs.push_back(std::move(port));
				return Status::Ok;
			};
			if (entry)
				for (const auto &port : entry->Outputs) {
					if (append(port.Id, port.ConstructorDefault, port.ConstructorExpression) != Status::Ok)
						return diagnostic.Code;
				}
			for (const auto &port : node.DynamicOutputs)
				if (append(port.Id, {}, "dynamic output") != Status::Ok) return diagnostic.Code;
			records[index] = candidate.Nodes.size();
			candidate.Nodes.push_back(std::move(record));
		}
		for (const auto &plan : owners) {
			const auto &node = document.Nodes[plan.Index];
			if (ownerRecords[plan.Index] == SIZE_MAX) {
				ownerRecords[plan.Index] = candidate.Owners.size();
				candidate.Owners.push_back({node.Id, plan.Serialize, {}});
			}
			auto &owner = candidate.Owners[ownerRecords[plan.Index]];
			owner.Serialize = plan.Serialize;
			std::vector<std::string> members;
			if (plan.Members) {
				members.reserve(plan.Members->Elements.size());
				for (const auto &element : plan.Members->Elements) {
					const auto &id = std::get<std::string>(element);
					const auto found = lookup(id);
					if (exhausted)
						return refuse(
							Status::LimitExceeded, "cache-group loaded refresh exceeds comparison work"
						);
					if (found == SIZE_MAX) continue;
					members.push_back(id);
					candidate.Nodes[records[found]].OwnerId = std::string(node.Id);
				}
			}
			owner.Members = std::move(members);
		}
		if (!spend(ComparisonWork(candidate)))
			return refuse(
				Status::LimitExceeded,
				"cache-group initialization validation exceeds whole-operation comparison work"
			);
		auto workspace = budget.Reserve(ValidationWorkspace(candidate));
		if (!workspace)
			return refuse(Status::LimitExceeded, "cache-group initialization validation exceeds live bytes");
		if (ValidateCacheGroupReplay(candidate, maximumBytes, diagnostic) != Status::Ok)
			return diagnostic.Code;
		if (RetainedCacheGroupReplayBytes(candidate) > clone->Bytes() + constructors.Bytes())
			return refuse(Status::LimitExceeded, "cache-group initialized capacities exceed admitted bytes");
		output = std::move(candidate);
		return Status::Ok;
	} catch (const std::bad_alloc &) {
		diagnostic = {Status::LimitExceeded, {}, {}, "cache-group initialization allocation refused"};
		return diagnostic.Code;
	}

	Status ReconcileCacheGroupReplay(
		const Document &document,
		const CacheGroupReplayState &source,
		CacheGroupReplayState &output,
		const std::optional<SourceFrameCacheProjectObservation> &project,
		uint64_t maximumBytes,
		Diagnostic &diagnostic
	) try {
		ENGINE_PROFILE("imagegraph.cache_group.reconcile");
		diagnostic = {};
		const auto refuse = [&](Status code, std::string_view message) {
			diagnostic = {code, {}, {}, std::string(message)};
			return code;
		};
		maximumBytes = std::min(maximumBytes, Limits::MaximumEvaluationBytes);
		const auto baseWork = WorkProduct(ComparisonWork(source), 4);
		if (document.Nodes.size() > Limits::MaximumNodes || baseWork > COMPARISON_WORK_LIMIT)
			return refuse(Status::LimitExceeded, "cache-group reconciliation exceeds comparison work bounds");
		uint64_t longest = 1;
		for (const auto &node : document.Nodes) {
			if (!ValidText(node.Id) || !ValidText(node.Type))
				return refuse(Status::InvalidValue, "cache-group document identity is invalid");
			longest = std::max({longest, uint64_t(node.Id.size() + 1), uint64_t(node.Type.size() + 1)});
		}
		for (const auto &node : source.Nodes)
			longest =
				std::max({longest, uint64_t(node.NodeId.size() + 1), uint64_t(node.NodeType.size() + 1)});
		if (MeshAddBytes(baseWork, WorkProduct(source.Nodes.size() * document.Nodes.size() * 8ull, longest)) >
			COMPARISON_WORK_LIMIT)
			return refuse(Status::LimitExceeded, "cache-group reconciliation exceeds document scan bounds");
		if (ValidateCacheGroupReplay(source, maximumBytes, diagnostic) != Status::Ok) return diagnostic.Code;
		const auto priorOutput = &source == &output ? 0 : RetainedCacheGroupReplayBytes(output);
		const auto overlap = MeshAddBytes(
			RetainedCacheGroupReplayBytes(source),
			MeshAddBytes(priorOutput, MeshAddBytes(StateBytes(source, false), ValidationWorkspace(source)))
		);
		if (overlap > maximumBytes)
			return refuse(Status::LimitExceeded, "cache-group reconciliation overlap exceeds byte bounds");
		const auto alive = [&](const CacheGroupReplayNode &record) {
			return std::any_of(document.Nodes.begin(), document.Nodes.end(), [&](const auto &node) {
				return node.Id == record.NodeId && node.Type == record.NodeType;
			});
		};
		for (const auto &owner : source.Owners)
			if (!alive(*Find(source.Nodes, owner.NodeId)) && owner.Serialize && !owner.Members.empty() &&
				!project)
				return refuse(
					Status::UnsupportedExecution,
					"cache-group owner retirement requires authoritative project observations"
				);
		CacheGroupReplayState candidate = source;
		for (const auto &owner : source.Owners) {
			if (alive(*Find(source.Nodes, owner.NodeId))) continue;
			CacheGroupReplayOperation operation{CacheGroupReplayAction::DestroyOwner, owner.NodeId};
			if (project) operation.Project = *project;
			if (EnableAllowed(owner, operation)) SetActivity(candidate, owner, true);
			for (auto &node : candidate.Nodes)
				if (node.OwnerId == owner.NodeId) node.OwnerId.clear();
		}
		std::erase_if(candidate.Owners, [&](const auto &owner) {
			return !alive(*Find(source.Nodes, owner.NodeId));
		});
		std::erase_if(candidate.Nodes, [&](const auto &node) { return !alive(node); });
		for (auto &owner : candidate.Owners)
			std::erase_if(owner.Members, [&](const auto &id) {
				return Find(candidate.Nodes, id) == candidate.Nodes.end();
			});
		if (ValidateCacheGroupReplay(candidate, maximumBytes, diagnostic) != Status::Ok)
			return diagnostic.Code;
		if (MeshAddBytes(
				RetainedCacheGroupReplayBytes(source),
				MeshAddBytes(priorOutput, RetainedCacheGroupReplayBytes(candidate))
			) > maximumBytes)
			return refuse(Status::LimitExceeded, "cache-group reconciled candidate exceeds live byte bounds");
		output = std::move(candidate);
		return Status::Ok;
	} catch (const std::bad_alloc &) {
		diagnostic = {Status::LimitExceeded, {}, {}, "cache-group reconciliation allocation refused"};
		return diagnostic.Code;
	}
}
