#pragma once

#include "ImageGraphDocumentEdit.hpp"

#include <engine/imagegraph/RigidMeshAction.hpp>

#include <algorithm>
#include <imgui.h>

namespace studio::detail {
	inline bool ImageGraphRigidMeshCaptureCurrent(
		const engine::imagegraph::Document &document,
		std::string_view nodeId,
		std::string_view selectedNode,
		uint64_t revision,
		uint64_t inputRevision,
		uint64_t capturedRevision,
		uint64_t capturedInputRevision,
		engine::imagegraph::Diagnostic &error
	) {
		const auto node = std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const auto &held) {
			return held.Id == nodeId;
		});
		if (nodeId == selectedNode && node != document.Nodes.end() && node->Type == "pc.rigid_object" &&
			revision == capturedRevision && inputRevision == capturedInputRevision)
			return true;
		error = {
			engine::imagegraph::Status::InvalidValue,
			std::string(nodeId),
			"attribute_mesh",
			"Generate Mesh selection or inputs changed during capture"
		};
		return false;
	}

	// One captured input generation produces one undoable attribute replacement.
	inline bool ApplyImageGraphRigidMeshAction(
		engine::imagegraph::Document &document,
		ImageGraphHistory &history,
		std::string_view nodeId,
		const engine::imagegraph::EvaluationSnapshot &inputs,
		engine::imagegraph::Diagnostic &error,
		uint64_t maximumBytes = engine::imagegraph::Limits::MaximumEvaluationBytes
	) try {
		using namespace engine::imagegraph;
		error = {};
		const auto refuse = [&](Status code, std::string message) {
			error = {code, std::string(nodeId), "attribute_mesh", std::move(message)};
			return false;
		};
		const auto node = std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const auto &held) {
			return held.Id == nodeId;
		});
		if (node == document.Nodes.end() || node->Type != "pc.rigid_object")
			return refuse(Status::UnknownNode, "Generate Mesh requires a rigid object node");
		const auto bytes = DocumentRetainedPayloadBytes(document);
		if (!bytes || *bytes >= maximumBytes / 2)
			return refuse(Status::LimitExceeded, "Generate Mesh document copy exceeds its live byte budget");
		ArrayValue replacement;
		if (PrepareRigidMeshAction(inputs, maximumBytes - 2 * *bytes, replacement, error) != Status::Ok) {
			error.NodeId = std::string(nodeId);
			return false;
		}
		bool attemptedChange = false;
		const bool changed = ApplyImageGraphDocumentEdit(document, history, [&](Document &staged) {
			if (!SetImageGraphValue(staged, nodeId, "attribute_mesh", std::move(replacement), error))
				return false;
			Plan plan;
			if (Compile(staged, plan, error) != Status::Ok) return false;
			attemptedChange = staged != document;
			return true;
		});
		if (!changed && attemptedChange && error.Code == Status::Ok)
			return refuse(Status::LimitExceeded, "Generate Mesh cannot retain its undo transition");
		return changed;
	} catch (const std::bad_alloc &) {
		error = {
			engine::imagegraph::Status::LimitExceeded,
			std::string(nodeId),
			"attribute_mesh",
			"Generate Mesh allocation was refused"
		};
		return false;
	}

	// Capture borrows one prepared snapshot for the synchronous authoring action.
	template <class Capture, class Changed>
	bool DrawImageGraphRigidMeshAction(
		engine::imagegraph::Document &document,
		ImageGraphHistory &history,
		std::string_view nodeId,
		const Capture &capture,
		const Changed &changed,
		engine::imagegraph::Diagnostic &error,
		uint64_t maximumBytes = engine::imagegraph::Limits::MaximumEvaluationBytes
	) {
		if (!ImGui::Button("Generate Mesh")) return false;
		const auto *inputs = capture(error);
		if (!inputs ||
			!ApplyImageGraphRigidMeshAction(document, history, nodeId, *inputs, error, maximumBytes))
			return false;
		changed();
		return true;
	}
}
