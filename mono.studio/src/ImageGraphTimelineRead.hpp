#pragma once

#include "ImageGraphGroupHost.hpp"

#include <engine/core/Profiling.hpp>

namespace studio {
	// grug keep one derived read snapshot; host replay and authored data still own the facts.
	struct ImageGraphTimelineRead {
		std::optional<engine::imagegraph::Document> Snapshot;
		uint64_t AuthoringRevision = 0, ReplayRevision = 0, DisplayRevision = 0;

		std::optional<uint64_t> RetainedBytes() const {
			return Snapshot ? engine::imagegraph::DocumentRetainedPayloadBytes(*Snapshot)
							: std::optional<uint64_t>{0};
		}
		bool Matches(const ImageGraphGroupHost &host, uint64_t revision) const {
			return Snapshot && AuthoringRevision == revision && host.Revision == revision &&
				   host.Replay.InstancesBound() && host.Replay.AuthoringRevision() == revision &&
				   ReplayRevision == host.Replay.ObservationRevision();
		}
		// grug project only a prepared host. refusal keeps old storage, but callers must not read stale
		// stamps.
		bool Refresh(
			const engine::imagegraph::Document &authored,
			const ImageGraphGroupHost &host,
			uint64_t revision,
			engine::imagegraph::Diagnostic &error,
			uint64_t maximumBytes = engine::imagegraph::Limits::MaximumEvaluationBytes
		) try {
			using namespace engine::imagegraph;
			ENGINE_PROFILE("studio.imagegraph.timeline_read");
			const auto oldBytes = RetainedBytes();
			if (!maximumBytes || maximumBytes > Limits::MaximumEvaluationBytes || !oldBytes ||
				!host.Budget(error, {&authored}, {&host.Replay}, oldBytes.value_or(0), maximumBytes)) {
				error = {Status::LimitExceeded, {}, {}, "timeline snapshot overlap exceeds payload bounds"};
				return false;
			}
			if (Matches(host, revision)) return true;
			Document candidate;
			if (!host.ProjectForSave(authored, revision, candidate, error, maximumBytes - *oldBytes))
				return false;
			const auto candidateBytes = DocumentRetainedPayloadBytes(candidate);
			if (!candidateBytes ||
				!host.Budget(error, {&authored, &candidate}, {&host.Replay}, *oldBytes, maximumBytes))
				return false;
			Snapshot = std::move(candidate);
			AuthoringRevision = revision;
			ReplayRevision = host.Replay.ObservationRevision();
			DisplayRevision = DisplayRevision == UINT64_MAX ? 1 : DisplayRevision + 1;
			return true;
		} catch (const std::bad_alloc &) {
			error = {
				engine::imagegraph::Status::LimitExceeded, {}, {}, "timeline snapshot allocation failed"
			};
			return false;
		}
	};
	// grug prepare declarations on a staged host when preview has not done it yet.
	// snapshot and live replay publish together; no getter event or authored edit happens here.
	inline bool PrepareImageGraphTimelineRead(
		const engine::imagegraph::Document &authored,
		ImageGraphGroupHost &host,
		uint64_t revision,
		engine::imagegraph::EvaluationRequest request,
		ImageGraphTimelineRead &read,
		engine::imagegraph::Diagnostic &error,
		uint64_t maximumBytes = engine::imagegraph::Limits::MaximumEvaluationBytes
	) try {
		using namespace engine::imagegraph;
		if (host.Revision == revision && host.Replay.InstancesBound() &&
			host.Replay.AuthoringRevision() == revision)
			return read.Refresh(authored, host, revision, error, maximumBytes);
		const auto oldReadBytes = read.RetainedBytes();
		if (!maximumBytes || maximumBytes > Limits::MaximumEvaluationBytes || !oldReadBytes ||
			*oldReadBytes >= maximumBytes) {
			error = {Status::LimitExceeded, {}, {}, "timeline preparation has no snapshot allowance"};
			return false;
		}
		ImageGraphGroupHost prepared;
		const auto allowance = host.Budget(error, {}, {&host.Replay}, 0, maximumBytes);
		if (!allowance) return false;
		prepared.BorrowedBytes = Limits::MaximumEvaluationBytes - allowance;
		prepared.Revision = revision == 0 ? 1 : 0;
		const auto rebindAllowance = host.Budget(error, {}, {}, *oldReadBytes, maximumBytes);
		if (!rebindAllowance ||
			RebindGroupReplay(authored, host.Replay, revision, prepared.Replay, error, rebindAllowance) !=
				Status::Ok)
			return false;
		prepared.BorrowedBytes += *oldReadBytes;
		const auto planAllowance = prepared.Budget(error, {&authored}, {&prepared.Replay}) / 2;
		if (!planAllowance) return false;
		Plan plan;
		if (Compile(authored, plan, error, planAllowance) != Status::Ok) return false;
		prepared.BorrowedBytes += planAllowance;
		if (!prepared.Prepare(authored, plan, revision, request, error)) return false;
		plan = {};
		prepared.BorrowedBytes -= planAllowance + *oldReadBytes;
		if (!read.Refresh(authored, prepared, revision, error)) return false;
		host.Replay = std::move(prepared.Replay);
		host.Revision = revision;
		read.ReplayRevision = host.Replay.ObservationRevision();
		return true;
	} catch (const std::bad_alloc &) {
		error = {engine::imagegraph::Status::LimitExceeded, {}, {}, "timeline preparation allocation failed"};
		return false;
	}

}
