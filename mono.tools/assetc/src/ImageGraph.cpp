#include <engine/assets/Texture.hpp>
#include <engine/bake/Image.hpp>
#include <engine/bake/ImageGraph.hpp>
#include <engine/core/Bytes.hpp>

#include <algorithm>
#include <assetc/ImageGraph.hpp>
#include <atomic>
#include <fstream>
#include <new>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace assetc {
	namespace fs = std::filesystem;
	namespace {
		bool Within(const fs::path &root, const fs::path &path) {
			return std::mismatch(root.begin(), root.end(), path.begin(), path.end()).first == root.end();
		}
		bool Paths(
			const fs::path &allowedRoot,
			const fs::path &project,
			fs::path &root,
			fs::path &location,
			std::string &failure,
			bool requireProject = true
		) {
			std::error_code error;
			root = fs::canonical(allowedRoot, error);
			if (error || !fs::is_directory(root, error)) {
				failure = "image graph allowed root is unavailable";
				return false;
			}
			location = requireProject ? fs::canonical(project, error) : fs::weakly_canonical(project, error);
			if (error || !Within(root, location) ||
				(requireProject && !fs::is_regular_file(location, error)) ||
				(!requireProject && !fs::is_directory(location.parent_path(), error))) {
				failure = "image graph project is unavailable or outside the allowed root";
				return false;
			}
			return true;
		}
		bool
		BoundedRead(const fs::path &path, size_t maximum, std::vector<std::byte> &out, std::string &failure) {
			std::ifstream file(path, std::ios::binary | std::ios::ate);
			const auto length = file.tellg();
			if (!file || length <= 0 || static_cast<uint64_t>(length) > maximum) {
				failure = "image graph input is empty, unavailable or exceeds its byte budget";
				return false;
			}
			try {
				std::vector<std::byte> bytes(static_cast<size_t>(length));
				file.seekg(0);
				file.read(reinterpret_cast<char *>(bytes.data()), length);
				if (!file || file.peek() != std::char_traits<char>::eof()) {
					failure = "image graph input changed during reading";
					return false;
				}
				out = std::move(bytes);
				return true;
			} catch (const std::bad_alloc &) {
				failure = "image graph input allocation failed";
				return false;
			}
		}
	}
	bool ReadImageGraphProject(
		const fs::path &allowedRoot,
		const fs::path &project,
		engine::imagegraph::Document &out,
		std::string &failure
	) {
		fs::path root, location;
		if (!Paths(allowedRoot, project, root, location, failure)) return false;
		std::vector<std::byte> bytes;
		if (!BoundedRead(location, engine::imagegraph::Limits::MaximumDocumentBytes, bytes, failure))
			return false;
		engine::imagegraph::Diagnostic diagnostic;
		if (!engine::imagegraph::Read(
				std::string_view(reinterpret_cast<const char *>(bytes.data()), bytes.size()), out, diagnostic
			)) {
			failure =
				diagnostic.Node.empty() ? diagnostic.Message : diagnostic.Node + ": " + diagnostic.Message;
			return false;
		}
		return true;
	}
	bool ReadImageGraphSourceFile(
		const fs::path &allowedRoot,
		const fs::path &project,
		std::string_view reference,
		const engine::assets::ContentPolicy &policy,
		engine::imagegraph::Image &out,
		std::string &failure
	) {
		if (reference.empty() || reference.size() > 4096 || reference.find('\0') != std::string_view::npos ||
			reference.find('\\') != std::string_view::npos || reference.find(':') != std::string_view::npos) {
			failure = "image graph source must be a portable relative path";
			return false;
		}
		const fs::path relative{std::string(reference)};
		if (relative.is_absolute() || relative.has_root_name() || relative.has_root_directory()) {
			failure = "image graph source must be relative to its project";
			return false;
		}
		fs::path root, location;
		if (!Paths(allowedRoot, project, root, location, failure, false)) return false;
		std::error_code error;
		const auto source = fs::canonical(location.parent_path() / relative, error);
		if (error || !Within(root, source) || !fs::is_regular_file(source, error)) {
			failure = "image graph source is missing or outside the allowed root";
			return false;
		}
		std::string extension = source.extension().string();
		std::transform(extension.begin(), extension.end(), extension.begin(), [](char value) {
			return value >= 'A' && value <= 'Z' ? char(value - 'A' + 'a') : value;
		});
		if (extension == ".imagegraph") {
			failure = "recursive image graph sources are not supported";
			return false;
		}
		if (!policy.Allows(engine::assets::FormOfName(reference))) {
			failure = "image graph source content is turned off";
			return false;
		}
		std::vector<std::byte> bytes;
		if (!BoundedRead(source, engine::imagegraph::Limits::MaximumImageBytes, bytes, failure)) return false;
		engine::assets::ContentForm decodedForm = engine::assets::ContentForm::Unknown;
		switch (engine::bake::ImageFormatOfBytes(bytes)) {
		case engine::bake::ImageFormat::Png:
			decodedForm = engine::assets::ContentForm::Png;
			break;
		case engine::bake::ImageFormat::Jpeg:
			decodedForm = engine::assets::ContentForm::Jpeg;
			break;
		case engine::bake::ImageFormat::Bmp:
			decodedForm = engine::assets::ContentForm::Bmp;
			break;
		case engine::bake::ImageFormat::Gif:
			decodedForm = engine::assets::ContentForm::Gif;
			break;
		case engine::bake::ImageFormat::Svg:
		case engine::bake::ImageFormat::Unknown:
			break;
		}
		if (decodedForm == engine::assets::ContentForm::Unknown) {
			engine::core::ByteReader reader(bytes);
			if (reader.ReadUInt32() == engine::assets::Texture::MAGIC)
				decodedForm = engine::assets::ContentForm::ATex;
		}
		if (!policy.Allows(decodedForm)) {
			failure = "image graph decoded source content is turned off";
			return false;
		}
		return engine::bake::DecodeImageGraphSource(reference, bytes, out, failure);
	}
	bool
	PublishImageGraphTexture(const fs::path &path, std::span<const std::byte> bytes, std::string &failure) {
		std::error_code error;
		fs::create_directories(path.parent_path(), error);
		if (error) {
			failure = "cannot create image graph output directory";
			return false;
		}
		static std::atomic<uint64_t> serial{0};
		fs::path scratch;
		bool claimed = false;
		for (size_t attempt = 0; attempt < 32 && !claimed; ++attempt) {
			scratch = path;
			scratch += ".imagegraph-write-" + std::to_string(serial.fetch_add(1, std::memory_order_relaxed));
			claimed = fs::create_directory(scratch, error);
			if (error && error != std::errc::file_exists) break;
		}
		if (!claimed) {
			failure = "cannot stage image graph output";
			return false;
		}
		const auto staged = scratch / "texture";
		bool complete = false;
		{
			std::ofstream file(staged, std::ios::binary | std::ios::trunc);
			file.write(
				reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size())
			);
			file.flush();
			complete = file.good();
			file.close();
			complete = complete && !file.fail();
		}
		if (complete) {
#ifdef _WIN32
			complete = MoveFileExW(
						   staged.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH
					   ) != 0;
#else
			fs::rename(staged, path, error);
			complete = !error;
#endif
		}
		fs::remove_all(scratch, error);
		if (!complete) failure = "cannot publish image graph output";
		return complete;
	}
}
