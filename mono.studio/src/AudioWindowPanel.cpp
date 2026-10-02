#include "AudioWindowPanel.hpp"

#include "StudioWavTimelineDraw.hpp"

#include <engine/core/Profiling.hpp>

#include <algorithm>
#include <new>
#include <stdexcept>
namespace studio {
	bool AudioWindowPanel::Update(
		const engine::imagegraph::Document &doc,
		std::string_view nodeId,
		const engine::imagegraph::EvaluationRequest &request,
		uint64_t documentRevision,
		uint64_t inputRevision,
		engine::imagegraph::Diagnostic &diagnostic,
		uint64_t byteBudget
	) {
		using namespace engine::imagegraph;
		ENGINE_PROFILE("image composer audio window observation");
		const auto frame = GetFrameTime(request);
		if (Attempted && Target == nodeId && AttemptDocument == documentRevision &&
			AttemptInput == inputRevision && AttemptFrame == frame && AttemptBudget == byteBudget &&
			AttemptMaximumImageDimension == request.MaximumImageDimension) {
			diagnostic = LastError;
			return Valid;
		}
		Attempted = false;
		Valid = false;
		try {
			const uint64_t cap = std::min(byteBudget, Limits::MaximumEvaluationBytes);
			const uint64_t old = Observation.Points.capacity() * sizeof(Vector2) + Target.capacity() + 1;
			const uint64_t targetBytes = std::max(nodeId.size(), std::string{}.capacity()) + 1;
			if (old > cap || (Target != nodeId && targetBytes > cap - old)) {
				diagnostic =
					LastError = {Status::LimitExceeded, {}, {}, "Audio Window target identity exceeds cap"};
				return false;
			}
			if (Target != nodeId) {
				std::string next;
				next.reserve(nodeId.size());
				if (next.capacity() + 1 > cap - old) {
					diagnostic = LastError = {
						Status::LimitExceeded, {}, {}, "Audio Window actual target capacity exceeds cap"
					};
					return false;
				}
				next = nodeId;
				Target = std::move(next);
			}
			Attempted = true;
			AttemptDocument = documentRevision;
			AttemptInput = inputRevision;
			AttemptFrame = frame;
			AttemptBudget = byteBudget;
			AttemptMaximumImageDimension = request.MaximumImageDimension;
			if (!HavePlan || PlanRevision != documentRevision) {
				if (Compile(doc, Compiled, LastError) != Status::Ok) {
					diagnostic = LastError;
					return false;
				}
				HavePlan = true;
				PlanRevision = documentRevision;
			}
			const uint64_t metadata = Target.capacity() + 1;
			if (metadata > cap) {
				diagnostic = LastError = {Status::LimitExceeded, {}, {}, "Audio Window identity exceeds cap"};
				return false;
			}
			if (ResolveAudioWindowPresentation(
					doc, Compiled, nodeId, request, cap - metadata, Observation, LastError
				) != Status::Ok) {
				diagnostic = LastError;
				return false;
			}
			Valid = true;
			diagnostic = LastError = {};
			return true;
		} catch (const std::bad_alloc &) {
			LastError = {
				engine::imagegraph::Status::LimitExceeded, {}, {}, "Audio Window panel allocation failed"
			};
		} catch (const std::length_error &) {
			LastError = {
				engine::imagegraph::Status::LimitExceeded,
				{},
				{},
				"Audio Window panel allocation length exceeded"
			};
		}
		diagnostic = LastError;
		return false;
	}
	void AudioWindowPanel::Draw(float height) const {
		if (!Valid) return;
		ENGINE_PROFILE("image composer audio window draw");
		const auto origin = ImGui::GetCursorScreenPos();
		const float width = std::max(1.f, ImGui::GetContentRegionAvail().x);
		ImGui::InvisibleButton("##audio-window-observation", {width, height});
		auto *draw = ImGui::GetWindowDrawList();
		const auto waveform = [&](ImU32 colour) {
			for (size_t i = 1; i < Observation.Points.size(); ++i) {
				auto a = Observation.Points[i - 1], b = Observation.Points[i];
				if (!ClipWavSegment(a, b, 1)) continue;
				draw->AddLine(
					{origin.x + float(a.X) * width, origin.y + height / 2 + float(a.Y) * height},
					{origin.x + float(b.X) * width, origin.y + height / 2 + float(b.Y) * height},
					colour
				);
			}
		};
		draw->PushClipRect(origin, {origin.x + width, origin.y + height}, true);
		waveform(0x80ffffff);
		const float start = origin.x + float(std::clamp(Observation.Start, 0., 1.)) * width;
		const float end = origin.x + float(std::clamp(Observation.End, 0., 1.)) * width;
		if (end > start) {
			draw->PushClipRect({start, origin.y}, {end, origin.y + height}, true);
			waveform(ImGui::GetColorU32(ImGuiCol_CheckMark));
			draw->PopClipRect();
		}
		const float cursor = origin.x + float(std::clamp(Observation.Cursor, 0., 1.)) * width;
		draw->AddLine(
			{cursor, origin.y + height / 2 - 16},
			{cursor, origin.y + height / 2 + 16},
			ImGui::GetColorU32(ImGuiCol_CheckMark)
		);
		draw->PopClipRect();
	}
}
