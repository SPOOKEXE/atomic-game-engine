#pragma once

#include <engine/imagegraph/Document.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace studio::detail {
	inline bool RenameImageGraphFontArtifact(
		const std::filesystem::path &source, const std::filesystem::path &destination, std::error_code &error
	) {
#ifdef _WIN32
		if (MoveFileExW(
				source.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH
			))
			return true;
		error = std::error_code(static_cast<int>(GetLastError()), std::system_category());
		return false;
#else
		std::filesystem::rename(source, destination, error);
		return !error;
#endif
	}

	template <class WriteStagedFile>
	bool PublishImageGraphFontArtifact(
		const std::filesystem::path &destination,
		WriteStagedFile &&writeStagedFile,
		engine::imagegraph::Diagnostic &diagnostic,
		bool (*replaceFile)(const std::filesystem::path &, const std::filesystem::path &, std::error_code &) =
			RenameImageGraphFontArtifact
	) try {
		if (destination.empty()) {
			diagnostic = {
				engine::imagegraph::Status::InvalidValue, {}, "font inputs", "Artifact path is empty"
			};
			return false;
		}
		static std::atomic<uint64_t> sequence{0};
		const auto parent =
			destination.parent_path().empty() ? std::filesystem::path(".") : destination.parent_path();
		std::filesystem::path staging;
		std::error_code error;
		for (size_t attempt = 0; attempt < 64; ++attempt) {
			const auto suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
								"-" + std::to_string(sequence.fetch_add(1));
			const auto candidate = parent / (".imagegraph-font-" + suffix);
			if (std::filesystem::create_directory(candidate, error)) {
				staging = candidate;
				break;
			}
			if (error && error != std::errc::file_exists) break;
			error.clear();
		}
		if (staging.empty()) {
			diagnostic = {
				engine::imagegraph::Status::InvalidValue,
				{},
				"font inputs",
				"Could not create an exclusive artifact staging directory"
			};
			return false;
		}
		struct Cleanup {
			std::filesystem::path Path;
			bool Keep = false;
			~Cleanup() {
				if (!Keep) {
					std::error_code ignored;
					std::filesystem::remove_all(Path, ignored);
				}
			}
		} cleanup{staging};
		const auto stagedFile = staging / "content";
		if (!std::forward<WriteStagedFile>(writeStagedFile)(stagedFile)) {
			diagnostic = {
				engine::imagegraph::Status::InvalidValue,
				{},
				"font inputs",
				"Could not write staged font artifact"
			};
			return false;
		}
		const auto targetStatus = std::filesystem::symlink_status(destination, error);
		const bool targetExists = !error && std::filesystem::exists(targetStatus);
		if (error && error != std::errc::no_such_file_or_directory) {
			diagnostic = {
				engine::imagegraph::Status::InvalidValue,
				{},
				"font inputs",
				"Could not inspect artifact destination"
			};
			return false;
		}
		if (targetExists &&
			(!std::filesystem::is_regular_file(targetStatus) || std::filesystem::is_symlink(targetStatus))) {
			diagnostic = {
				engine::imagegraph::Status::InvalidValue,
				{},
				"font inputs",
				"Artifact destination must be a regular file"
			};
			return false;
		}
		if (!replaceFile(stagedFile, destination, error)) {
			diagnostic = {
				engine::imagegraph::Status::InvalidValue,
				{},
				"font inputs",
				"Could not atomically publish the staged font artifact"
			};
			return false;
		}
		diagnostic = {};
		return true;
	} catch (...) {
		diagnostic = {
			engine::imagegraph::Status::LimitExceeded,
			{},
			"font inputs",
			"Font artifact publication allocation or filesystem operation failed"
		};
		return false;
	}
} // namespace studio::detail
