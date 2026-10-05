#include "CacheGroupReplayClone.hpp"
#include "ValuePayload.hpp"

#include <engine/core/Profiling.hpp>
#include <engine/imagegraph/FrameCacheReplay.hpp>
#include <engine/imagegraph/SourceFrameCacheProject.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <new>

namespace engine::imagegraph {
	std::string_view SourceFrameCacheRowType(const DataReplayEntry &entry) {
		if (entry.Values.size() < 2 || entry.Values[0].Frame != 0 || entry.Values[1].Frame != 1) return {};
		const auto *type = std::get_if<std::string>(&entry.Values[0].Data);
		return type && (*type == "pc.cache" || *type == "pc.cache_array") ? std::string_view(*type)
																		  : std::string_view{};
	}
	std::string_view SourceFrameCacheIdentity(const Node &node) {
		const std::string *saved = nullptr;
		for (const auto &property : node.SourceProperties)
			if (property.Port == "cache") saved = std::get_if<std::string>(&property.Data);
		return saved ? std::string_view(*saved) : std::string_view{};
	}
	std::string_view SourceFrameCacheSavedText(const Node &node) {
		bool serialize = true;
		for (const auto &property : node.SourceProperties)
			if (property.Port == "serialize") {
				const auto *flag = std::get_if<bool>(&property.Data);
				if (!flag) return {};
				serialize = *flag;
			}
		return serialize ? SourceFrameCacheIdentity(node) : std::string_view{};
	}
	const Value *SourceFrameCacheLastOutput(const DataReplayEntry &entry) {
		return SourceFrameCacheRowType(entry).empty() ? nullptr : &entry.Values[1].Data;
	}
	uint64_t ClearedSourceFrameCacheReplayBytes(const Node &node, const DataReplayState &source) {
		if ((node.Type != "pc.cache" && node.Type != "pc.cache_array") || node.Id.empty() ||
			node.Id.size() > Limits::MaximumTextBytes ||
			node.SourceProperties.size() > Limits::MaximumPropertiesPerNode ||
			source.Entries.size() > Limits::MaximumArrayElements)
			return UINT64_MAX;
		bool seenSerialize = false, seenCache = false;
		for (const auto &property : node.SourceProperties) {
			if (property.Port == "serialize") {
				if (seenSerialize || !std::holds_alternative<bool>(property.Data)) return UINT64_MAX;
				seenSerialize = true;
			} else if (property.Port == "cache") {
				if (seenCache || !std::holds_alternative<std::string>(property.Data)) return UINT64_MAX;
				seenCache = true;
			}
		}
		const auto saved = SourceFrameCacheIdentity(node);
		if (saved.size() > Limits::MaximumTextBytes) return UINT64_MAX;
		uint64_t bytes = RetainedDataReplayBytes(source);
		if (bytes > Limits::MaximumEvaluationBytes) return UINT64_MAX;
		bool found = false;
		for (const auto &row : source.Entries) {
			if (row.NodeId != node.Id) continue;
			if (SourceFrameCacheRowType(row) != node.Type || row.LoadedCacheData != saved) return UINT64_MAX;
			found = true;
		}
		// Charge the full clone before dropping slots. No decoded packet copy is needed.
		if (!found && source.Entries.size() == Limits::MaximumArrayElements) return UINT64_MAX;
		if (found) bytes += sizeof(DataReplayEntry);
		if (!found) {
			bytes += sizeof(DataReplayEntry) + 2 * sizeof(DataReplayValueFrame) +
					 std::max(node.Id.size(), std::string{}.capacity()) +
					 std::max(saved.size(), std::string{}.capacity()) + 64;
		}
		return bytes;
	}
	Status ClearSourceFrameCacheReplay(
		const Node &node,
		const DataReplayState &source,
		DataReplayState &output,
		Diagnostic &diagnostic,
		uint64_t maximumBytes
	) try {
		ENGINE_PROFILE("imagegraph.frame_cache.clear");
		const auto fail = [&](Status code, const char *message) {
			diagnostic = {code, node.Id, {}, message};
			return code;
		};
		if (!maximumBytes || maximumBytes > Limits::MaximumEvaluationBytes)
			return fail(Status::LimitExceeded, "frame-cache clear byte cap is outside bounds");
		const uint64_t replacement = ClearedSourceFrameCacheReplayBytes(node, source);
		if (replacement == UINT64_MAX)
			return fail(Status::InvalidValue, "frame-cache clear requires matching typed source rows");
		const uint64_t retained = RetainedDataReplayBytes(source);
		const uint64_t oldOutput = &source == &output ? 0 : RetainedDataReplayBytes(output) - sizeof(output);
		const bool hasRows = std::any_of(source.Entries.begin(), source.Entries.end(), [&](const auto &row) {
			return row.NodeId == node.Id;
		});
		const uint64_t workspace = DataReplayValidationWorkspaceBytes(source, hasRows ? 0 : 1);
		if (retained > maximumBytes || oldOutput > maximumBytes - retained ||
			replacement > maximumBytes - retained - oldOutput ||
			workspace > maximumBytes - retained - oldOutput - replacement)
			return fail(Status::LimitExceeded, "frame-cache clear replacement exceeds live byte bounds");
		if (ValidateDataReplay(source, maximumBytes, diagnostic) != Status::Ok) return diagnostic.Code;
		DataReplayState candidate;
		candidate.CacheGroups = source.CacheGroups;
		candidate.Entries.reserve(source.Entries.size() + (hasRows ? 0 : 1));
		bool found = false;
		for (const auto &row : source.Entries) {
			candidate.Entries.push_back(row);
			if (row.NodeId != node.Id) continue;
			found = true;
			auto &cleared = candidate.Entries.back();
			cleared.FrameCacheConstructorCleared = true;
			cleared.Values.erase(cleared.Values.begin() + 2, cleared.Values.end());
		}
		if (!found) {
			DataReplayEntry row;
			row.NodeId = node.Id;
			row.Initialized = true;
			row.FrameCacheConstructorCleared = true;
			row.PreviousValue = 1;
			row.LoadedCacheData = SourceFrameCacheIdentity(node);
			row.Values = {
				{0, node.Type},
				{1, node.Type == "pc.cache" ? Value{int64_t{-4}} : Value{ArrayValue{ValueType::Any, {}}}}
			};
			candidate.Entries.push_back(std::move(row));
		}
		output = std::move(candidate);
		diagnostic = {};
		return Status::Ok;
	} catch (const std::bad_alloc &) {
		diagnostic = {Status::LimitExceeded, node.Id, {}, "frame-cache clear allocation refused"};
		return diagnostic.Code;
	}
	namespace {
		enum class FrameCacheOperation { ClearButton, Enable, Disable };
		Status ApplyFrameCacheOperation(
			const Node &node,
			FrameCacheOperation operation,
			const DataReplayState &source,
			DataReplayState &output,
			bool playing,
			const std::optional<SourceFrameCacheProjectObservation> &project,
			Diagnostic &diagnostic,
			uint64_t maximumBytes
		) try {
			ENGINE_PROFILE("imagegraph.frame_cache.group_action");
			const auto fail = [&](Status code, const char *message) {
				diagnostic = {code, node.Id, {}, message};
				return code;
			};
			if (!maximumBytes || maximumBytes > Limits::MaximumEvaluationBytes)
				return fail(Status::LimitExceeded, "frame-cache action byte cap is outside bounds");
			if ((node.Type != "pc.cache" && node.Type != "pc.cache_array") || node.Id.empty() ||
				node.Id.size() > Limits::MaximumTextBytes ||
				node.SourceProperties.size() > Limits::MaximumPropertiesPerNode)
				return fail(Status::InvalidValue, "frame-cache action requires a bounded cache node");
			bool serialize = true, seenSerialize = false, seenCache = false;
			const ArrayValue *authoredGroup = nullptr;
			for (const auto &property : node.SourceProperties) {
				if (property.Port == "serialize") {
					const auto *flag = std::get_if<bool>(&property.Data);
					if (!flag || seenSerialize)
						return fail(
							Status::InvalidValue, "frame-cache action requires one typed Serialize flag"
						);
					serialize = *flag;
					seenSerialize = true;
				}
				if (property.Port == "cache") {
					if (seenCache || !std::holds_alternative<std::string>(property.Data))
						return fail(
							Status::InvalidValue, "frame-cache action requires one typed cache receipt"
						);
					seenCache = true;
				}
				if (property.Port != "cache_group") continue;
				const auto *group = std::get_if<ArrayValue>(&property.Data);
				if (authoredGroup || !group || group->ElementType != ValueType::Text ||
					!group->Items.empty() || !group->Nested.empty() ||
					group->Elements.size() > Limits::MaximumNodes ||
					!detail::ValidValuePayload(property.Data, true))
					return fail(Status::InvalidValue, "frame-cache action requires one bounded string group");
				authoredGroup = group;
			}
			const bool button = operation == FrameCacheOperation::ClearButton;
			if (!button && serialize && !project)
				return fail(
					Status::UnsupportedExecution, "frame-cache group action requires project observations"
				);
			if (operation == FrameCacheOperation::Disable && project &&
				(!ValidFrameTime(project->ProjectFrame) || !std::isfinite(project->ProjectLastFrame)))
				return fail(Status::InvalidValue, "frame-cache group disable project clock is invalid");
			const bool allowed =
				serialize && (!project || (!project->ProjectLoading && !project->ProjectAppending));
			const bool enable = operation != FrameCacheOperation::Disable && allowed;
			const bool disable = operation == FrameCacheOperation::Disable && allowed && playing && project &&
								 SourceFrameCacheIsLastProjectFrame(*project);
			const bool activityChange = enable || disable;
			const bool clearFrames = enable || (button && node.Type == "pc.cache");
			const uint64_t retained = RetainedDataReplayBytes(source);
			const uint64_t clearedBytes =
				clearFrames ? ClearedSourceFrameCacheReplayBytes(node, source) : retained;
			if (clearedBytes == UINT64_MAX)
				return fail(Status::InvalidValue, "frame-cache action requires matching typed source rows");
			const uint64_t oldOutput =
				&source == &output ? 0 : RetainedDataReplayBytes(output) - sizeof(output);
			const bool additionalRow =
				clearFrames &&
				std::none_of(source.Entries.begin(), source.Entries.end(), [&](const auto &row) {
					return row.NodeId == node.Id;
				});
			const uint64_t replacement = clearFrames ? clearedBytes : retained;
			const uint64_t workspace = DataReplayValidationWorkspaceBytes(source, additionalRow ? 1 : 0);
			if (retained > maximumBytes || oldOutput > maximumBytes - retained ||
				replacement > maximumBytes - retained - oldOutput ||
				workspace > maximumBytes - retained - oldOutput - replacement)
				return fail(Status::LimitExceeded, "frame-cache action replacement exceeds live byte bounds");
			if (ValidateDataReplay(source, maximumBytes - oldOutput - replacement, diagnostic) != Status::Ok)
				return diagnostic.Code;
			const auto owner = std::find_if(
				source.CacheGroups.Owners.begin(), source.CacheGroups.Owners.end(), [&](const auto &item) {
					return item.NodeId == node.Id;
				}
			);
			if (!button) {
				if (owner != source.CacheGroups.Owners.end()) {
					const auto ownedNode = std::find_if(
						source.CacheGroups.Nodes.begin(),
						source.CacheGroups.Nodes.end(),
						[&](const auto &item) { return item.NodeId == node.Id; }
					);
					if (ownedNode->NodeType != node.Type)
						return fail(
							Status::InvalidValue, "frame-cache group owner type does not match the action"
						);
				}
			}
			const bool nonempty = (authoredGroup && !authoredGroup->Elements.empty()) ||
								  (owner != source.CacheGroups.Owners.end() && !owner->Members.empty());
			if (serialize && nonempty && !project)
				return fail(
					Status::UnsupportedExecution, "frame-cache group action requires project observations"
				);
			if (activityChange && nonempty && owner == source.CacheGroups.Owners.end() &&
				(!button || !source.CacheGroups.Nodes.empty()))
				return fail(
					Status::UnsupportedExecution,
					"frame-cache group must be initialized before a group action"
				);
			if (activityChange && owner != source.CacheGroups.Owners.end()) {
				uint64_t nodeNames = 0, memberNames = 0;
				for (const auto &member : source.CacheGroups.Nodes)
					nodeNames += member.NodeId.size() + 1;
				for (const auto &member : owner->Members)
					memberNames += member.size() + 1;
				constexpr uint64_t WORK_LIMIT = 64ull * 1024 * 1024;
				if (nodeNames > WORK_LIMIT / std::max(size_t{1}, owner->Members.size()) ||
					memberNames > (WORK_LIMIT - nodeNames * owner->Members.size()) /
									  std::max(size_t{1}, source.CacheGroups.Nodes.size()))
					return fail(Status::LimitExceeded, "frame-cache group action exceeds name work bounds");
			}
			if (clearFrames) {
				if (ClearSourceFrameCacheReplay(node, source, output, diagnostic, maximumBytes) != Status::Ok)
					return diagnostic.Code;
			} else {
				DataReplayState candidate = source;
				output = std::move(candidate);
			}
			// Admitted copies own all strings and getters; activity changes allocate nothing.
			for (auto &current : output.CacheGroups.Owners) {
				if (current.NodeId != node.Id) continue;
				current.Serialize = serialize;
				if (!activityChange) continue;
				for (const auto &id : current.Members)
					for (auto &member : output.CacheGroups.Nodes)
						if (member.NodeId == id) member.RenderActive = enable;
			}
			diagnostic = {};
			return Status::Ok;
		} catch (const std::bad_alloc &) {
			diagnostic = {Status::LimitExceeded, node.Id, {}, "frame-cache action allocation refused"};
			return diagnostic.Code;
		}
	}
	Status ClearSourceFrameCacheButtonReplay(
		const Node &node,
		const DataReplayState &source,
		DataReplayState &output,
		const std::optional<SourceFrameCacheProjectObservation> &project,
		Diagnostic &diagnostic,
		uint64_t maximumBytes
	) {
		return ApplyFrameCacheOperation(
			node, FrameCacheOperation::ClearButton, source, output, false, project, diagnostic, maximumBytes
		);
	}
	Status ApplySourceFrameCacheGroupReplay(
		const Node &node,
		CacheGroupReplayAction action,
		const DataReplayState &source,
		DataReplayState &output,
		bool playing,
		const std::optional<SourceFrameCacheProjectObservation> &project,
		Diagnostic &diagnostic,
		uint64_t maximumBytes
	) {
		if (action != CacheGroupReplayAction::Enable && action != CacheGroupReplayAction::Disable) {
			diagnostic = {
				Status::InvalidValue, node.Id, {}, "frame-cache group action must enable or disable"
			};
			return diagnostic.Code;
		}
		return ApplyFrameCacheOperation(
			node,
			action == CacheGroupReplayAction::Enable ? FrameCacheOperation::Enable
													 : FrameCacheOperation::Disable,
			source,
			output,
			playing,
			project,
			diagnostic,
			maximumBytes
		);
	}
	static Status
	AdmitSourceCacheEditData(const DataReplayState &source, uint64_t &work, Diagnostic &diagnostic) {
		const auto fail = [&](Status code, const char *message) {
			diagnostic = {code, {}, {}, message};
			return code;
		};
		const auto charge = [&](uint64_t count, uint64_t bytes) {
			if (count && bytes > work / count) return false;
			work -= count * bytes;
			return true;
		};
		// grug reject record counts and aggregate clone work before retained payload traversal.
		if (source.Entries.size() > Limits::MaximumArrayElements ||
			!charge(1, detail::CacheGroupReplayComparisonWork(source.CacheGroups)))
			return fail(Status::LimitExceeded, "frame-cache source counts exceed work bounds");
		for (const auto &row : source.Entries) {
			if (row.Values.size() > Limits::MaximumArrayElements ||
				row.NodeId.size() > Limits::MaximumTextBytes ||
				row.LoadedCacheData.size() > Limits::MaximumTextBytes ||
				!charge(
					4,
					sizeof(row) + row.NodeId.size() + row.LoadedCacheData.size() +
						row.Values.size() * sizeof(DataReplayValueFrame)
				))
				return fail(Status::LimitExceeded, "frame-cache source rows exceed work bounds");
			for (const auto &frame : row.Values) {
				const auto clone = ValueClonePayloadBytes(frame.Data);
				if (!clone) return fail(Status::InvalidValue, "frame-cache source value is malformed");
				if (!charge(4, *clone))
					return fail(Status::LimitExceeded, "frame-cache source values exceed work bounds");
			}
		}
		if (!charge(2, detail::CacheGroupReplayCloneBytes(source.CacheGroups)))
			return fail(Status::LimitExceeded, "frame-cache group payload exceeds work bounds");
		return Status::Ok;
	}
	static Status EnableSourceFrameCacheEditedGroupsImpl(
		const Document &document,
		std::span<const std::string_view> editedNodes,
		const DataReplayState &source,
		DataReplayState &output,
		const std::optional<SourceFrameCacheProjectObservation> &project,
		Diagnostic &diagnostic,
		uint64_t maximumBytes,
		uint64_t &work
	) try {
		ENGINE_PROFILE("imagegraph.frame_cache.input_edits");
		const auto fail = [&](Status code, const char *message) {
			diagnostic = {code, {}, {}, message};
			return code;
		};
		if (!maximumBytes || maximumBytes > Limits::MaximumEvaluationBytes ||
			document.Nodes.size() > Limits::MaximumNodes || document.Groups.size() > Limits::MaximumGroups ||
			editedNodes.size() > Limits::MaximumNodes)
			return fail(Status::LimitExceeded, "frame-cache input edits exceed count or byte bounds");
		if (work > SOURCE_FRAME_CACHE_EDIT_WORK_BYTES)
			return fail(Status::LimitExceeded, "frame-cache input work cap is outside bounds");
		bool workExceeded = false;
		const auto charge = [&](uint64_t count, uint64_t bytes) {
			if (count && bytes > work / count) {
				workExceeded = true;
				return false;
			}
			work -= count * bytes;
			return true;
		};
		const auto same = [&](std::string_view left, std::string_view right) {
			return charge(1, std::max(left.size(), right.size()) + 1) && left == right;
		};
		if (AdmitSourceCacheEditData(source, work, diagnostic) != Status::Ok) return diagnostic.Code;
		const uint64_t retained = RetainedDataReplayBytes(source);
		const uint64_t oldOutput = &source == &output ? 0 : RetainedDataReplayBytes(output);
		const uint64_t initialWorkspace = DataReplayValidationWorkspaceBytes(source);
		if (retained > maximumBytes || oldOutput > maximumBytes - retained ||
			initialWorkspace > maximumBytes - retained - oldOutput)
			return fail(Status::LimitExceeded, "frame-cache input edit validation exceeds live byte bounds");
		if (ValidateDataReplay(source, maximumBytes - oldOutput, diagnostic) != Status::Ok)
			return diagnostic.Code;
		std::array<const Node *, Limits::MaximumNodes> owners{};
		size_t ownerCount = 0;
		const auto select = [&](std::string_view id) {
			const CacheGroupReplayNode *member = nullptr;
			for (const auto &node : source.CacheGroups.Nodes) {
				if (same(node.NodeId, id)) {
					member = &node;
					break;
				}
				if (workExceeded) return;
			}
			if (!member || member->OwnerId.empty()) return;
			for (size_t i = 0; i < ownerCount; ++i)
				if (same(owners[i]->Id, member->OwnerId) || workExceeded) return;
			for (const auto &node : document.Nodes) {
				if (same(node.Id, member->OwnerId)) {
					owners[ownerCount++] = &node;
					return;
				}
				if (workExceeded) return;
			}
		};
		for (const auto id : editedNodes) {
			if (id.empty() || id.size() > Limits::MaximumTextBytes)
				return fail(Status::InvalidValue, "frame-cache edited node identity is invalid");
			select(id);
			std::string_view groupId;
			for (const auto &node : document.Nodes) {
				if (same(node.Id, id)) {
					groupId = node.GroupId;
					break;
				}
				if (workExceeded) break;
			}
			std::array<bool, Limits::MaximumGroups> visited{};
			while (!groupId.empty() && !workExceeded) {
				bool found = false;
				for (size_t i = 0; i < document.Groups.size(); ++i) {
					const auto &group = document.Groups[i];
					if (!same(group.Id, groupId)) {
						if (workExceeded) break;
						continue;
					}
					if (visited[i])
						return fail(Status::InvalidValue, "frame-cache edited group ancestry is cyclic");
					visited[i] = true;
					if (!group.OwnerNodeId.empty()) select(group.OwnerNodeId);
					groupId = group.ParentId;
					found = true;
					break;
				}
				if (!found && !workExceeded)
					return fail(Status::InvalidValue, "frame-cache edited group ancestry is missing");
			}
			if (workExceeded)
				return fail(Status::LimitExceeded, "frame-cache input edits exceed name work bounds");
		}
		if (!ownerCount && &source == &output) {
			diagnostic = {};
			return Status::Ok;
		}
		uint64_t candidateBytes = retained;
		size_t additionalRows = 0;
		for (size_t i = 0; i < ownerCount; ++i) {
			const auto &owner = *owners[i];
			if (!charge(8, candidateBytes))
				return fail(Status::LimitExceeded, "frame-cache input edits exceed batch copy work bounds");
			for (const auto &row : source.Entries)
				if (!charge(3, std::max(row.NodeId.size(), owner.Id.size()) + 1))
					return fail(Status::LimitExceeded, "frame-cache input edits exceed row work bounds");
			bool serialize = true;
			if (owner.SourceProperties.size() > Limits::MaximumPropertiesPerNode)
				return fail(Status::LimitExceeded, "frame-cache edited owner properties exceed bounds");
			for (const auto &property : owner.SourceProperties) {
				if (property.Port != "serialize") continue;
				const auto *flag = std::get_if<bool>(&property.Data);
				if (!flag)
					return fail(Status::InvalidValue, "frame-cache edited owner Serialize is not typed");
				serialize = *flag;
			}
			const bool clear =
				serialize && (!project || (!project->ProjectLoading && !project->ProjectAppending));
			if (clear) {
				const auto cleared = ClearedSourceFrameCacheReplayBytes(owner, source);
				if (cleared == UINT64_MAX)
					return fail(
						Status::InvalidValue, "frame-cache edited owner requires matching typed rows"
					);
				const auto extra = cleared - retained;
				if (extra > maximumBytes - std::min(candidateBytes, maximumBytes))
					return fail(
						Status::LimitExceeded, "frame-cache input edit candidates exceed byte bounds"
					);
				candidateBytes += extra;
				const bool present =
					std::any_of(source.Entries.begin(), source.Entries.end(), [&](const auto &row) {
						return row.NodeId == owner.Id;
					});
				additionalRows += !present;
			}
			// One batch owns the budget for every copy and member lookup, not one cap per owner.
			uint64_t names = 0;
			for (const auto &node : source.CacheGroups.Nodes)
				names += node.NodeId.size() + 1;
			for (const auto &group : source.CacheGroups.Owners) {
				if (!same(group.NodeId, owner.Id)) continue;
				if (!charge(group.Members.size(), names)) break;
				for (const auto &member : group.Members)
					if (!charge(source.CacheGroups.Nodes.size(), member.size() + 1)) break;
			}
			if (!charge(1, candidateBytes) || workExceeded)
				return fail(Status::LimitExceeded, "frame-cache input edits exceed batch work bounds");
		}
		const uint64_t workspace = DataReplayValidationWorkspaceBytes(source, additionalRows);
		// The outer transaction retains source and destination while each admitted action builds its clone.
		if (retained > maximumBytes || oldOutput > maximumBytes - retained ||
			candidateBytes > (maximumBytes - retained - oldOutput) / 2 ||
			workspace > maximumBytes - retained - oldOutput - 2 * candidateBytes)
			return fail(Status::LimitExceeded, "frame-cache input edit transaction exceeds live byte bounds");
		DataReplayState candidate = source;
		const auto actionBytes = maximumBytes - retained - oldOutput;
		for (size_t i = 0; i < ownerCount; ++i)
			if (ApplySourceFrameCacheGroupReplay(
					*owners[i],
					CacheGroupReplayAction::Enable,
					candidate,
					candidate,
					false,
					project,
					diagnostic,
					actionBytes
				) != Status::Ok)
				return diagnostic.Code;
		output = std::move(candidate);
		diagnostic = {};
		return Status::Ok;
	} catch (const std::bad_alloc &) {
		diagnostic = {Status::LimitExceeded, {}, {}, "frame-cache input edit allocation refused"};
		return diagnostic.Code;
	}
	Status EnableSourceFrameCacheEditedGroups(
		const Document &document,
		std::span<const std::string_view> editedNodes,
		const DataReplayState &source,
		DataReplayState &output,
		const std::optional<SourceFrameCacheProjectObservation> &project,
		Diagnostic &diagnostic,
		uint64_t maximumBytes
	) {
		uint64_t work = SOURCE_FRAME_CACHE_EDIT_WORK_BYTES;
		return EnableSourceFrameCacheEditedGroupsImpl(
			document, editedNodes, source, output, project, diagnostic, maximumBytes, work
		);
	}
	Status SynchronizeSourceFrameCacheEdits(
		const Document &document,
		std::span<const std::string_view> editedNodes,
		std::span<const std::string_view> membershipOwners,
		std::span<const std::string_view> serializeOwners,
		std::span<DataReplayState *const> journals,
		const std::optional<SourceFrameCacheProjectObservation> &project,
		Diagnostic &diagnostic,
		uint64_t maximumBytes,
		uint64_t maximumWork
	) try {
		ENGINE_PROFILE("imagegraph.frame_cache.authored_edits");
		const auto fail = [&](Status code, const char *message) {
			diagnostic = {code, {}, {}, message};
			return code;
		};
		if (!maximumBytes || maximumBytes > Limits::MaximumEvaluationBytes ||
			maximumWork > SOURCE_FRAME_CACHE_EDIT_WORK_BYTES || journals.empty() || journals.size() > 2 ||
			document.Nodes.size() > Limits::MaximumNodes || document.Groups.size() > Limits::MaximumGroups ||
			editedNodes.size() > Limits::MaximumNodes || membershipOwners.size() > Limits::MaximumNodes ||
			serializeOwners.size() > Limits::MaximumNodes)
			return fail(Status::LimitExceeded, "frame-cache authored transaction cap is outside bounds");
		std::array<DataReplayState, 2> candidates;
		uint64_t work = maximumWork, held = 0;
		for (size_t i = 0; i < journals.size(); ++i) {
			if (!journals[i]) return fail(Status::InvalidValue, "frame-cache authored journal is absent");
			for (size_t j = 0; j < i; ++j)
				if (journals[i] == journals[j])
					return fail(Status::InvalidValue, "frame-cache authored journals alias");
			if (AdmitSourceCacheEditData(*journals[i], work, diagnostic) != Status::Ok)
				return diagnostic.Code;
			held = detail::MeshAddBytes(held, RetainedDataReplayBytes(*journals[i]));
			held = detail::MeshAddBytes(held, RetainedDataReplayBytes(candidates[i]));
		}
		if (held >= maximumBytes)
			return fail(Status::LimitExceeded, "frame-cache authored journals exceed live bytes");
		for (size_t i = 0; i < journals.size(); ++i) {
			const auto empty = RetainedDataReplayBytes(candidates[i]);
			const auto cap = maximumBytes - held + RetainedDataReplayBytes(*journals[i]) + empty;
			const auto status = EnableSourceFrameCacheEditedGroupsImpl(
				document, {}, *journals[i], candidates[i], project, diagnostic, cap, work
			);
			if (status != Status::Ok) return status;
			held = detail::MeshAddBytes(held - empty, RetainedDataReplayBytes(candidates[i]));
			if (held >= maximumBytes)
				return fail(Status::LimitExceeded, "frame-cache authored candidates exceed live bytes");
		}
		uint64_t groupBytes = 0;
		std::array<CacheGroupReplayState *, 2> groups{};
		for (size_t i = 0; i < journals.size(); ++i) {
			groups[i] = &candidates[i].CacheGroups;
			groupBytes += RetainedCacheGroupReplayBytes(*groups[i]);
		}
		if (!membershipOwners.empty() || !serializeOwners.empty()) {
			const auto status = detail::SynchronizeAuthoredCacheGroupMetadata(
				document,
				membershipOwners,
				serializeOwners,
				std::span(groups).first(journals.size()),
				maximumBytes - held + groupBytes,
				diagnostic,
				work
			);
			if (status != Status::Ok) return status;
			uint64_t updated = 0;
			for (size_t i = 0; i < journals.size(); ++i)
				updated += RetainedCacheGroupReplayBytes(*groups[i]);
			held = detail::MeshAddBytes(held - groupBytes, updated);
		}
		if (!editedNodes.empty()) {
			for (size_t i = 0; i < journals.size(); ++i) {
				const auto before = RetainedDataReplayBytes(candidates[i]);
				if (held >= maximumBytes)
					return fail(Status::LimitExceeded, "frame-cache authored Enable exceeds live bytes");
				const auto status = EnableSourceFrameCacheEditedGroupsImpl(
					document,
					editedNodes,
					candidates[i],
					candidates[i],
					project,
					diagnostic,
					maximumBytes - held + before,
					work
				);
				if (status != Status::Ok) return status;
				held = detail::MeshAddBytes(held - before, RetainedDataReplayBytes(candidates[i]));
			}
		}
		if (held > maximumBytes)
			return fail(Status::LimitExceeded, "frame-cache authored result exceeds live bytes");
		for (size_t i = 0; i < journals.size(); ++i)
			*journals[i] = std::move(candidates[i]);
		diagnostic = {};
		return Status::Ok;
	} catch (const std::bad_alloc &) {
		diagnostic = {Status::LimitExceeded, {}, {}, "frame-cache authored edit allocation refused"};
		return diagnostic.Code;
	}

	Status OverlaySourceFrameCacheRows(
		const Document &document,
		const DataReplayState &retained,
		DataReplayState &target,
		FrameCacheOutputPolicy policy,
		Diagnostic &diagnostic,
		uint64_t maximumBytes
	) try {
		ENGINE_PROFILE("imagegraph.frame_cache.overlay");
		const auto fail = [&](Status code, const char *message) {
			diagnostic = {code, {}, {}, message};
			return code;
		};
		if (policy != FrameCacheOutputPolicy::RetainedObservation &&
			policy != FrameCacheOutputPolicy::PreObservation && policy != FrameCacheOutputPolicy::Constructor)
			return fail(Status::InvalidValue, "invalid frame-cache output observation policy");
		if (maximumBytes > Limits::MaximumEvaluationBytes || document.Nodes.size() > Limits::MaximumNodes ||
			retained.Entries.size() > Limits::MaximumArrayElements ||
			target.Entries.size() > Limits::MaximumArrayElements)
			return fail(Status::LimitExceeded, "frame-cache overlay exceeds count or byte bounds");
		// Charge worst-case membership/name work before scanning IDs or allocating copies.
		uint64_t names = 0;
		for (const auto &node : document.Nodes) {
			if (names >= 64ull * 1024 * 1024 || node.Id.size() >= 64ull * 1024 * 1024 - names)
				return fail(Status::LimitExceeded, "frame-cache membership exceeds work bounds");
			names += node.Id.size() + 1;
		}
		const uint64_t scans = 3 * (retained.Entries.size() + target.Entries.size());
		if (names > 64ull * 1024 * 1024 - 64)
			return fail(Status::LimitExceeded, "frame-cache membership exceeds work bounds");
		names += 64;
		if (scans && names > (64ull * 1024 * 1024) / scans)
			return fail(Status::LimitExceeded, "frame-cache membership exceeds work bounds");
		const uint64_t pairs = 3 * uint64_t(retained.Entries.size()) * target.Entries.size();
		uint64_t longest = 1;
		for (const auto &entry : retained.Entries)
			longest = std::max(longest, uint64_t(entry.NodeId.size() + 1));
		for (const auto &entry : target.Entries)
			longest = std::max(longest, uint64_t(entry.NodeId.size() + 1));
		if (pairs && longest > (64ull * 1024 * 1024 - scans * names) / pairs)
			return fail(Status::LimitExceeded, "frame-cache row merge exceeds work bounds");
		const auto alive = [&](const DataReplayEntry &entry) {
			const auto type = SourceFrameCacheRowType(entry);
			return !type.empty() &&
				   std::any_of(document.Nodes.begin(), document.Nodes.end(), [&](const auto &node) {
					   return node.Id == entry.NodeId && node.Type == type &&
							  (entry.LoadedCacheData.empty() ||
							   SourceFrameCacheIdentity(node) == entry.LoadedCacheData);
				   });
		};
		const uint64_t targetGroups = RetainedCacheGroupReplayBytes(target.CacheGroups);
		const uint64_t retainedGroups = RetainedCacheGroupReplayBytes(retained.CacheGroups);
		if (targetGroups > maximumBytes || retainedGroups > maximumBytes)
			return fail(Status::LimitExceeded, "frame-cache retained group owners exceed bounds");
		uint64_t copies =
			2 * (targetGroups - sizeof(target.CacheGroups)) + (retainedGroups - sizeof(retained.CacheGroups));
		if (copies > maximumBytes)
			return fail(Status::LimitExceeded, "frame-cache retained group owners exceed bounds");
		size_t count = 0;
		for (const auto &entry : target.Entries)
			if (SourceFrameCacheRowType(entry).empty() || alive(entry)) ++count;
		for (const auto &entry : target.Entries) {
			const uint64_t bytes = RetainedDataReplayEntryBytes(entry);
			if (bytes > (maximumBytes - std::min(copies, maximumBytes)) / 2)
				return fail(Status::LimitExceeded, "frame-cache target copy exceeds bounds");
			copies += bytes * 2;
		}
		for (const auto &entry : retained.Entries)
			if (alive(entry)) {
				const uint64_t bytes = RetainedDataReplayEntryBytes(entry);
				if (bytes > (maximumBytes - std::min(copies, maximumBytes)) / 2)
					return fail(Status::LimitExceeded, "frame-cache retained copy exceeds bounds");
				copies += bytes * 2;
				if (std::none_of(target.Entries.begin(), target.Entries.end(), [&](const auto &old) {
						return old.NodeId == entry.NodeId && old.ProcessorRow == entry.ProcessorRow &&
							   (SourceFrameCacheRowType(old).empty() ||
								SourceFrameCacheRowType(old) == SourceFrameCacheRowType(entry));
					}))
					++count;
			}
		if (count > Limits::MaximumArrayElements ||
			count * sizeof(DataReplayEntry) > maximumBytes - std::min(copies, maximumBytes))
			return fail(Status::LimitExceeded, "frame-cache overlay table exceeds bounds");
		copies += count * sizeof(DataReplayEntry);
		const uint64_t validation = std::max(retained.Entries.size(), target.Entries.size()) * sizeof(size_t);
		if (validation > maximumBytes - std::min(copies, maximumBytes))
			return fail(Status::LimitExceeded, "frame-cache validation workspace exceeds bounds");
		if (ValidateDataReplay(retained, Limits::MaximumEvaluationBytes, diagnostic) != Status::Ok ||
			ValidateDataReplay(target, Limits::MaximumEvaluationBytes, diagnostic) != Status::Ok)
			return diagnostic.Code;
		DataReplayState candidate;
		// A frame-row overlay keeps the target observation's producer activity and outputs.
		candidate.CacheGroups = target.CacheGroups;
		candidate.Entries.reserve(count);
		for (const auto &entry : target.Entries)
			if (SourceFrameCacheRowType(entry).empty() || alive(entry)) candidate.Entries.push_back(entry);
		for (const auto &entry : retained.Entries)
			if (alive(entry)) {
				const auto position =
					std::find_if(candidate.Entries.begin(), candidate.Entries.end(), [&](const auto &row) {
						return row.NodeId == entry.NodeId && row.ProcessorRow == entry.ProcessorRow;
					});
				DataReplayEntry replacement = entry;
				auto &output = replacement.Values[1].Data;
				if (policy == FrameCacheOutputPolicy::Constructor ||
					policy == FrameCacheOutputPolicy::PreObservation) {
					const Value *earlier =
						position == candidate.Entries.end() ? nullptr : SourceFrameCacheLastOutput(*position);
					if (policy == FrameCacheOutputPolicy::PreObservation && earlier)
						output = *earlier;
					else if (SourceFrameCacheRowType(entry) == "pc.cache")
						output = int64_t{-4};
					else
						output = ArrayValue{ValueType::Any, {}};
				}
				if (position == candidate.Entries.end())
					candidate.Entries.push_back(std::move(replacement));
				else
					*position = std::move(replacement);
			}
		target = std::move(candidate);
		diagnostic = {};
		return Status::Ok;
	} catch (const std::bad_alloc &) {
		diagnostic = {Status::LimitExceeded, {}, {}, "frame-cache overlay allocation failed"};
		return diagnostic.Code;
	}
}
