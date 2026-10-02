#include "WavTimelinePanel.hpp"

#include "StudioWavTimelineDraw.hpp"

#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>

#include <algorithm>
#include <new>
#include <stdexcept>
namespace studio {
	bool WavTimelinePanel::Update(
		const engine::imagegraph::Document &doc,
		std::string_view nodeId,
		const engine::imagegraph::EvaluationRequest &request,
		double fps,
		uint64_t documentRevision,
		uint64_t inputRevision,
		engine::imagegraph::Diagnostic &diagnostic,
		uint64_t byteBudget
	) {
		using namespace engine::imagegraph;
		ENGINE_PROFILE("image composer wav timeline observation");
		const auto frame = GetFrameTime(request);
		if (Attempted && Target == nodeId && AttemptDocument == documentRevision &&
			AttemptInput == inputRevision && AttemptFps == fps && AttemptFrame == frame &&
			AttemptBudget == byteBudget) {
			diagnostic = LastError;
			return Valid;
		}
		Valid = false;
		try {
			const uint64_t cap = std::min(byteBudget, Limits::MaximumEvaluationBytes);
			const uint64_t retainedMetadata = Geometry.Points.capacity() * sizeof(Vector2) +
											  GeometryPath.capacity() + 1 + Target.capacity() + 1;
			const uint64_t nextTargetBytes = std::max(nodeId.size(), std::string{}.capacity()) + 1;
			if (retainedMetadata > cap || (Target != nodeId && nextTargetBytes > cap - retainedMetadata)) {
				LastError = {Status::LimitExceeded, {}, {}, "waveform target identity exceeds cap"};
				diagnostic = LastError;
				return false;
			}
			if (Target != nodeId) {
				std::string nextTarget;
				nextTarget.reserve(nodeId.size());
				if (nextTarget.capacity() + 1 > cap - retainedMetadata) {
					LastError = {
						Status::LimitExceeded, {}, {}, "waveform target identity capacity exceeds cap"
					};
					diagnostic = LastError;
					return false;
				}
				nextTarget = nodeId;
				Target = std::move(nextTarget);
			}
			Attempted = true;
			AttemptDocument = documentRevision;
			AttemptInput = inputRevision;
			AttemptFps = fps;
			AttemptFrame = frame;
			AttemptBudget = byteBudget;
			const auto fail = [&](Status code, const char *message) {
				LastError = {code, std::string(nodeId), {}, message};
				diagnostic = LastError;
				return false;
			};
			const auto node = std::find_if(doc.Nodes.begin(), doc.Nodes.end(), [&](const Node &value) {
				return value.Id == nodeId;
			});
			if (node == doc.Nodes.end() || node->Type != "pc.wav_file_read")
				return fail(Status::InvalidValue, "waveform target is not WAV File In");
			if (!HavePlan || PlanRevision != documentRevision) {
				if (Compile(doc, Compiled, LastError) != Status::Ok) {
					diagnostic = LastError;
					return false;
				}
				PlanRevision = documentRevision;
				HavePlan = true;
			}
			const uint64_t oldBytes = Geometry.Points.capacity() * sizeof(Vector2) + GeometryPath.capacity() +
									  1 + Target.capacity() + 1;
			if (oldBytes > cap) return fail(Status::LimitExceeded, "retained waveform exceeds cap");
			EvaluationSnapshot snapshot;
			if (EvaluateNodeInputs(doc, Compiled, nodeId, request, snapshot, LastError, cap - oldBytes) !=
				Status::Ok) {
				diagnostic = LastError;
				return false;
			}
			const std::string *path = nullptr;
			for (const auto &input : snapshot.Values())
				if (input.Port == "path") path = std::get_if<std::string>(&input.Data);
			if (!path) return fail(Status::UnsupportedExecution, "waveform path is not scalar text");
			if (!HaveGeometry || GeometryPath != *path || GeometryInputRevision != inputRevision ||
				GeometryFps != fps) {
				const uint64_t nextPathBytes = std::max(path->size(), std::string{}.capacity()) + 1;
				if (snapshot.RetainedBytes() > cap - oldBytes ||
					nextPathBytes > cap - oldBytes - snapshot.RetainedBytes())
					return fail(Status::LimitExceeded, "waveform identity exceeds cap");
				std::string nextPath;
				nextPath.reserve(path->size());
				const uint64_t retainedPath = nextPath.capacity() + 1;
				if (retainedPath > cap - oldBytes - snapshot.RetainedBytes())
					return fail(Status::LimitExceeded, "waveform identity capacity exceeds cap");
				if (request.AudioClips.size() > Limits::MaximumNodes)
					return fail(Status::LimitExceeded, "WAV source count exceeds cap");
				const AudioBit *clip = nullptr;
				for (const auto &asset : request.AudioClips)
					if (asset.SourceId == *path) {
						if (clip) return fail(Status::DuplicateId, "WAV source identity is duplicated");
						clip = &asset.Data;
					}
				if (!clip) return fail(Status::InvalidValue, "WAV source asset is missing");
				if (BuildWavTimelinePresentation(
						*clip,
						frame,
						fps,
						cap - GeometryPath.capacity() - 1 - Target.capacity() - 1 - retainedPath -
							snapshot.RetainedBytes(),
						Geometry,
						LastError
					) != Status::Ok) {
					diagnostic = LastError;
					return false;
				}
				nextPath = *path;
				GeometryPath = std::move(nextPath);
				GeometryInputRevision = inputRevision;
				GeometryFps = fps;
				HaveGeometry = true;
			} else
				engine::core::Metrics::Count("studio.wav_timeline.geometry_cache_hits", 1);
			Geometry.Progress =
				Geometry.Duration == 0
					? 0
					: double(std::clamp(FrameTimeToReal(frame) / fps / Geometry.Duration, 0.L, 1.L));
			Valid = true;
			LastError = {};
			diagnostic = {};
			return true;
		} catch (const std::bad_alloc &) {
			LastError = {Status::LimitExceeded, {}, {}, "waveform panel allocation failed"};
		} catch (const std::length_error &) {
			LastError = {Status::LimitExceeded, {}, {}, "waveform panel allocation length exceeded"};
		}
		diagnostic = LastError;
		return false;
	}
	void WavTimelinePanel::Draw(float height) const {
		if (!Valid) return;
		ENGINE_PROFILE("image composer wav timeline draw");
		const float width = std::max(1.f, ImGui::GetContentRegionAvail().x);
		const double frames = Geometry.Duration * GeometryFps;
		const float pixelsPerFrame = frames > 0 ? float(width / frames) : width;
		DrawWavTimelineObservation(Geometry, pixelsPerFrame, width, height);
	}
}
