#include "SourcePathShiftMemo.hpp"

#include <engine/imagegraph/HostCapture.hpp>

#include <algorithm>

namespace engine::imagegraph {
	Status PrepareHostCapture(
		const Document &document,
		const Plan &plan,
		std::string_view nodeId,
		const EvaluationRequest &request,
		HostNodeCapture &capture,
		Diagnostic &diagnostic
	) {
		const auto node = std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const Node &entry) {
			return entry.Id == nodeId;
		});
		if (node == document.Nodes.end()) {
			diagnostic = {Status::UnknownNode, std::string(nodeId), {}, "host capture node is missing"};
			return diagnostic.Code;
		}
		EvaluationSnapshot snapshot;
		const Status status = EvaluateNodeInputs(document, plan, nodeId, request, snapshot, diagnostic);
		if (status != Status::Ok) return status;
		HostNodeCapture prepared;
		prepared.Authored = *node;
		detail::StripSourcePathShiftIdentities(prepared.Authored);
		prepared.Tick = request.Tick;
		prepared.Subframe = request.Subframe;
		prepared.NegativeFrame = request.NegativeFrame;
		prepared.Inputs.reserve(snapshot.Values().size());
		for (const auto &value : snapshot.Values())
			prepared.Inputs.push_back({value.Port, value.Data});
		prepared.InputImages.reserve(snapshot.Images().size());
		for (const auto &image : snapshot.Images())
			prepared.InputImages.push_back({image.Port, SurfaceHash(image.Data)});
		capture = std::move(prepared);
		diagnostic = {};
		return Status::Ok;
	}
}
