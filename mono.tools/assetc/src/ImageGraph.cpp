#include <engine/assets/ContentHash.hpp>
#include <engine/assets/Texture.hpp>
#include <engine/bake/Image.hpp>
#include <engine/bake/ImageGraph.hpp>
#include <engine/core/Bytes.hpp>
#include <engine/imagegraph/Reference.hpp>

#include <algorithm>
#include <assetc/ImageGraph.hpp>
#include <atomic>
#include <fstream>
#include <map>
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
		return ReadImageGraphSourceFileTyped(
			allowedRoot, project, engine::imagegraph::Source{std::string(reference)}, policy, out, failure
		);
	}
	bool ReadImageGraphSourceFileTyped(
		const fs::path &allowedRoot,
		const fs::path &project,
		const engine::imagegraph::Source &graphSource,
		const engine::assets::ContentPolicy &policy,
		engine::imagegraph::Image &out,
		std::string &failure
	) {
		const std::string_view reference = graphSource.Path;
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
		if (extension == ".imagegraph" || extension == ".aimagegraph") {
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
		return engine::bake::DecodeImageGraphSourceTyped(graphSource, bytes, out, failure);
	}
	bool CookImageGraphProject(
		const fs::path &allowedRoot,
		const fs::path &project,
		const engine::imagegraph::Document &document,
		std::string_view graphAssetName,
		const engine::assets::ContentPolicy &policy,
		engine::bake::CookedImageGraph &out,
		std::string &failure
	) {
		if (!policy.Allows(engine::assets::FormOfName("project.imagegraph")) ||
			!policy.Allows(engine::assets::FormOfName("project.aimagegraph")) ||
			!policy.Allows(engine::assets::ContentForm::ATex)) {
			failure = "live graph project, runtime graph or texture content is turned off";
			return false;
		}
		fs::path root, location;
		if (!Paths(allowedRoot, project, root, location, failure, false)) return false;
		const auto resolver = [&](const engine::imagegraph::Source &source,
								  engine::imagegraph::Image &image,
								  std::string &reason) {
			return ReadImageGraphSourceFileTyped(root, location, source, policy, image, reason);
		};
		engine::imagegraph::Diagnostic diagnostic;
		if (!engine::bake::CookImageGraph(document, graphAssetName, resolver, out, diagnostic)) {
			failure =
				diagnostic.Node.empty() ? diagnostic.Message : diagnostic.Node + ": " + diagnostic.Message;
			return false;
		}
		return true;
	}

	bool PublishCookedImageGraph(
		const fs::path &outputRoot, const engine::bake::CookedImageGraph &cooked, std::string &failure
	) {
		if (!engine::imagegraph::IsRuntimeAsset(cooked.Name) ||
			cooked.Name.starts_with("__imagegraph_sources/") ||
			cooked.Sources.size() >
				engine::imagegraph::Limits::MaximumNodes + engine::imagegraph::Limits::MaximumParameters) {
			failure = "live graph publication has an invalid asset name or source count";
			return false;
		}
		for (const auto &output : cooked.Graph.Outputs) {
			if (!engine::imagegraph::IsReferenceToken(output.Name)) {
				failure = "live graph publication output needs a portable reference token";
				return false;
			}
		}
		try {
			std::string canonical;
			engine::imagegraph::Diagnostic diagnostic;
			if (!engine::imagegraph::Write(cooked.Graph, canonical, diagnostic) || canonical != cooked.Text) {
				failure = "live graph publication bytes do not match checked graph";
				return false;
			}
			std::vector<std::pair<std::string, std::vector<std::byte>>> staged;
			std::map<std::string, const engine::assets::TextureData *> textures;
			size_t retained = 0;
			for (const auto &source : cooked.Sources) {
				if (!source.Texture.IsValid() ||
					source.Texture.Pixels.size() >
						engine::imagegraph::Limits::MaximumRetainedBytes - retained) {
					failure = "live graph publication source exceeds byte budget";
					return false;
				}
				retained += source.Texture.Pixels.size();
				engine::core::ByteWriter writer;
				if (!engine::assets::Texture::Write(writer, source.Texture)) {
					failure = "live graph publication source texture is invalid";
					return false;
				}
				const auto bytes = writer.Bytes();
				const auto expected =
					"__imagegraph_sources/" + engine::assets::Hasher::Of(bytes).ToHex() + ".atex";
				if (source.Name != expected) {
					failure = "live graph source name does not match its canonical texture bytes";
					return false;
				}
				textures.emplace(source.Name, &source.Texture);
				staged.emplace_back(source.Name, std::vector<std::byte>(bytes.begin(), bytes.end()));
			}
			const auto sourceExists = [&](const engine::imagegraph::Source &source) {
				const auto found = textures.find(source.Path);
				if (found == textures.end() ||
					engine::assets::IsSRGB(found->second->Format) !=
						(source.Interpretation == engine::imagegraph::SourceInterpretation::Colour)) {
					failure = "live graph publication source closure is missing or has wrong sampling space";
					return false;
				}
				return true;
			};
			for (const auto &node : cooked.Graph.Nodes) {
				const auto *source = std::get_if<engine::imagegraph::Source>(&node.Value);
				if (source && !sourceExists(*source)) return false;
			}
			engine::imagegraph::Document resolved;
			engine::imagegraph::Plan plan;
			if (!engine::imagegraph::ResolveInputs(cooked.Graph, {}, resolved, diagnostic) ||
				!engine::imagegraph::Compile(resolved, plan, diagnostic)) {
				failure = diagnostic.Message;
				return false;
			}
			std::vector<engine::imagegraph::SourceExtent> extents;
			for (const auto &node : resolved.Nodes) {
				const auto *source = std::get_if<engine::imagegraph::Source>(&node.Value);
				if (!source) continue;
				if (!sourceExists(*source)) return false;
				const auto *texture = textures.at(source->Path);
				extents.push_back({node.Id, texture->Width, texture->Height});
			}
			for (const auto &output : resolved.Outputs) {
				engine::imagegraph::ExecutionPlan execution;
				if (!engine::imagegraph::Prepare(
						resolved, plan, output.Name, extents, execution, diagnostic
					)) {
					failure = diagnostic.Message;
					return false;
				}
			}
			std::error_code error;
			fs::create_directories(outputRoot, error);
			const auto root = fs::canonical(outputRoot, error);
			if (error || !fs::is_directory(root, error)) {
				failure = "live graph publication root is unavailable";
				return false;
			}
			const auto confined = [&](const std::string &name) {
				const auto path = fs::weakly_canonical(root / name, error);
				if (error || !Within(root, path)) {
					failure = "live graph publication path leaves output root";
					return false;
				}
				return true;
			};
			if (!confined(cooked.Name)) return false;
			for (const auto &[name, bytes] : staged) {
				if (!confined(name)) return false;
				const auto path = root / name;
				if (fs::exists(path, error)) {
					std::vector<std::byte> existing;
					if (!BoundedRead(path, bytes.size(), existing, failure) || existing != bytes) {
						failure = "live graph source address already holds different bytes";
						return false;
					}
				}
			}
			for (const auto &[name, bytes] : staged) {
				if (fs::exists(root / name, error)) continue;
				if (!PublishImageGraphTexture(root / name, bytes, failure)) return false;
			}
			// grug swaps graph last. old graph still names untouched immutable textures.
			return PublishImageGraphTexture(
				root / cooked.Name, std::as_bytes(std::span(cooked.Text.data(), cooked.Text.size())), failure
			);
		} catch (const std::bad_alloc &) {
			failure = "live graph publication allocation failed";
			return false;
		}
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
