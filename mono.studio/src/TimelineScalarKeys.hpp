#pragma once

#include "ImageGraphCapturedKeyEdit.hpp"
#include "ImageGraphKeyPinProjection.hpp"
#include "TimelineKeyEditor.hpp"
#include "TimelineScalarKeyControls.hpp"

#include <functional>

namespace studio::detail {
	// grug borrow effective storage; return after publication before touching old rows.
	template <class Select, class Apply>
	bool DrawTimelineScalarKeys(
		const engine::imagegraph::Document &document,
		TimelineKeyEditor &editor,
		engine::imagegraph::Diagnostic &diagnostic,
		const Select &select,
		const Apply &apply,
		const std::function<bool(engine::imagegraph::Curve &)> &drawCurve = {}
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
					const auto changePinned = [&](const auto &change, bool remove, uint64_t controlHeld) {
						Document captured;
						const auto wrapperBytes = DocumentRetainedPayloadBytes(captured);
						if (!wrapperBytes || controlHeld > *budget || viewBytes > *budget - controlHeld ||
							*wrapperBytes > *budget - controlHeld - viewBytes ||
							CaptureSourceKeyframes(
								document,
								{&identity, 1},
								captured.Keyframes,
								diagnostic,
								*budget - controlHeld - viewBytes - *wrapperBytes
							) != Status::Ok)
							return false;
						const auto capturedBytes = DocumentRetainedPayloadBytes(captured);
						const auto logicalPinBytes = KeyframePayloadBytes(captured.Keyframes.front());
						if (!capturedBytes || !logicalPinBytes ||
							*capturedBytes > *budget - controlHeld - viewBytes) {
							diagnostic = {
								Status::LimitExceeded, {}, {}, "scalar table pins exceed edit payload budget"
							};
							return false;
						}
						const uint64_t borrowedBytes = viewBytes + *capturedBytes + controlHeld;
						const bool changed = apply(
							[&](Document &candidate,
								uint64_t availableBytes = Limits::MaximumEvaluationBytes) {
								// grug host already charged pins; projection charges logical pins again
								// inside its ledger.
								if (availableBytes > Limits::MaximumEvaluationBytes ||
									*logicalPinBytes > Limits::MaximumEvaluationBytes - availableBytes)
									return false;
								return WithImageGraphProjectedKeyPins(
									document,
									candidate,
									captured.Keyframes,
									availableBytes + *logicalPinBytes,
									[&](std::span<const Keyframe> pins, uint64_t remaining) {
										if (!remove)
											return EditCapturedImageGraphKeys(
												candidate,
												pins,
												[](const auto &, size_t) { return true; },
												change,
												diagnostic,
												4096 + controlHeld,
												0,
												remaining,
												{&axis, 1}
											);
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
						return changed;
					};
					const auto documentBytes = DocumentRetainedPayloadBytes(document);
					if (!documentBytes || viewBytes > *budget || *documentBytes > *budget - viewBytes)
						return false;
					ImGui::PushOverrideID(rowId);
					const bool metadataChanged = DrawTimelineScalarKeyControls(
						key,
						*budget - viewBytes - *documentBytes,
						diagnostic,
						[&](const auto &change, uint64_t held) { return changePinned(change, false, held); },
						[&](Curve &curve) { return drawCurve && drawCurve(curve); }
					);
					ImGui::TableSetColumnIndex(7);
					const bool remove = !metadataChanged && ImGui::SmallButton("Delete");
					ImGui::PopID();
					if (metadataChanged) return true;
					if (remove && changePinned([](auto &, size_t) {}, true, 0)) return true;
				}
		}
		return false;
	}
}
