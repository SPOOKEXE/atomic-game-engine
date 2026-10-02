#include "WavExport.hpp"

#include <atomic>
#include <cstdio>
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
		static std::atomic<uint64_t> serial{0};
		const std::filesystem::path destination(candidate.Path);
		auto temporary = destination;
		temporary += ".atomic-export-" + std::to_string(serial.fetch_add(1)) + ".tmp";
		std::error_code error;
		// Exclusive creation prevents truncating another export's temporary file.
		struct TemporaryCleanup {
			const std::filesystem::path &Path;
			bool Active = false;
			~TemporaryCleanup() {
				if (!Active) return;
				try {
					std::error_code ignored;
					std::filesystem::remove(Path, ignored);
				} catch (...) {}
			}
		} cleanup{temporary};
		FILE *stream = std::fopen(temporary.string().c_str(), "wbx");
		if (!stream) {
			diagnostic = {
				Status::InvalidValue, std::string(nodeId), "path", "could not create WAV temporary file"
			};
			return false;
		}
		cleanup.Active = true;
		const bool written =
			std::fwrite(candidate.Bytes.data(), 1, candidate.Bytes.size(), stream) == candidate.Bytes.size();
		const bool closed = std::fclose(stream) == 0;
		if (!written || !closed) {
			std::filesystem::remove(temporary, error);
			diagnostic = {
				Status::InvalidValue,
				std::string(nodeId),
				"path",
				"could not write complete WAV temporary file"
			};
			return false;
		}
		std::filesystem::rename(temporary, destination, error);
		if (error) {
			const auto reason = error.message();
			std::filesystem::remove(temporary, error);
			diagnostic = {
				Status::InvalidValue,
				std::string(nodeId),
				"path",
				"could not replace WAV destination: " + reason
			};
			return false;
		}
		cleanup.Active = false;
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
