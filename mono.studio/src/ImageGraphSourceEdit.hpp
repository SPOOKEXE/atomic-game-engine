#pragma once

#include "ImageGraphGroupHost.hpp"
#include "ImageGraphHlslInputs.hpp"

namespace studio {
	// Authoring and the cached callback declarations commit together after undo
	// admission.
	template <class BeforeCommit>
	inline bool ApplyImageGraphSourceDynamicInput(
		engine::imagegraph::Document &document,
		ImageGraphHistory &history,
		ImageGraphGroupHost &host,
		uint64_t revision,
		std::string_view nodeId,
		const engine::imagegraph::DynamicInput &replacement,
		engine::imagegraph::EvaluationRequest request,
		engine::imagegraph::Diagnostic &error,
		const BeforeCommit &beforeCommit,
		const engine::imagegraph::EvaluationSnapshot *preparedInputs = nullptr
	) try {
		using namespace engine::imagegraph;
		const auto originalBytes = DocumentRetainedPayloadBytes(document);
		const auto valueBytes =
			replacement.Default ? ValueClonePayloadBytes(*replacement.Default) : std::optional<uint64_t>{0};
		const uint64_t names = nodeId.size() + replacement.Id.size() + replacement.SourceLayerName.size() +
							   replacement.SourceInputId.size();
		const auto refuse = [&] {
			error = {
				Status::LimitExceeded,
				std::string(nodeId),
				replacement.Id,
				"source input transaction exceeds its live payload or undo budget"
			};
			return false;
		};
		constexpr uint64_t maximum = Limits::MaximumEvaluationBytes;
		if (!originalBytes || !valueBytes || names > maximum || *valueBytes > maximum - names ||
			*valueBytes > (maximum - names) / 3 || *originalBytes > (maximum - names - 3 * *valueBytes) / 2)
			return refuse();
		Document staged = document;
		ImageGraphGroupHost stagedHost;
		stagedHost.BorrowedBytes = *originalBytes + *valueBytes + names;
		if (RebindGroupReplay(
				staged, host.Replay, revision, stagedHost.Replay, error, stagedHost.Budget(error)
			) != Status::Ok)
			return false;
		stagedHost.Revision = host.Revision;
		if (host.Replay.RetainedBytes() > maximum - stagedHost.BorrowedBytes) return refuse();
		stagedHost.BorrowedBytes += host.Replay.RetainedBytes();
		const auto originalNode =
			std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const auto &node) {
				return node.Id == nodeId;
			});
		std::vector<SourceAnimatorReplacement> replacedAnimators;
		if (originalNode != document.Nodes.end() && originalNode->Type == "pc.hlsl" &&
			detail::HlslRefreshControl(replacement.Id)) {
			if (!preparedInputs) {
				error = {
					Status::InvalidValue,
					std::string(nodeId),
					replacement.Id,
					"Shader refresh requires the prepared current input generation"
				};
				return false;
			}
			if (preparedInputs->RetainedBytes() > maximum - stagedHost.BorrowedBytes) return refuse();
			stagedHost.BorrowedBytes += preparedInputs->RetainedBytes();
			if (!detail::StageHlslArgumentRefresh(
					staged, nodeId, replacement, *preparedInputs, error, &replacedAnimators
				))
				return false;
		}
		if (!replacedAnimators.empty()) {
			GroupReplayState replacementReplay;
			if (RebindGroupReplayWithAnimatorReplacements(
					staged,
					replacedAnimators,
					stagedHost.Replay,
					revision,
					replacementReplay,
					error,
					stagedHost.Budget(error)
				) != Status::Ok)
				return false;
			stagedHost.Replay = std::move(replacementReplay);
		}
		if (!SetImageGraphDynamicInput(staged, nodeId, replacement, error)) return false;
		const auto node = std::find_if(staged.Nodes.begin(), staged.Nodes.end(), [&](const auto &item) {
			return item.Id == nodeId;
		});
		const auto *entry = FindCatalogueEntry(node->Type);
		size_t group = 0;
		const auto *input = entry ? FindDynamicTemplate(*entry, replacement.Id, group) : nullptr;
		if (input && input->SourceIndex >= 0 && replacement.Default) {
			GroupRefreshEvent event;
			event.NodeId = std::string(nodeId);
			event.EditedPort = replacement.Id;
			event.LocalValue = &*replacement.Default;
			event.LocalAnimated =
				ImageGraphGroupHost::Mode(staged, *node, replacement.Id) == GroupSubtypeAnimator::Animated;
			event.At = request;
			if (!stagedHost.Edit(staged, history, revision, event, error, false)) return false;
		}
		if (!beforeCommit(staged, stagedHost)) return false;
		const auto stagedBytes = DocumentRetainedPayloadBytes(staged);
		if (!stagedBytes || *stagedBytes > maximum - stagedHost.BorrowedBytes ||
			stagedHost.Replay.RetainedBytes() > maximum - stagedHost.BorrowedBytes - *stagedBytes)
			return refuse();
		if (document == staged) return false;
		if (!history.TryRecord(document, staged)) return refuse();
		stagedHost.BorrowedBytes = 0;
		document = std::move(staged);
		host = std::move(stagedHost);
		return true;
	} catch (const std::bad_alloc &) {
		error = {
			engine::imagegraph::Status::LimitExceeded,
			std::string(nodeId),
			replacement.Id,
			"source input transaction allocation failed"
		};
		return false;
	}
	inline bool ApplyImageGraphSourceDynamicInput(
		engine::imagegraph::Document &document,
		ImageGraphHistory &history,
		ImageGraphGroupHost &host,
		uint64_t revision,
		std::string_view nodeId,
		const engine::imagegraph::DynamicInput &replacement,
		engine::imagegraph::EvaluationRequest request,
		engine::imagegraph::Diagnostic &error
	) {
		return ApplyImageGraphSourceDynamicInput(
			document, history, host, revision, nodeId, replacement, request, error, [](auto &, auto &) {
				return true;
			}
		);
	}
}
