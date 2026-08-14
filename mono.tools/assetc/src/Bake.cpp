#include <engine/assets/Material.hpp>
#include <engine/bake/Graph.hpp>
#include <engine/bake/Image.hpp>
#include <engine/bake/Model.hpp>
#include <engine/core/Bytes.hpp>
#include <engine/core/Log.hpp>

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
			//
			// **`.svg` is here and it is the one that has no size of its own**,
			// which is why it enters the graph through a different node — see
			// `IsVector` and the pipeline below.
			return extension == ".png" || extension == ".jpg" || extension == ".jpeg" ||
				   extension == ".bmp" || extension == ".gif" || extension == ".svg";
		}

		// The images that are drawings rather than pixels.
		//
		// One entry today, and a predicate rather than a comparison because the
		// pipeline asks the question in two places — which node opens the chain,
		// and how the texture cap is applied.
		bool IsVector(std::string_view extension) {
			return extension == ".svg";
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

		// Puts one baked asset wherever the settings say it goes, and records
		// what that cost.
		//
		// **One branch, at the only point where a bake touches its output.**
		// Everything above this — the decode, the fit, the resize, the texture
		// rewriting — is the same work whether the result becomes a file or
		// stays in the caller's hands, and a second baker for the second case
		// would be a second copy of all of it. `Settings::Output` carries the
		// argument in full.
		void Emit(const Settings &settings, Report &report, Baked &baked, std::span<const std::byte> bytes) {
			baked.Bytes = bytes.size();

			if (settings.Output.empty()) {
				baked.Payload.assign(bytes.begin(), bytes.end());
				report.OutputBytes += bytes.size();
				return;
			}

			if (!WriteFile(settings.Output / baked.Output, bytes)) {
				baked.Failure = "cannot write";
				report.Failures++;
				return;
			}
			report.OutputBytes += bytes.size();
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
		// **The five keys, read in one pass rather than five.** A `.mat` is a
		// handful of lines and re-scanning it per key would be five parses of the
		// same text — but the real reason is that the keys are read *together*,
		// so a file naming `normal` twice resolves the same way as one naming
		// `colour` twice and there is one rule about which wins.
		struct MaterialKeys {
			std::string Colour;
			std::string Normal;
			std::string Roughness;
			std::string Occlusion;
			std::string Height;
			std::string Emissive;
		};

		MaterialKeys MaterialKeysOf(std::span<const std::byte> bytes) {
			MaterialKeys out;
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
				const std::string value(trim(line.substr(equals + 1)));

				// **Both spellings of colour, and one of everything else.** The
				// two colour spellings exist because the fetcher wrote `color`
				// and authors write both; there is no such history for the rest,
				// so inventing synonyms now would be four more things to keep
				// answering for.
				if (key == "color" || key == "colour") {
					out.Colour = value;
				} else if (key == "normal") {
					out.Normal = value;
				} else if (key == "roughness") {
					out.Roughness = value;
				} else if (key == "occlusion" || key == "ao") {
					out.Occlusion = value;
				} else if (key == "height") {
					out.Height = value;
				} else if (key == "emissive") {
					out.Emissive = value;
				}
			}
			return out;
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
		// **No output directory is a run that writes nothing**, not a run
		// rooted at the working directory — `Settings::Output` says so, and
		// creating "" here would have made that an empty path joined onto every
		// name instead.
		if (!settings.Output.empty()) {
			fs::create_directories(settings.Output, error);
			if (error) {
				failure = "assetc: cannot create " + settings.Output.string();
				return report;
			}
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

		// **After the sort, so a filtered run and a whole one agree about what
		// a source is.** Filtering the walk instead would work today and would
		// stop the moment something in the loop depended on a neighbour.
		if (!settings.Only.empty()) {
			const std::string wanted = Slashed(settings.Only);
			std::erase_if(sources, [&wanted](const std::string &source) { return source != wanted; });
			if (sources.empty()) {
				failure = "assetc: " + settings.Only + " is not in " + settings.Input.string();
				return report;
			}
		}

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

			// **Before the extension is dispatched on, so nothing decodes.** The
			// point of a content flag is that the parser is never reached: an
			// SVG refused here does not go through the rasteriser, and the
			// hostile-file cost charge in `bake/src/Svg.cpp` is a second line of
			// defence rather than the only one.
			const engine::assets::ContentForm form = engine::assets::FormOfName(relative);
			if (!settings.Content.Allows(form)) {
				baked.Failure =
					std::string("refused: ") + engine::assets::Describe(form) + " content is turned off";
				report.Refused++;
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
				const MaterialKeys keys = MaterialKeysOf(bytes);

				const size_t slash = relative.find_last_of('/');
				const std::string directory =
					slash == std::string::npos ? std::string() : relative.substr(0, slash);

				engine::assets::MaterialData material;

				// **Through the same `BakedName` a model's texture reference goes
				// through**, which is the whole reason that function is exported.
				// A material naming `Bricks_Color.png` and a baked tree holding
				// `Bricks_Color.atex` have to line up, and two spellings of the
				// rule is a material that resolves to nothing on a machine nobody
				// tested.
				//
				// **One rule for all five**, so a normal map outside the tree
				// fails exactly as a colour map does. Written as a loop over
				// pointers rather than five copies for that reason: five copies
				// is five places for the rule to drift.
				const std::pair<const std::string *, std::string *> maps[] = {
					{&keys.Colour, &material.ColourMap},
					{&keys.Normal, &material.NormalMap},
					{&keys.Roughness, &material.RoughnessMap},
					{&keys.Occlusion, &material.OcclusionMap},
					{&keys.Height, &material.HeightMap},
					{&keys.Emissive, &material.EmissiveMap},
				};

				for (const auto &[named, into] : maps) {
					if (named->empty()) {
						continue;
					}

					std::string resolved;
					if (Resolve(directory, *named, resolved)) {
						into->assign(BakedName(resolved));
					} else {
						// Refuse references outside the input tree, exactly as a
						// model's are refused. An untextured material is a real
						// state — `assets/Material.hpp` — so this is a material
						// that draws the default rather than a failed row.
						baked.Failure = "a map is outside the input tree";
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
				Emit(settings, report, baked, writer.Bytes());
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
				Emit(settings, report, baked, bytes);
				report.Assets.push_back(std::move(baked));
				continue;
			}

			const bool vector = image && IsVector(extension);

			engine::bake::Graph graph;
			engine::bake::NodeId source = graph.AddSource(relative, bytes);

			// **A drawing enters through `Rasterize` and everything else through
			// `Import`.** A zero target asks for the size the document itself
			// declares, which is the only size an SVG can be said to have —
			// `assetc` bakes a tree and has no per-file switches, so a caller who
			// wants a particular one builds the graph rather than walking a
			// directory.
			engine::bake::NodeId import =
				vector ? graph.AddRasterize(0, 0) : graph.Add(engine::bake::NodeKind::Import);
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

					if (vector) {
						// **Drawn again at the cap rather than box-filtered down
						// to it**, which is the whole reason the raster size is a
						// node parameter. Resampling would give edges belonging
						// to the filter instead of to the shapes, and a drawing
						// is the one input where the sharp answer is still
						// available.
						//
						// A second graph rather than a mutated one: a node's
						// parameters are fixed when it is added, and that is what
						// makes a graph a description of what a bake did.
						graph = engine::bake::Graph{};
						source = graph.AddSource(relative, bytes);
						import = graph.AddRasterize(width, height);
						graph.Connect(source, import);
						tail = import;

						if (!graph.Run(graphFailure)) {
							baked.Failure = graphFailure;
							report.Failures++;
							report.Assets.push_back(std::move(baked));
							continue;
						}
					} else {
						const engine::bake::NodeId resize = graph.AddResize(width, height);
						graph.Connect(tail, resize);
						tail = resize;
					}
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

					// **The resolver first, because it knows things the tree
					// cannot.** A flattened store has no `tex/` folder beside the
					// model — see `Settings::ResolveTexture` for what that broke
					// and for how the import log puts it back.
					const bool named = settings.ResolveTexture &&
									   settings.ResolveTexture(relative, submesh.Texture, resolved);

					if (!named && !Resolve(directory, submesh.Texture, resolved)) {
						// Refuse references outside the input tree.
						submesh.Texture.clear();
						continue;
					}

					// **Checked against the tree, and not checking is what let
					// this ship.** `Resolve` is purely lexical — it joins and
					// normalises and never asks whether the file is there — so a
					// reference to something that is not being baked became a
					// perfectly well-formed name for an asset that would never
					// exist. Nothing downstream can catch it: the publisher
					// signs whatever it is given, and the client's miss looks
					// exactly like a texture that has not streamed in yet.
					std::error_code missing;
					if (!std::filesystem::is_regular_file(settings.Input / resolved, missing)) {
						ENGINE_WARN(
							"bake: {}: texture '{}' resolves to '{}', which is not in the input tree — "
							"the submesh will draw untextured",
							relative,
							submesh.Texture,
							resolved
						);
						submesh.Texture.clear();
						report.DanglingTextures++;
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
				Emit(settings, report, baked, writer.Bytes());
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

			// **Last of the texture nodes, and the order is load-bearing.** Every
			// node above changes the pixels the levels are filtered from, and a
			// resize drops the chain outright — built before one, a texture would
			// reach disk with no levels and nothing saying why.
			if (image && settings.Mipmaps) {
				const engine::bake::NodeId mipmap = graph.Add(engine::bake::NodeKind::Mipmap);
				graph.Connect(tail, mipmap);
				tail = mipmap;
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
			Emit(settings, report, baked, exports.back().Bytes);
			report.Assets.push_back(std::move(baked));
		}

		return report;
	}
}
