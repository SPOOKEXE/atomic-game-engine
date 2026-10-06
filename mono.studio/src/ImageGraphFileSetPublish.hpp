#pragma once

#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/imagegraph/Document.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <new>
#include <span>

namespace studio::detail {
	struct ImageGraphFilePublication {
		std::filesystem::path Destination;
		std::span<const std::byte> Bytes;
	};
	// grug retain old siblings until every replacement succeeds. rollback keeps its backup if restore
	// fails. readers can observe files one at a time; power-loss durability needs a different protocol.
	struct ImageGraphFileSetStage {
		struct Entry {
			std::filesystem::path Destination, Temporary, Backup;
			bool BackedUp = false;
			bool Published = false;
		};
		std::filesystem::path Directory;
		std::array<Entry, 2> Files;
		size_t Count = 0;
		bool Committed = false;
		bool RestoreFailed = false;

		bool Rollback() noexcept {
			bool restored = true;
			for (size_t index = Count; index > 0; --index) {
				auto &file = Files[index - 1];
				std::error_code error;
				if (file.Published) {
					std::filesystem::remove(file.Destination, error);
					if (error) {
						restored = false;
						continue;
					}
					file.Published = false;
				}
				if (file.BackedUp) {
					std::filesystem::rename(file.Backup, file.Destination, error);
					if (error)
						restored = false;
					else
						file.BackedUp = false;
				}
			}
			RestoreFailed = !restored;
			return restored;
		}
		bool Commit() {
			for (size_t index = 0; index < Count; ++index) {
				auto &file = Files[index];
				std::error_code error;
				const auto status = std::filesystem::symlink_status(file.Destination, error);
				if (error && error != std::errc::no_such_file_or_directory) return false;
				if (std::filesystem::exists(status)) {
					if (!std::filesystem::is_regular_file(status)) return false;
					std::filesystem::rename(file.Destination, file.Backup, error);
					if (error) return false;
					file.BackedUp = true;
				}
				std::filesystem::rename(file.Temporary, file.Destination, error);
				if (error) return false;
				file.Published = true;
			}
			Committed = true;
			return true;
		}
		~ImageGraphFileSetStage() {
			if (RestoreFailed || (!Committed && !Rollback())) return;
			if (Directory.empty()) return;
			std::error_code error;
			// grug remove only known siblings; recursive cleanup can allocate after commit.
			for (size_t index = 0; index < Count; ++index) {
				std::filesystem::remove(Files[index].Temporary, error);
				std::filesystem::remove(Files[index].Backup, error);
			}
			std::filesystem::remove(Directory, error);
		}
	};

	// grug stage both collection files on the destination filesystem before moving any old file.
	[[nodiscard]] inline bool PublishImageGraphFileSet(
		std::span<const ImageGraphFilePublication> files,
		engine::imagegraph::Diagnostic &diagnostic,
		uint64_t maximumBytes
	) try {
		ENGINE_PROFILE("studio.imagegraph.file_set_publish");
		using namespace engine::imagegraph;
		diagnostic = {};
		const auto fail = [&](Status code, const char *message) {
			diagnostic = {code, {}, "path", message};
			return false;
		};
		if (files.empty() || files.size() > 2)
			return fail(Status::InvalidValue, "graph file set must contain one or two files");
		if (!maximumBytes || maximumBytes > Limits::MaximumEvaluationBytes)
			return fail(Status::LimitExceeded, "graph file set allowance is outside bounds");
		uint64_t remaining = maximumBytes;
		ImageGraphFileSetStage stage;
		for (size_t index = 0; index < files.size(); ++index) {
			const auto &file = files[index];
			if (file.Destination.empty() || file.Destination.filename().empty() ||
				file.Destination.native().size() > 4096)
				return fail(Status::InvalidValue, "enter a bounded graph file set destination");
			if (file.Bytes.size() > remaining)
				return fail(Status::LimitExceeded, "graph file set exceeds its byte limit");
			remaining -= file.Bytes.size();
			std::error_code error;
			auto &entry = stage.Files[index];
			entry.Destination = std::filesystem::absolute(file.Destination, error).lexically_normal();
			if (error) return fail(Status::InvalidValue, "graph file set path is invalid");
			if (index && (entry.Destination.parent_path() != stage.Files[0].Destination.parent_path() ||
						  entry.Destination == stage.Files[0].Destination))
				return fail(Status::InvalidValue, "graph file set needs distinct sibling paths");
			const auto status = std::filesystem::symlink_status(entry.Destination, error);
			if ((error && error != std::errc::no_such_file_or_directory) ||
				(std::filesystem::exists(status) && !std::filesystem::is_regular_file(status)))
				return fail(Status::InvalidValue, "graph file set destination is not a regular file");
		}
		static std::atomic<uint64_t> serial{0};
		const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
		for (unsigned attempt = 0; attempt < 32; ++attempt) {
			auto directory =
				stage.Files[0].Destination.parent_path() /
				(".atomic-graph-set-" + std::to_string(stamp) + "-" + std::to_string(serial.fetch_add(1)));
			std::error_code error;
			if (std::filesystem::create_directory(directory, error)) {
				stage.Directory = std::move(directory);
				break;
			}
			if (error && error != std::errc::file_exists)
				return fail(Status::InvalidValue, "could not create graph file set staging directory");
		}
		if (stage.Directory.empty())
			return fail(Status::InvalidValue, "could not reserve graph file set staging directory");
		stage.Count = files.size();
		for (size_t index = 0; index < files.size(); ++index) {
			auto &entry = stage.Files[index];
			entry.Temporary = stage.Directory / (std::to_string(index) + ".new");
			entry.Backup = stage.Directory / (std::to_string(index) + ".old");
			const auto native = entry.Temporary.string();
			FILE *stream = std::fopen(native.c_str(), "wbx");
			if (!stream) return fail(Status::InvalidValue, "could not create graph file set temporary");
			const auto bytes = files[index].Bytes;
			const bool complete =
				(bytes.empty() || std::fwrite(bytes.data(), 1, bytes.size(), stream) == bytes.size()) &&
				std::fflush(stream) == 0;
			const bool closed = std::fclose(stream) == 0;
			if (!complete || !closed)
				return fail(Status::InvalidValue, "could not write complete graph file set temporary");
		}
		// grug register counters before moving old files; telemetry allocation must not undo a save.
		engine::core::Metrics::Count("studio.graph_file_set.staged_bytes", double(maximumBytes - remaining));
		engine::core::Metrics::Count("studio.graph_file_set.staged_files", double(files.size()));
		if (!stage.Commit()) {
			if (!stage.Rollback()) {
				diagnostic = {
					Status::InvalidValue,
					{},
					"path",
					"graph file set restore failed; retained backups at " + stage.Directory.string()
				};
				return false;
			}
			return fail(Status::InvalidValue, "could not replace graph file set; previous files restored");
		}
		return true;
	} catch (const std::bad_alloc &) {
		diagnostic = {
			engine::imagegraph::Status::LimitExceeded, {}, "path", "graph file set allocation failed"
		};
		return false;
	} catch (const std::filesystem::filesystem_error &) {
		diagnostic = {engine::imagegraph::Status::InvalidValue, {}, "path", "graph file set path is invalid"};
		return false;
	}
}
