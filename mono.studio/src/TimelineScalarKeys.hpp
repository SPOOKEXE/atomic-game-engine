#pragma once

#include "ImageGraphKeyPinProjection.hpp"
#include "TimelineKeyEditor.hpp"

namespace studio::detail {
	// grug borrow effective storage; return after publication before touching old rows.
	template <class Select, class Apply>
	bool DrawTimelineScalarKeys(
		const engine::imagegraph::Document &document,
		TimelineKeyEditor &editor,
		engine::imagegraph::Diagnostic &diagnostic,
		const Select &select,
		const Apply &apply
	) {
		using namespace engine::imagegraph;
		const auto budget = editor.Remaining(true, true);
		if (!budget) {
			diagnostic = {Status::LimitExceeded, {}, {}, "scalar table exceeds the timeline payload budget"};
			return false;
		}
		std::vector<SourceAxisObservation> views;
		Diagnostic observationError;
		if (ObserveSourceKeyframeAxes(document, views, observationError, *budget) != Status::Ok) {
			diagnostic = std::move(observationError);
			return false;
		}
		const uint64_t viewBytes = views.capacity() * sizeof(SourceAxisObservation);
		for (const auto &view : views) {
			for (int8_t axis = 0; axis < 2; ++axis)
				for (const auto &key : view.Storage->Axes[size_t(axis)].Keys) {
					ImGui::TableNextRow();
					ImGui::TableSetColumnIndex(0);
					const SourceKeyframeIdentity identity{view.NodeId, view.Port, GetFrameTime(key), axis};
					if (editor.DrawRow(view.NodeId, view.Port, identity.Time, axis))
						select(
							ImageGraphKeyframeIdentity{
								std::string(view.NodeId), std::string(view.Port), identity.Time, axis
							}
						);
					const ImGuiID rowId = ImGui::GetItemID();
					ImGui::TableSetColumnIndex(1);
					ImGui::Text("%.20Lg", FrameTimeToReal(GetFrameTime(key)));
					ImGui::TableSetColumnIndex(2);
					ImGui::TextUnformatted(key.Interpolation.c_str());
					if (key.Ease) {
						ImGui::TableSetColumnIndex(3);
						ImGui::TextUnformatted(key.Ease->InType.c_str());
						ImGui::TableSetColumnIndex(4);
						ImGui::TextUnformatted(key.Ease->OutType.c_str());
					}
					ImGui::TableSetColumnIndex(5);
					ImGui::TextUnformatted(key.SourceDriver || key.SineDriver ? "Driven" : "None");
					ImGui::TableSetColumnIndex(6);
					ImGui::TextUnformatted(key.Kind == KeyframeKind::Adder ? "Adder" : "Normal");
					ImGui::TableSetColumnIndex(7);
					ImGui::PushOverrideID(rowId);
					const bool remove = ImGui::SmallButton("Delete");
					ImGui::PopID();
					if (!remove) continue;
					Document captured;
					const auto wrapperBytes = DocumentRetainedPayloadBytes(captured);
					if (!wrapperBytes || viewBytes > *budget || *wrapperBytes > *budget - viewBytes ||
						CaptureSourceKeyframes(
							document,
							{&identity, 1},
							captured.Keyframes,
							diagnostic,
							*budget - viewBytes - *wrapperBytes
						) != Status::Ok)
						continue;
					const auto capturedBytes = DocumentRetainedPayloadBytes(captured);
					const auto logicalPinBytes = KeyframePayloadBytes(captured.Keyframes.front());
					if (!capturedBytes || !logicalPinBytes || *capturedBytes > *budget - viewBytes) {
						diagnostic = {
							Status::LimitExceeded, {}, {}, "scalar table pins exceed edit payload budget"
						};
						return false;
					}
					const uint64_t borrowedBytes = viewBytes + *capturedBytes;
					const bool changed = apply(
						[&](Document &candidate, uint64_t availableBytes = Limits::MaximumEvaluationBytes) {
							// grug host already charged pins; projection charges logical pins again inside
							// its ledger.
							if (availableBytes > Limits::MaximumEvaluationBytes ||
								*logicalPinBytes > Limits::MaximumEvaluationBytes - availableBytes)
								return false;
							return WithImageGraphProjectedKeyPins(
								document,
								candidate,
								captured.Keyframes,
								availableBytes + *logicalPinBytes,
								[&](std::span<const Keyframe> pins, uint64_t remaining) {
									const SourceKeyframeEdit edit{&pins.front(), nullptr, false, axis};
									return ApplySourceKeyframeEdits(
											   candidate, {&edit, 1}, candidate, diagnostic, remaining
										   ) == Status::Ok;
								},
								diagnostic,
								{&axis, 1}
							);
						},
						borrowedBytes
					);
					if (changed) return true;
				}
		}
		return false;
	}
}
