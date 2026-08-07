#include <engine/assets/Material.hpp>
#include <engine/bake/Graph.hpp>
#include <engine/bake/Image.hpp>
#include <engine/bake/Model.hpp>
#include <engine/core/Bytes.hpp>

#include <algorithm>
#include <assetc/Bake.hpp>
#include <fstream>
#include <map>
#include <set>
#include <span>
#include <string_view>

namespace assetc {

	namespace fs = std::filesystem;

	namespace {
		using engine::assets::AssetKind;

		// Extensions recognized by the runtime asset catalogue.
		constexpr std::string_view MESH_EXTENSION = ".amesh";
		constexpr std::string_view TEXTURE_EXTENSION = ".atex";
		constexpr std::string_view MATERIAL_EXTENSION = ".amat";

		std::string Lowered(std::string text) {
			std::transform(text.begin(), text.end(), text.begin(), [](char value) {
				return (value >= 'A' && value <= 'Z') ? static_cast<char>(value - 'A' + 'a') : value;
			});
			return text;
		}

		// Manifest paths use forward slashes on every platform.
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

		// **`.mat` is a source and `.amat` is what it bakes to**, the same split
		// `.png` and `.atex` have. What is inside a `.mat` is three lines of text
		// somebody or something wrote — see `ReadMaterialSource` — and a runtime
		// parses no text.
		bool IsMaterial(std::string_view extension) {
			return extension == ".mat";
		}

		bool IsImage(std::string_view extension) {
			// **`.gif` is here and its output is a flipbook sheet**, which is the
			// one entry whose baked result is not a picture of its input.
			// `bake::ReadGif` lays the frames out as a square power-of-two grid,
			// so a GIF becomes an ordinary texture and every path downstream —
			// the chunker, the manifest, the renderer's table — handles it with no
			// knowledge that it animates. `bake/src/Gif.cpp` carries the argument.
			return extension == ".png" || extension == ".jpg" || extension == ".jpeg" ||
				   extension == ".bmp" || extension == ".gif";
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

		// Resolves relative references without allowing traversal outside the tree.
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

		// One `key = value` line at a time, `#` to end of line is a comment.
		//
		// **A hand-written parser and not a format with a library**, which is the
		// boring option `AGENTS.md` asks for: a material source has one key today
		// and the whole of what it must do is survive being written by
		// `scripts/fetch-materials.py` and read here. JSON would be a vendor
		// dependency in a tool for six lines of text; a binary source would not be
		// editable, which is the one thing a *source* has to be.
		//
		// **Unknown keys are ignored rather than refused.** The fetcher writes
		// `color` and nothing else today, and `ROADMAP.md` v0.11 adds four more
		// when there is a pass that reads them — a baker that refused an unknown
		// key would make every material written for the newer engine unbakeable by
		// the older one, which is the wrong direction for a content tree that
		// outlives a build.
		std::string MaterialColourOf(std::span<const std::byte> bytes) {
			const std::string_view text(reinterpret_cast<const char *>(bytes.data()), bytes.size());

			size_t start = 0;
			while (start < text.size()) {
				size_t end = std::min(text.find('\n', start), text.size());
				std::string_view line = text.substr(start, end - start);
				start = end + 1;

				line = line.substr(0, std::min(line.find('#'), line.size()));

				const size_t equals = line.find('=');
				if (equals == std::string_view::npos) {
					continue;
				}

				const auto trim = [](std::string_view value) {
					while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
						value.remove_prefix(1);
					}
					while (!value.empty() &&
						   (value.back() == ' ' || value.back() == '\t' || value.back() == '\r')) {
						value.remove_suffix(1);
					}
					return value;
				};

				const std::string key = Lowered(std::string(trim(line.substr(0, equals))));
				if (key == "color" || key == "colour") {
					return std::string(trim(line.substr(equals + 1)));
				}
			}
			return {};
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
		if (IsMaterial(extension)) {
			return WithoutExtension(path) + std::string(MATERIAL_EXTENSION);
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

		// Sort sources so reports are deterministic across filesystems.
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

			// **Handled before the graph, because a material has no pixels.**
			// Everything below this decodes an image or a model; a material is a
			// reference and the only work is resolving it, so running it through
			// `bake::Graph` would need an import node for a format with nothing to
			// import.
			if (IsMaterial(extension)) {
				const std::string colour = MaterialColourOf(bytes);

				engine::assets::MaterialData material;
				if (!colour.empty()) {
					const size_t slash = relative.find_last_of('/');
					const std::string directory =
						slash == std::string::npos ? std::string() : relative.substr(0, slash);

					// **Through the same `BakedName` a model's texture reference
					// goes through**, which is the whole reason that function is
					// exported. A material naming `Bricks_Color.png` and a baked
					// tree holding `Bricks_Color.atex` have to line up, and two
					// spellings of the rule is a material that resolves to nothing
					// on a machine nobody tested.
					std::string resolved;
					if (Resolve(directory, colour, resolved)) {
						material.ColourMap = BakedName(resolved);
					} else {
						// Refuse references outside the input tree, exactly as a
						// model's are refused. An untextured material is a real
						// state — `assets/Material.hpp` — so this is a material
						// that draws the default rather than a failed row.
						baked.Failure = "the colour map is outside the input tree";
						report.Failures++;
					}
				}

				engine::core::ByteWriter writer;
				if (!engine::assets::Material::Write(writer, material)) {
					baked.Failure = "the material is not one the format can hold";
					report.Failures++;
					report.Assets.push_back(std::move(baked));
					continue;
				}

				baked.Output = BakedName(relative);
				baked.Kind = AssetKind::Material;
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

			if (!model && !image) {
				if (!settings.CopyUnknown) {
					continue;
				}

				// Preserve unknown files unless explicitly disabled.
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

			// Decode first; dimensions determine whether resize is needed.
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

			// Rewrite model texture references with the same naming rule.
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
						// Refuse references outside the input tree.
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

			// **After the resize, because the resize carries the fields across
			// and this one overwrites one of them.** The other order would work
			// today and would break the first time a resize stopped preserving
			// the rate — which is exactly the failure `ResizeImage`'s note is
			// about.
			if (image && settings.FlipbookFps > 0.0f) {
				const engine::bake::NodeId retime = graph.AddRetime(settings.FlipbookFps);
				graph.Connect(tail, retime);
				tail = retime;
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
