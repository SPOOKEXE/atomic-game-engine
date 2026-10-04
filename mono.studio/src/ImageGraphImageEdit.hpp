#pragma once

#include <engine/imagegraph/Surface.hpp>
#include <engine/imagegraphexport/GraphImageCache.hpp>

#include <algorithm>
#include <new>
#include <studio/ImageGraph.hpp>

namespace studio::detail {
	inline bool SourceImageType(std::string_view type) {
		return type == "pc.image" || type == "pc.image_sequence" || type == "pc.image_animated";
	}
	// The caller binds this immutable snapshot to the selected node and exact request.
	// Copying receipt metadata never evaluates upstream producers again.
	inline bool PrepareImageGraphImageControls(
		const engine::imagegraph::Document &document,
		const engine::imagegraph::EvaluationRequest &request,
		std::string_view nodeId,
		const engine::imagegraph::EvaluationSnapshot &snapshot,
		engine::imagegraph::HostNodeCapture &capture,
		engine::imagegraph::Diagnostic &diagnostic,
		uint64_t maximumBytes = engine::imagegraph::Limits::MaximumEvaluationBytes
	) try {
		using namespace engine::imagegraph;
		const auto node = std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const auto &item) {
			return item.Id == nodeId;
		});
		const auto fail = [&](const char *message) {
			diagnostic = {Status::LimitExceeded, std::string(nodeId), {}, message};
			return false;
		};
		if (node == document.Nodes.end() || !SourceImageType(node->Type) || !snapshot.ImageArrays().empty()) {
			diagnostic = {
				Status::UnsupportedExecution,
				std::string(nodeId),
				{},
				"Cache actions require normalized source image controls"
			};
			return false;
		}
		uint64_t held = snapshot.RetainedBytes();
		const auto spend = [&](uint64_t bytes) {
			if (held > maximumBytes || bytes > maximumBytes - held) return false;
			held += bytes;
			return true;
		};
		if (!spend(snapshot.Values().size() * sizeof(AuthoredValue)) ||
			!spend(snapshot.Images().size() * sizeof(HostResolvedImage)))
			return fail("Prepared image control backing exceeds operation bounds");
		for (const auto &value : snapshot.Values()) {
			const auto bytes = ValueClonePayloadBytes(value.Data);
			if (!bytes || !spend(*bytes) || !spend(std::max(value.Port.size(), std::string{}.capacity()) + 1))
				return fail("Prepared image control payload exceeds operation bounds");
		}
		std::vector<AuthoredValue> values;
		std::vector<HostResolvedImage> images;
		values.reserve(snapshot.Values().size());
		images.reserve(snapshot.Images().size());
		if (!spend((values.capacity() - snapshot.Values().size()) * sizeof(AuthoredValue)) ||
			!spend((images.capacity() - snapshot.Images().size()) * sizeof(HostResolvedImage)))
			return fail("Prepared image control reserve exceeds operation bounds");
		for (const auto &value : snapshot.Values())
			values.push_back({value.Port, value.Data});
		for (const auto &image : snapshot.Images())
			images.push_back({image.Port, &image.Data});
		uint64_t retained = 0;
		return PrepareResolvedHostCapture(
				   {*node,
					request,
					values,
					images,
					maximumBytes - held,
					document.Timeline ? &*document.Timeline : nullptr,
					snapshot.InheritedSurfaceFormat(),
					snapshot.InheritedInterpolation()},
				   maximumBytes - held,
				   capture,
				   retained,
				   diagnostic
			   ) == Status::Ok;
	} catch (const std::bad_alloc &) {
		diagnostic = {
			engine::imagegraph::Status::LimitExceeded, {}, {}, "Prepared image control allocation failed"
		};
		return false;
	}
	// The prepared producer attests EncodedCache came from its immutable live sprite ledger.
	// History is admitted before either document or replay owner is replaced.
	inline bool ApplyPreparedImageGraphImage(
		engine::imagegraph::Document &document,
		ImageGraphHistory &history,
		engine::imagegraph::GroupReplayState &replay,
		const engine::imagegraphio::SourceImageFrameObservation &prepared,
		const engine::imagegraphio::SourceImageEditOptions &options,
		engine::imagegraph::Diagnostic &diagnostic,
		bool &changed,
		uint64_t maximumBytes = engine::imagegraph::Limits::MaximumEvaluationBytes
	) {
		using namespace engine::imagegraph;
		changed = false;
		Document candidate;
		GroupReplayState next;
		bool edited = false;
		if (engine::imagegraphio::ApplySourceImageEdit(
				document, prepared, replay, options, candidate, next, edited, diagnostic, maximumBytes
			) != Status::Ok)
			return false;
		if (!edited) return true;
		if (!history.TryRecord(document, candidate)) {
			diagnostic = {
				Status::LimitExceeded,
				prepared.Controls.Authored.Id,
				{},
				"Image cache undo transaction exceeds history budget"
			};
			return false;
		}
		document = std::move(candidate);
		replay = std::move(next);
		changed = true;
		return true;
	}
}
