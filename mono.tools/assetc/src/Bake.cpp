#include <engine/bake/Graph.hpp>
#include <engine/bake/Image.hpp>
#include <engine/bake/Model.hpp>
#include <engine/core/Bytes.hpp>

#include <algorithm>
#include <assetc/Bake.hpp>
#include <fstream>
#include <map>
#include <set>

namespace assetc {

	namespace fs = std::filesystem;

	namespace {
		using engine::assets::AssetKind;

		// The extension a baked mesh and a baked texture get.
		//
		// `assets::KindOfName` knows both, which is what lets a publisher
		// pointed at the output tree classify without being told anything about
		// this program.
		constexpr std::string_view MESH_EXTENSION = ".amesh";
		constexpr std::string_view TEXTURE_EXTENSION = ".atex";

		std::string Lowered(std::string text) {
			std::transform(text.begin(), text.end(), text.begin(), [](char value) {
				return (value >= 'A' && value <= 'Z') ? static_cast<char>(value - 'A' + 'a') : value;
			});
			return text;
		}

		// Forward slashes, whatever the platform spells them as. A manifest's
		// names are forward-slashed, so a tree baked on Windows and one baked
		// here have to produce the same names or the two disagree about what
		// was published.
		std::string Slashed(std::string text) {
			std::replace(text.begin(), text.end(), '\\', '/');
			return text;
		}

		std::string ExtensionOf(std::string_view path) {
			const size_t dot = path.find_last_of('.');
			if (dot == std::string_view::npos || path.find('/', dot) != std::string_view::npos) {
				return {};
			}
			return Lowered(std::string(path.substr(dot)));
		}

		std::string WithoutExtension(std::string_view path) {
			const size_t dot = path.find_last_of('.');
			if (dot == std::string_view::npos || path.find('/', dot) != std::string_view::npos) {
				return std::string(path);
			}
			return std::string(path.substr(0, dot));
		}

		bool IsModel(std::string_view extension) {
			return extension == ".glb" || extension == ".gltf" || extension == ".obj" || extension == ".pmx";
		}

		bool IsImage(std::string_view extension) {
			return extension == ".png" || extension == ".jpg" || extension == ".jpeg" || extension == ".bmp";
		}

		std::vector<std::byte> ReadFile(const fs::path &path) {
			std::ifstream file(path, std::ios::binary);
			if (!file) {
				return {};
			}
			file.seekg(0, std::ios::end);
			const std::streamoff length = file.tellg();
			if (length <= 0) {
				return {};
			}
			file.seekg(0, std::ios::beg);

			std::vector<std::byte> bytes(static_cast<size_t>(length));
			file.read(reinterpret_cast<char *>(bytes.data()), length);
			bytes.resize(static_cast<size_t>(file.gcount()));
			return bytes;
		}

		bool WriteFile(const fs::path &path, std::span<const std::byte> bytes) {
			std::error_code error;
			fs::create_directories(path.parent_path(), error);

			std::ofstream file(path, std::ios::binary | std::ios::trunc);
			if (!file) {
				return false;
			}
			file.write(
				reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size())
			);
			return file.good();
		}

		// Where a path relative to one file's directory lands, relative to the
		// tree's root.
		//
		// **Resolves `..` and refuses to leave the tree.** A model naming
		// `../../../etc/passwd` as its texture is the traversal case, and this
		// is a build tool run over content somebody uploaded — `cdn::ContentRoot`
		// does the same job on the publishing side and for the same reason.
		bool Resolve(std::string_view directory, std::string_view relative, std::string &out) {
			std::vector<std::string> parts;

			const auto push = [&parts](std::string_view text) {
				size_t start = 0;
				while (start <= text.size()) {
					const size_t slash = std::min(text.find('/', start), text.size());
					const std::string_view part = text.substr(start, slash - start);
					start = slash + 1;

					if (part.empty() || part == ".") {
						continue;
					}
					if (part == "..") {
						if (parts.empty()) {
							return false;
						}
						parts.pop_back();
						continue;
					}
					parts.emplace_back(part);
				}
				return true;
			};

			if (!push(directory) || !push(relative)) {
				return false;
			}

			out.clear();
			for (const std::string &part : parts) {
				if (!out.empty()) {
					out.push_back('/');
				}
				out += part;
			}
			return !out.empty();
		}
	}

	std::string BakedName(std::string_view path) {
		const std::string extension = ExtensionOf(path);
		if (IsModel(extension)) {
			return WithoutExtension(path) + std::string(MESH_EXTENSION);
		}
		if (IsImage(extension)) {
			return WithoutExtension(path) + std::string(TEXTURE_EXTENSION);
		}
		return std::string(path);
	}

	Report Bake(const Settings &settings, std::string &failure) {
		Report report;

		std::error_code error;
		if (!fs::is_directory(settings.Input, error)) {
			failure = "assetc: " + settings.Input.string() + " is not a directory";
			return report;
		}
		fs::create_directories(settings.Output, error);
		if (error) {
			failure = "assetc: cannot create " + settings.Output.string();
			return report;
		}

		// Gathered and sorted before anything is baked, so two runs over one
		// tree produce the same report in the same order. Directory iteration
		// order is the filesystem's business and differs between machines,
		// which would otherwise make a build log undiffable.
		std::vector<std::string> sources;
		for (const fs::directory_entry &entry : fs::recursive_directory_iterator(settings.Input, error)) {
			if (!entry.is_regular_file()) {
				continue;
			}
			sources.push_back(Slashed(fs::relative(entry.path(), settings.Input).generic_string()));
		}
		std::sort(sources.begin(), sources.end());

		for (const std::string &relative : sources) {
			Baked baked;
			baked.Source = relative;

			const std::vector<std::byte> bytes = ReadFile(settings.Input / relative);
			report.SourceBytes += bytes.size();

			if (bytes.empty()) {
				baked.Failure = "unreadable or empty";
				report.Failures++;
				report.Assets.push_back(std::move(baked));
				continue;
			}

			const std::string extension = ExtensionOf(relative);
			const bool model = IsModel(extension);
			const bool image = IsImage(extension);

			if (!model && !image) {
				if (!settings.CopyUnknown) {
					continue;
				}

				// Copied rather than skipped, so the output tree is the whole
				// game and not just the half this program understands.
				baked.Output = relative;
				baked.Kind = engine::assets::KindOfName(relative);
				baked.Bytes = bytes.size();
				if (!WriteFile(settings.Output / relative, bytes)) {
					baked.Failure = "cannot write";
					report.Failures++;
				} else {
					report.OutputBytes += bytes.size();
				}
				report.Assets.push_back(std::move(baked));
				continue;
			}

			engine::bake::Graph graph;
			const engine::bake::NodeId source = graph.AddSource(relative, bytes);
			const engine::bake::NodeId import = graph.Add(engine::bake::NodeKind::Import);
			graph.Connect(source, import);

			engine::bake::NodeId tail = import;

			if (model && settings.ModelSize > 0.0f) {
				const engine::bake::NodeId fit = graph.AddFit(settings.ModelSize);
				graph.Connect(tail, fit);
				tail = fit;
			}

			// The resize is added unconditionally for an oversized image, and
			// the size it targets needs the decoded dimensions — which the
			// graph does not have until it has run. So the import runs first
			// and the tail is extended afterwards, which is the one place this
			// program builds a graph in two passes.
			std::string graphFailure;
			if (!graph.Run(graphFailure)) {
				baked.Failure = graphFailure;
				report.Failures++;
				report.Assets.push_back(std::move(baked));
				continue;
			}

			if (image && settings.MaximumTexture > 0) {
				const engine::assets::TextureData &decoded = graph.Output(import).Texture;
				const uint32_t longest = std::max(decoded.Width, decoded.Height);
				if (longest > settings.MaximumTexture) {
					const double scale = static_cast<double>(settings.MaximumTexture) / longest;
					const uint32_t width =
						std::max<uint32_t>(1, static_cast<uint32_t>(decoded.Width * scale));
					const uint32_t height =
						std::max<uint32_t>(1, static_cast<uint32_t>(decoded.Height * scale));

					const engine::bake::NodeId resize = graph.AddResize(width, height);
					graph.Connect(tail, resize);
					tail = resize;
				}
			}

			baked.Output = BakedName(relative);

			// **The texture references are rewritten here**, where both halves
			// of the naming rule are in one place: the model reported the path
			// it spells and `BakedName` says what that path became.
			if (model) {
				const size_t slash = relative.find_last_of('/');
				const std::string directory =
					slash == std::string::npos ? std::string() : relative.substr(0, slash);

				engine::assets::MeshData mesh = graph.Output(tail).Mesh;
				for (engine::assets::Submesh &submesh : mesh.Submeshes) {
					if (submesh.Texture.empty()) {
						continue;
					}
					std::string resolved;
					if (!Resolve(directory, submesh.Texture, resolved)) {
						// A texture path that climbs out of the tree. Dropped
						// rather than followed: the submesh draws with its base
						// colour, which is visibly plain rather than silently
						// serving a file from outside the content directory.
						submesh.Texture.clear();
						continue;
					}
					submesh.Texture = BakedName(resolved);
				}

				// Serialised directly rather than through a write node, because
				// the rewriting happened outside the graph and feeding a mesh
				// back into one would need a node kind whose only job is to
				// carry a value the caller already has.
				engine::core::ByteWriter writer;
				if (!engine::assets::Mesh::Write(writer, mesh)) {
					baked.Failure = "the imported mesh is not one the format can hold";
					report.Failures++;
					report.Assets.push_back(std::move(baked));
					continue;
				}

				baked.Kind = AssetKind::Mesh;
				baked.Bytes = writer.Size();
				if (!WriteFile(settings.Output / baked.Output, writer.Bytes())) {
					baked.Failure = "cannot write";
					report.Failures++;
				} else {
					report.OutputBytes += writer.Size();
				}
				report.Assets.push_back(std::move(baked));
				continue;
			}

			const engine::bake::NodeId write = graph.AddWrite(baked.Output);
			graph.Connect(tail, write);

			if (!graph.Run(graphFailure)) {
				baked.Failure = graphFailure;
				report.Failures++;
				report.Assets.push_back(std::move(baked));
				continue;
			}

			const std::span<const engine::bake::BakedAsset> exports = graph.Baked();
			if (exports.empty()) {
				baked.Failure = "the pipeline produced nothing";
				report.Failures++;
				report.Assets.push_back(std::move(baked));
				continue;
			}

			baked.Kind = exports.back().Kind;
			baked.Bytes = exports.back().Bytes.size();
			if (!WriteFile(settings.Output / baked.Output, exports.back().Bytes)) {
				baked.Failure = "cannot write";
				report.Failures++;
			} else {
				report.OutputBytes += baked.Bytes;
			}
			report.Assets.push_back(std::move(baked));
		}

		return report;
	}
}
