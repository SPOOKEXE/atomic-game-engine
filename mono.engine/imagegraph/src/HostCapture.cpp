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
		if (node->Type == "pc.3_d_camera" || node->Type == "pc.3_d_camera_set") {
			prepared.CameraRow = 0;
			auto &policy = prepared.CameraPolicy.emplace();
			policy.InheritedSurfaceFormat = snapshot.InheritedSurfaceFormat();
			if (document.Project) {
				policy.ProjectWidth = document.Project->SurfaceWidth;
				policy.ProjectHeight = document.Project->SurfaceHeight;
				policy.ProjectColorDepth = document.Project->ColorDepth;
				policy.ProjectShader3D = document.Project->Shader3D;
			}
			for (const auto &value : snapshot.Values())
				if (value.Port == "dimension") policy.DimensionLinked = value.Linked;
		}
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
