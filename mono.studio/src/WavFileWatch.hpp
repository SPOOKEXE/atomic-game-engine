#pragma once

#include "WavFileChecker.hpp"

#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>

#include <chrono>
#include <filesystem>
#include <string>

namespace studio::detail {
	inline std::optional<int64_t> WavFileModifiedSecond(const std::filesystem::path &path) {
		ENGINE_PROFILE("studio.wav_checker.metadata");
		std::error_code error;
		engine::core::Metrics::Count("studio.wav_checker.metadata_queries", 1);
		if (!std::filesystem::is_regular_file(path, error) || error) return std::nullopt;
		engine::core::Metrics::Count("studio.wav_checker.metadata_queries", 1);
		const auto modified = std::filesystem::last_write_time(path, error);
		if (error) return std::nullopt;
		const auto system = std::filesystem::file_time_type::clock::to_sys(modified);
		return std::chrono::floor<std::chrono::seconds>(system.time_since_epoch()).count();
	}
	struct GrantedWavFileWatch {
		std::string SourceId;
		std::string GrantedPath;
		WavFileCheckerState Checker;
	};
	struct PreparedWavFileWatch {
		size_t Slot = 0;
		GrantedWavFileWatch Record;
	};
	struct WavFileWatches {
		std::array<std::optional<GrantedWavFileWatch>, engine::imagegraph::Limits::MaximumNodes> Files;
		uint64_t RetainedBytes() const {
			uint64_t bytes = sizeof(*this);
			for (const auto &file : Files)
				if (file) bytes += file->SourceId.capacity() + file->GrantedPath.capacity();
			return bytes;
		}
		// Only explicit successful loads establish authority. Authored node paths never replace it.
		bool PrepareBinding(
			std::string_view source,
			const std::filesystem::path &path,
			uint64_t maximumBytes,
			PreparedWavFileWatch &prepared,
			engine::imagegraph::Diagnostic &diagnostic
		) const {
			using namespace engine::imagegraph;
			const auto fail = [&](Status code, const char *message) {
				diagnostic = {code, {}, {}, message};
				return false;
			};
			if (source.empty() || source.size() > Limits::MaximumTextBytes || path.empty() ||
				path.native().size() > Limits::MaximumTextBytes)
				return fail(Status::LimitExceeded, "WAV checker grant text exceeds its bound");
			const uint64_t nameBytes = std::max(source.size(), std::string{}.capacity());
			// Path conversion and metadata lookup stay inside the bounded explicit load path.
			const uint64_t workspace =
				sizeof(PreparedWavFileWatch) + nameBytes + path.native().size() * 32 + 128;
			if (workspace > maximumBytes)
				return fail(Status::LimitExceeded, "WAV checker binding exceeds its live byte budget");
			size_t slot = Files.size(), available = Files.size();
			for (size_t index = 0; index < Files.size(); ++index) {
				if (!Files[index]) {
					if (available == Files.size()) available = index;
				} else if (Files[index]->SourceId == source)
					slot = index;
			}
			if (slot == Files.size()) slot = available;
			if (slot == Files.size())
				return fail(Status::LimitExceeded, "WAV checker grants exceed the native source bound");
			const auto modified = WavFileModifiedSecond(path);
			if (!modified) return fail(Status::InvalidValue, "WAV checker cannot observe the granted file");
			PreparedWavFileWatch candidate;
			candidate.Slot = slot;
			candidate.Record.SourceId = source;
			candidate.Record.GrantedPath = path.string();
			if (Files[slot] && Files[slot]->GrantedPath == candidate.Record.GrantedPath)
				candidate.Record.Checker = Files[slot]->Checker;
			candidate.Record.Checker.EditSecond =
				Files[slot] && Files[slot]->GrantedPath == candidate.Record.GrantedPath
					? std::max(*modified, candidate.Record.Checker.EditSecond)
					: *modified;
			prepared = std::move(candidate);
			diagnostic = {};
			return true;
		}
		void Publish(PreparedWavFileWatch prepared) {
			Files[prepared.Slot] = std::move(prepared.Record);
		}
		void Remove(std::string_view source) {
			for (auto &file : Files)
				if (file && file->SourceId == source) file.reset();
		}
	};
}
