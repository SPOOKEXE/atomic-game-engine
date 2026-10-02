#include <engine/imagegraphio/PxcxStructureEdit.hpp>

#include <atomic>
#include <cstdio>
#include <studio/PxcxSave.hpp>

namespace studio {
	bool SavePxcxProjection(
		const std::filesystem::path &destination,
		const engine::bake::PxcxArchive &source,
		const engine::imagegraph::Document &authored,
		engine::imagegraph::FrameTime captureTime,
		engine::imagegraph::Diagnostic &diagnostic
	) {
		using namespace engine::imagegraph;
		diagnostic = {};
		const auto reject = [&](std::string reason) {
			diagnostic = {Status::InvalidValue, {}, "path", std::move(reason)};
			return false;
		};
		if (destination.empty()) return reject("enter a PXC archive path");
		engine::imagegraphio::PxcxImport imported;
		std::string failure;
		if (!engine::imagegraphio::ImportPxcxImageGraph(source, imported, failure)) return reject(failure);
		std::vector<std::byte> bytes;
		if (!engine::imagegraphio::WritePxcxProjection(imported, authored, captureTime, bytes, diagnostic))
			return false;
		static std::atomic<uint64_t> serial{0};
		auto temporary = destination;
		temporary += ".atomic-pxc-" + std::to_string(serial.fetch_add(1)) + ".tmp";
		FILE *stream = std::fopen(temporary.string().c_str(), "wbx");
		if (!stream) return reject("could not create PXC temporary file");
		const bool complete =
			std::fwrite(bytes.data(), 1, bytes.size(), stream) == bytes.size() && std::fflush(stream) == 0;
		const bool closed = std::fclose(stream) == 0;
		std::error_code error;
		if (!complete || !closed) {
			std::filesystem::remove(temporary, error);
			return reject("could not write complete PXC temporary file");
		}
		std::filesystem::rename(temporary, destination, error);
		if (error) {
			std::filesystem::remove(temporary, error);
			return reject("could not replace PXC destination");
		}
		return true;
	}
}
