#pragma once

// grug share one file commit path for native graphs and source archives.

#include <engine/core/Profiling.hpp>
#include <engine/imagegraph/Document.hpp>

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <new>
#include <span>

namespace studio::detail {
	// grug close the complete sibling before replacing the old save. this does not promise power-loss
	// durability.
	[[nodiscard]] inline bool PublishImageGraphFile(
		const std::filesystem::path &destination,
		std::span<const std::byte> bytes,
		engine::imagegraph::Diagnostic &diagnostic,
		uint64_t maximumBytes
	) try {
		ENGINE_PROFILE("studio.imagegraph.file_publish");
		using namespace engine::imagegraph;
		diagnostic = {};
		const auto fail = [&](Status code, const char *message) {
			diagnostic = {code, {}, "path", message};
			return false;
		};
		if (destination.empty()) return fail(Status::InvalidValue, "enter a graph destination path");
		if (!maximumBytes || bytes.size() > maximumBytes)
			return fail(Status::LimitExceeded, "graph save exceeds its file byte limit");
		static std::atomic<uint64_t> serial{0};
		auto temporary = destination;
		temporary += ".atomic-graph-" + std::to_string(serial.fetch_add(1)) + ".tmp";
		FILE *stream = std::fopen(temporary.string().c_str(), "wbx");
		if (!stream) return fail(Status::InvalidValue, "could not create graph temporary file");
		const bool complete =
			(bytes.empty() || std::fwrite(bytes.data(), 1, bytes.size(), stream) == bytes.size()) &&
			std::fflush(stream) == 0;
		const bool closed = std::fclose(stream) == 0;
		std::error_code error;
		if (!complete || !closed) {
			std::filesystem::remove(temporary, error);
			return fail(Status::InvalidValue, "could not write complete graph temporary file");
		}
		std::filesystem::rename(temporary, destination, error);
		if (error) {
			std::filesystem::remove(temporary, error);
			return fail(Status::InvalidValue, "could not replace graph destination");
		}
		return true;
	} catch (const std::bad_alloc &) {
		diagnostic = {engine::imagegraph::Status::LimitExceeded, {}, "path", "graph save allocation failed"};
		return false;
	} catch (const std::filesystem::filesystem_error &) {
		diagnostic = {engine::imagegraph::Status::InvalidValue, {}, "path", "graph save path is invalid"};
		return false;
	}
}
