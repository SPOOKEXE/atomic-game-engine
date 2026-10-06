#pragma once

#include "ImageGraphGroupHost.hpp"
#include "ImageGraphKeyPinProjection.hpp"

#include <engine/core/Profiling.hpp>

namespace studio {
	inline bool NeedsImageGraphSourceKeyCapture(const engine::imagegraph::Document &document) {
		if (document.SourceAnimators) return true;
		for (const auto &node : document.Nodes) {
			if (node.InstanceBase.empty()) continue;
			const auto *entry = engine::imagegraph::FindCatalogueEntry(node.Type);
			if (entry && engine::imagegraph::HasNativeExecutor(node.Type)) return true;
		}
		for (const auto &group : document.Groups)
			for (const auto &port : group.Ports)
				if (!port.ControlNodeId.empty()) return true;
		return false;
	}

	// grug capture constructor writers before the edit so undo survives a cleared host.
	// callback receives the remaining budget and publishes no external state.
	template <class Edit>
	bool ApplyImageGraphSourceKeyEdit(
		engine::imagegraph::Document &document,
		ImageGraphHistory &history,
		ImageGraphGroupHost &host,
		uint64_t revision,
		engine::imagegraph::EvaluationRequest request,
		const Edit &edit,
		engine::imagegraph::Diagnostic &error,
		uint64_t maximumBytes = engine::imagegraph::Limits::MaximumEvaluationBytes,
		bool *unchanged = nullptr,
		const std::function<
			bool(engine::imagegraph::Document &, const engine::imagegraph::GroupReplayState *, uint64_t)>
			&finalize = {}
	) try {
		using namespace engine::imagegraph;
		ENGINE_PROFILE("studio.imagegraph.source_key_edit");
		if (unchanged) *unchanged = false;
		if (!maximumBytes || maximumBytes > Limits::MaximumEvaluationBytes) {
			error = {Status::LimitExceeded, {}, {}, "source key transaction budget is outside bounds"};
			return false;
		}
		Document before;
		GroupReplayState declarations;
		const bool capture = NeedsImageGraphSourceKeyCapture(document);
		if (capture) {
			ImageGraphGroupHost prepared;
			const auto allowance = host.Budget(error, {}, {&host.Replay}, 0, maximumBytes);
			if (!allowance) return false;
			prepared.BorrowedBytes = Limits::MaximumEvaluationBytes - allowance;
			prepared.Revision = host.Revision;
			if (RebindGroupReplay(
					document,
					host.Replay,
					revision,
					prepared.Replay,
					error,
					host.Budget(error, {}, {}, 0, maximumBytes)
				) != Status::Ok)
				return false;
			const auto planAllowance = prepared.Budget(error, {&document}, {&prepared.Replay}) / 2;
			if (!planAllowance) return false;
			Plan plan;
			if (Compile(document, plan, error, planAllowance) != Status::Ok) return false;
			prepared.BorrowedBytes += planAllowance;
			if (!prepared.Prepare(document, plan, revision, request, error)) return false;
			plan = {};
			prepared.BorrowedBytes -= planAllowance;
			if (!prepared.ProjectForSave(document, revision, before, error)) return false;
			const auto rebindAllowance = prepared.Budget(error, {&document});
			if (!rebindAllowance ||
				RebindProjectedGroupReplay(
					before, prepared.Replay, revision, declarations, error, rebindAllowance
				) != Status::Ok)
				return false;
		} else {
			const auto bytes = DocumentRetainedPayloadBytes(document);
			if (!bytes || !host.Budget(error, {&document}, {&host.Replay}, *bytes, maximumBytes))
				return false;
			before = document;
		}
		const auto beforeBytes = DocumentRetainedPayloadBytes(before);
		if (!beforeBytes ||
			!host.Budget(
				error, {&document, &before}, {&host.Replay, &declarations}, *beforeBytes, maximumBytes
			))
			return false;
		Document changed = before;
		const auto editAllowance =
			host.Budget(error, {&document, &before}, {&host.Replay, &declarations}, 0, maximumBytes);
		if (!editAllowance || !edit(changed, editAllowance)) return false;
		if (changed == before) {
			if (unchanged) *unchanged = true;
			return false;
		}
		const uint64_t nextRevision = revision == UINT64_MAX ? 1 : revision + 1;
		GroupReplayState rebound;
		if (capture) {
			const auto restoreAllowance =
				host.Budget(error, {&document, &before}, {&host.Replay}, 0, maximumBytes);
			if (!restoreAllowance) return false;
			const auto status =
				changed.SourceAnimators
					? RestoreSourceAnimatorBindings(
						  changed, declarations, nextRevision, rebound, error, restoreAllowance
					  )
					: RebindGroupReplay(
						  changed, declarations, nextRevision, rebound, error, restoreAllowance
					  );
			if (status != Status::Ok) return false;
		}
		const auto finalizeAllowance = host.Budget(
			error, {&document, &before}, {&host.Replay, &declarations, &rebound}, 0, maximumBytes
		);
		if (!finalizeAllowance ||
			(finalize && !finalize(changed, capture ? &rebound : nullptr, finalizeAllowance)))
			return false;
		if (!host.Budget(
				error,
				{&document, &before, &changed},
				{&host.Replay, &declarations, &rebound},
				0,
				maximumBytes
			))
			return false;
		if (!history.TryRecord(before, changed)) {
			error = {Status::LimitExceeded, {}, {}, "source key edit exceeds undo history budget"};
			return false;
		}
		document = std::move(changed);
		if (capture) {
			host.Replay = std::move(rebound);
			host.Revision = nextRevision;
		}
		error = {};
		return true;
	} catch (const std::bad_alloc &) {
		error = {
			engine::imagegraph::Status::LimitExceeded, {}, {}, "source key transaction allocation failed"
		};
		return false;
	}

	// grug projection can sort or replace rows. keep the visible full pin, never its old index.
	template <class Edit>
	bool EditImageGraphPinnedKey(
		engine::imagegraph::Document &document,
		const engine::imagegraph::Document &authored,
		const engine::imagegraph::Keyframe &original,
		uint64_t availableBytes,
		const Edit &edit,
		engine::imagegraph::Diagnostic &error
	) {
		return WithImageGraphProjectedKeyPins(
			authored,
			document,
			{&original, 1},
			availableBytes,
			[&](std::span<const engine::imagegraph::Keyframe> pins, uint64_t remaining) {
				const auto found =
					std::find_if(document.Keyframes.begin(), document.Keyframes.end(), [&](const auto &key) {
						return key.NodeId == pins.front().NodeId && key.Port == pins.front().Port &&
							   engine::imagegraph::GetFrameTime(key) ==
								   engine::imagegraph::GetFrameTime(pins.front()) &&
							   key == pins.front();
					});
				return edit(document, size_t(found - document.Keyframes.begin()), remaining);
			},
			error
		);
	}
}
