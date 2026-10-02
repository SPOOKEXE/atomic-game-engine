#include "WavExport.hpp"

#include <engine/imagegraphexport/GraphFileHost.hpp>

#include <filesystem>
#include <imgui.h>
#include <new>
#include <stdexcept>

namespace studio {
	static bool ExportImageGraphWavImpl(
		const engine::imagegraph::Document &document,
		const engine::imagegraph::Plan &plan,
		std::string_view nodeId,
		const engine::imagegraph::EvaluationRequest &request,
		engine::imagegraph::WavExport &lastGood,
		engine::imagegraph::Diagnostic &diagnostic
	) {
		using namespace engine::imagegraph;
		const auto &policy = engine::assets::ContentPolicy::Process(engine::assets::ContentVerb::Publish);
		if (!policy.Allows(engine::assets::ContentForm::Wav)) {
			diagnostic = {
				Status::UnsupportedExecution,
				std::string(nodeId),
				"path",
				"WAV publication refused by content policy"
			};
			return false;
		}
		WavExport candidate;
		const uint64_t oldBytes = lastGood.Bytes.capacity() + lastGood.Path.capacity() + 1;
		if (oldBytes > Limits::MaximumEvaluationBytes) {
			diagnostic = {Status::LimitExceeded, std::string(nodeId), {}, "previous WAV export exceeds cap"};
			return false;
		}
		if (PrepareWavExport(
				document,
				plan,
				nodeId,
				request,
				Limits::MaximumEvaluationBytes - oldBytes,
				candidate,
				diagnostic
			) != Status::Ok)
			return false;
		if (!policy.AllowsName(candidate.Path)) {
			diagnostic = {
				Status::UnsupportedExecution,
				std::string(nodeId),
				"path",
				"WAV destination refused by content policy"
			};
			return false;
		}
		// This explicit action grants only the resolved, suffix-normalized destination.
		const engine::imagegraphexport::GraphFileGrant grant{
			std::string(nodeId), std::filesystem::path(candidate.Path), true
		};
		std::string failure;
		if (!engine::imagegraphexport::PublishGraphHostFile(
				grant, policy, candidate.Bytes, Limits::MaximumEvaluationBytes - oldBytes, failure
			)) {
			diagnostic = {Status::InvalidValue, std::string(nodeId), "path", std::move(failure)};
			return false;
		}
		lastGood = std::move(candidate);
		diagnostic = {};
		return true;
	}
	bool ExportImageGraphWav(
		const engine::imagegraph::Document &document,
		const engine::imagegraph::Plan &plan,
		std::string_view nodeId,
		const engine::imagegraph::EvaluationRequest &request,
		engine::imagegraph::WavExport &lastGood,
		engine::imagegraph::Diagnostic &diagnostic
	) {
		try {
			return ExportImageGraphWavImpl(document, plan, nodeId, request, lastGood, diagnostic);
		} catch (const std::bad_alloc &) {
			diagnostic = {engine::imagegraph::Status::LimitExceeded, {}, {}, "WAV host allocation failed"};
		} catch (const std::length_error &) {
			diagnostic = {
				engine::imagegraph::Status::LimitExceeded, {}, {}, "WAV host allocation length exceeded"
			};
		} catch (const std::filesystem::filesystem_error &) {
			diagnostic = {
				engine::imagegraph::Status::InvalidValue, {}, "path", "WAV host filesystem operation failed"
			};
		}
		return false;
	}
	void DrawImageGraphWavExport(
		const engine::imagegraph::Document &document,
		std::string_view nodeId,
		const engine::imagegraph::EvaluationRequest &request,
		engine::imagegraph::WavExport &lastGood,
		std::string &message
	) {
		if (ImGui::Button("Export WAV")) {
			engine::imagegraph::Diagnostic diagnostic;
			engine::imagegraph::Plan plan;
			message =
				engine::imagegraph::Compile(document, plan, diagnostic) == engine::imagegraph::Status::Ok &&
						ExportImageGraphWav(document, plan, nodeId, request, lastGood, diagnostic)
					? "Exported " + lastGood.Path
					: diagnostic.Message;
		}
		ImGui::TextWrapped("Native PCM export clamps samples and rounds ties to even.");
		if (!message.empty()) ImGui::TextWrapped("%s", message.c_str());
	}
}
