#include <engine/assets/AssetKind.hpp>
#include <engine/assets/ChunkStore.hpp>
#include <engine/assets/Manifest.hpp>
#include <engine/core/Log.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/delivery/GroupCodec.hpp>

#include <algorithm>
#include <cdn/Publisher.hpp>
#include <fstream>
#include <map>
#include <string>
#include <system_error>
#include <vector>

namespace cdn {
	namespace {
		namespace fs = std::filesystem;
		using engine::assets::AssetKind;
		using engine::assets::ChunkEntry;
		using engine::assets::Chunker;
		using engine::assets::ChunkSpan;
		using engine::assets::ChunkStore;
		using engine::assets::ContentHash;
		using engine::assets::Hasher;
		using engine::assets::Manifest;

		// How many small files are sampled to train the dictionary.
		//
		// Zstd wants a reasonable number of reasonably similar samples. Bounded
		// so that publishing a large game does not read all of it twice.
		constexpr size_t DICTIONARY_SAMPLES = 512;

		// The largest file sampled for training. A dictionary exists to capture
		// what many small files share; a large one would dominate the sample
		// and teach it about a single asset.
		constexpr size_t DICTIONARY_SAMPLE_BYTES = 64 * 1024;

		std::optional<std::vector<std::byte>> ReadWholeFile(const fs::path &path) {
			std::ifstream file(path, std::ios::binary | std::ios::ate);
			if (!file) {
				return std::nullopt;
			}
			const std::streamoff size = file.tellg();
			if (size < 0) {
				return std::nullopt;
			}
			file.seekg(0);

			std::vector<std::byte> bytes(static_cast<size_t>(size));
			if (size > 0) {
				file.read(reinterpret_cast<char *>(bytes.data()), size);
				if (!file) {
					return std::nullopt;
				}
			}
			return bytes;
		}

		// A path relative to the content root, with forward slashes.
		//
		// **Always forward slashes**, whatever the platform separator is. A
		// manifest is content addressed by name as well as by hash, so the same
		// files published on Windows and on Linux have to produce one manifest
		// — otherwise the two builds share no cache entries and nothing says
		// why.
		std::string ContentName(const fs::path &root, const fs::path &file) {
			const fs::path relative = fs::relative(file, root);
			std::string name = relative.generic_string();
			return name;
		}

		// What a file is needed *with*, standing in for a decision the import
		// pipeline will eventually make properly.
		//
		// The directory, hashed to a number. Somebody who put a mesh and its
		// textures in one folder was saying they belong together, and that is
		// the best statement of affinity available before there is an authoring
		// tool that declares one.
		//
		// **Zero is never returned for a real directory**, because zero means
		// "belongs with nothing in particular" to `Grouper` and would bind
		// every unrelated asset in the game into one lump.
		uint32_t AffinityOf(const std::string &name) {
			const size_t slash = name.find_last_of('/');
			if (slash == std::string::npos) {
				// Loose at the root: genuinely "with nothing in particular".
				return 0;
			}
			const std::string_view directory(name.data(), slash);
			const ContentHash hash = Hasher::Of(
				std::span<const std::byte>(
					reinterpret_cast<const std::byte *>(directory.data()), directory.size()
				)
			);
			uint32_t value = 0;
			for (size_t index = 0; index < 4; ++index) {
				value = (value << 8) | hash.Digest[index];
			}
			// Fold zero away rather than letting one directory in four billion
			// mean "unbound".
			return value == 0 ? 1u : value;
		}
	}

	std::optional<PublishReport> Publish(
		const fs::path &contentDirectory,
		const fs::path &storeDirectory,
		const engine::assets::SigningKey &key,
		const PublishSettings &settings
	) {
		ENGINE_PROFILE_CAT("cdn::Publish", engine::core::ProfileCategory::Assets);

		std::error_code failure;
		if (!fs::is_directory(contentDirectory, failure)) {
			ENGINE_ERROR("cdn: nothing to publish — {} is not a directory", contentDirectory.string());
			return std::nullopt;
		}

		std::optional<ChunkStore> store = ChunkStore::Open(storeDirectory, true);
		if (!store) {
			ENGINE_ERROR("cdn: cannot write a store at {}", storeDirectory.string());
			return std::nullopt;
		}

		PublishReport report;
		Manifest manifest;
		const Chunker chunker(settings.Chunking);

		// Gathered and sorted before anything is written, so two publishes of
		// one directory do the same work in the same order. The manifest is
		// byte-stable by construction, but the *store* writes and the
		// dictionary sample are not unless the walk order is fixed — and a
		// dictionary that differs run to run makes every prepared group a
		// different artefact.
		std::vector<fs::path> files;
		for (const auto &entry : fs::recursive_directory_iterator(contentDirectory, failure)) {
			if (entry.is_regular_file(failure)) {
				files.push_back(entry.path());
			}
		}
		std::sort(files.begin(), files.end());

		std::vector<GroupCandidate> candidates;
		std::vector<std::vector<std::byte>> samples;

		for (const fs::path &file : files) {
			std::optional<std::vector<std::byte>> bytes = ReadWholeFile(file);
			if (!bytes) {
				ENGINE_WARN("cdn: skipped {} — could not be read", file.string());
				continue;
			}
			if (bytes->empty()) {
				// An empty file has no chunks, so it would be an asset with an
				// empty root — a row in the manifest that fetches nothing.
				// Skipped, and said so, rather than published as a thing that
				// cannot be delivered.
				ENGINE_WARN("cdn: skipped {} — empty", file.string());
				continue;
			}

			const std::string name = ContentName(contentDirectory, file);

			std::vector<ChunkEntry> chunks;
			for (const ChunkSpan &span : chunker.Split(*bytes)) {
				const std::span<const std::byte> piece(bytes->data() + span.Offset, span.Bytes);
				const ContentHash hash = Hasher::Of(piece);
				if (!store->Write(hash, piece)) {
					ENGINE_ERROR("cdn: could not store a chunk of {}", name);
					return std::nullopt;
				}
				chunks.push_back(ChunkEntry{.Hash = hash, .Bytes = span.Bytes});
			}

			const ContentHash root = manifest.AddAsset(name, engine::assets::KindOfName(name), chunks);

			candidates.push_back(
				GroupCandidate{
					.Root = root,
					.Bytes = static_cast<uint64_t>(bytes->size()),
					.Affinity = AffinityOf(name),
					.Priority = 0,
				}
			);

			report.ContentBytes += bytes->size();
			++report.Assets;

			if (settings.TrainDictionary && samples.size() < DICTIONARY_SAMPLES &&
				bytes->size() <= DICTIONARY_SAMPLE_BYTES) {
				samples.push_back(*bytes);
			}
		}

		if (report.Assets == 0) {
			ENGINE_ERROR("cdn: {} holds nothing publishable", contentDirectory.string());
			return std::nullopt;
		}

		const Grouper grouper(settings.Grouping);
		const Assembly assembly = grouper.Assemble(candidates);
		report.Oversized = assembly.Oversized;

		for (const Group &group : assembly.Groups) {
			if (!manifest.AddBundle(group.Assets)) {
				ENGINE_ERROR("cdn: a group named content the manifest does not describe");
				return std::nullopt;
			}
			++report.Bundles;
		}

		if (settings.TrainDictionary && !samples.empty()) {
			std::vector<std::span<const std::byte>> views;
			views.reserve(samples.size());
			for (const std::vector<std::byte> &sample : samples) {
				views.push_back(sample);
			}
			if (auto dictionary = engine::delivery::Dictionary::Train(views, settings.DictionaryBytes)) {
				if (store->WriteDictionary(dictionary->Bytes())) {
					report.DictionaryTrained = true;
				}
			} else {
				// Ordinary on small content, and worth saying rather than
				// leaving somebody to wonder why the ratio is poor.
				ENGINE_INFO("cdn: no dictionary — too little content to train one on");
			}
		}

		// Signed last, over everything above. The root covers the descriptor
		// table as well as the bundles, so this one signature binds the content,
		// its names and its kinds together.
		report.Root = manifest.Root();
		if (!store->WriteManifest(manifest, key.SignManifestRoot(report.Root))) {
			ENGINE_ERROR("cdn: could not write the manifest");
			return std::nullopt;
		}

		report.Chunks = store->Count();
		report.StoredBytes = store->Bytes();
		return report;
	}
}
