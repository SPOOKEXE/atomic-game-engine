#include <engine/assets/HashTree.hpp>
#include <engine/assets/Manifest.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>

#include <algorithm>
#include <utility>

namespace engine::assets {

	namespace {
		// Bounds on what a parsed manifest may claim, so that a hostile count
		// cannot make a reader allocate before it has read anything.
		//
		// These are ceilings rather than expectations. A million assets is far
		// more than any game will publish and far less than the four billion a
		// 32-bit count could ask for, and the gap between those two numbers is
		// the whole attack.
		constexpr uint32_t MAXIMUM_ASSETS = 1'000'000;
		constexpr uint32_t MAXIMUM_BUNDLES = 1'000'000;
		constexpr uint32_t MAXIMUM_CHUNKS_PER_ASSET = 1'000'000;
		constexpr size_t MAXIMUM_NAME_BYTES = 1024;

		void WriteHash(core::ByteWriter &writer, const ContentHash &hash) {
			writer.WriteRaw(hash.Digest.data(), ContentHash::BYTES);
		}

		bool ReadHash(core::ByteReader &reader, ContentHash &hash) {
			return reader.ReadRaw(hash.Digest.data(), ContentHash::BYTES);
		}

		std::vector<ContentHash> ChunkHashes(const std::vector<ChunkEntry> &chunks) {
			std::vector<ContentHash> hashes;
			hashes.reserve(chunks.size());
			for (const ChunkEntry &chunk : chunks) {
				hashes.push_back(chunk.Hash);
			}
			return hashes;
		}
	}

	ContentHash Manifest::AddAsset(std::string name, std::vector<ChunkEntry> chunks) {
		AssetEntry entry;
		entry.Name = std::move(name);
		entry.Chunks = std::move(chunks);
		entry.Root = HashTree::RootOf(ChunkHashes(entry.Chunks));

		entry.TotalBytes = 0;
		for (const ChunkEntry &chunk : entry.Chunks) {
			entry.TotalBytes += chunk.Bytes;
		}

		const ContentHash root = entry.Root;

		// Sorted insert rather than push-and-sort-later: the order is part of
		// the format, so there is no window in which this object is in an order
		// the serialiser would not produce.
		const auto position = std::lower_bound(
			AssetsByName.begin(),
			AssetsByName.end(),
			entry.Name,
			[](const AssetEntry &candidate, const std::string &target) { return candidate.Name < target; }
		);

		if (position != AssetsByName.end() && position->Name == entry.Name) {
			// Publishing twice from one build should not leave two rows for one
			// name. Replacing is the only answer that keeps Find total.
			*position = std::move(entry);
		} else {
			AssetsByName.insert(position, std::move(entry));
		}

		return root;
	}

	std::optional<ContentHash> Manifest::AddBundle(std::span<const ContentHash> assetRoots) {
		if (assetRoots.empty()) {
			return std::nullopt;
		}

		BundleEntry bundle;
		bundle.Assets.assign(assetRoots.begin(), assetRoots.end());

		// A bundle is a set, so its identity must not depend on the order
		// somebody happened to add its members in.
		std::sort(bundle.Assets.begin(), bundle.Assets.end());
		bundle.Assets.erase(std::unique(bundle.Assets.begin(), bundle.Assets.end()), bundle.Assets.end());

		for (const ContentHash &root : bundle.Assets) {
			const AssetEntry *asset = FindByRoot(root);
			if (asset == nullptr) {
				// A bundle pointing at content this manifest does not describe
				// is unfetchable, and delivery time is far too late to find out.
				return std::nullopt;
			}
			bundle.TotalBytes += asset->TotalBytes;
		}

		bundle.Root = HashTree::RootOf(bundle.Assets);
		const ContentHash root = bundle.Root;

		const auto position = std::lower_bound(
			BundlesByRoot.begin(),
			BundlesByRoot.end(),
			bundle.Root,
			[](const BundleEntry &candidate, const ContentHash &target) { return candidate.Root < target; }
		);

		if (position != BundlesByRoot.end() && position->Root == bundle.Root) {
			// The same set of assets is the same bundle. Adding it twice is a
			// caller being careless, not a second group.
			return root;
		}

		BundlesByRoot.insert(position, std::move(bundle));
		return root;
	}

	ContentHash Manifest::Root() const {
		std::vector<ContentHash> roots;
		roots.reserve(BundlesByRoot.size());
		for (const BundleEntry &bundle : BundlesByRoot) {
			roots.push_back(bundle.Root);
		}
		return HashTree::RootOf(roots);
	}

	const AssetEntry *Manifest::Find(std::string_view name) const {
		const auto position = std::lower_bound(
			AssetsByName.begin(),
			AssetsByName.end(),
			name,
			[](const AssetEntry &candidate, std::string_view target) { return candidate.Name < target; }
		);
		if (position == AssetsByName.end() || position->Name != name) {
			return nullptr;
		}
		return &*position;
	}

	const AssetEntry *Manifest::FindByRoot(const ContentHash &root) const {
		// Assets are ordered by name rather than by root, so this is linear.
		// Deliberately: a second index would be a second thing to keep true,
		// and this is called while building a manifest rather than while
		// serving one. If it ever moves onto a serving path it wants an index,
		// and that is the moment to add one.
		const auto position =
			std::find_if(AssetsByName.begin(), AssetsByName.end(), [&root](const AssetEntry &asset) {
				return asset.Root == root;
			});
		return position == AssetsByName.end() ? nullptr : &*position;
	}

	void Manifest::Write(core::ByteWriter &writer) const {
		ENGINE_PROFILE("Manifest::Write");

		writer.WriteUInt32(MAGIC);
		writer.WriteUInt16(VERSION);

		writer.WriteUInt32(static_cast<uint32_t>(AssetsByName.size()));
		for (const AssetEntry &asset : AssetsByName) {
			writer.WriteString(asset.Name);
			WriteHash(writer, asset.Root);
			writer.WriteUInt64(asset.TotalBytes);
			writer.WriteUInt32(static_cast<uint32_t>(asset.Chunks.size()));
			for (const ChunkEntry &chunk : asset.Chunks) {
				WriteHash(writer, chunk.Hash);
				writer.WriteUInt32(chunk.Bytes);
			}
		}

		writer.WriteUInt32(static_cast<uint32_t>(BundlesByRoot.size()));
		for (const BundleEntry &bundle : BundlesByRoot) {
			WriteHash(writer, bundle.Root);
			writer.WriteUInt64(bundle.TotalBytes);
			writer.WriteUInt32(static_cast<uint32_t>(bundle.Assets.size()));
			for (const ContentHash &root : bundle.Assets) {
				WriteHash(writer, root);
			}
		}
	}

	std::optional<Manifest> Manifest::Read(core::ByteReader &reader) {
		ENGINE_PROFILE("Manifest::Read");

		// Every refusal below returns nothing rather than a partly built
		// manifest. A half-parsed manifest is the shape a caller uses by
		// accident, and this input arrives from an origin — repo_layout.md §1
		// says anyone can run one.
		// Marks the reader failed as well as returning nothing. ByteReader::Fail
		// exists for exactly this — a caller that has decided the contents are
		// wrong for a reason the reader cannot see — so that one flag carries
		// the verdict and a caller reading further from the same buffer cannot
		// miss it.
		const auto refuse = [&reader]() -> std::optional<Manifest> {
			reader.Fail();
			core::Metrics::Count("assets.manifest.refused", 1.0);
			return std::nullopt;
		};

		if (reader.ReadUInt32() != MAGIC) {
			return refuse();
		}
		if (reader.ReadUInt16() != VERSION) {
			// Refused rather than guessed at. A reader that carries on into a
			// version it does not know is a reader mis-parsing hostile bytes.
			return refuse();
		}

		Manifest manifest;

		const uint32_t assetCount = reader.ReadUInt32();
		if (assetCount > MAXIMUM_ASSETS) {
			return refuse();
		}
		manifest.AssetsByName.reserve(assetCount);

		for (uint32_t index = 0; index < assetCount; ++index) {
			AssetEntry asset;

			const std::string_view name = reader.ReadString();
			if (name.empty() || name.size() > MAXIMUM_NAME_BYTES) {
				return refuse();
			}
			asset.Name.assign(name);

			if (!ReadHash(reader, asset.Root)) {
				return refuse();
			}
			asset.TotalBytes = reader.ReadUInt64();

			const uint32_t chunkCount = reader.ReadUInt32();
			if (chunkCount > MAXIMUM_CHUNKS_PER_ASSET) {
				return refuse();
			}
			asset.Chunks.reserve(chunkCount);

			uint64_t measured = 0;
			for (uint32_t chunk = 0; chunk < chunkCount; ++chunk) {
				ChunkEntry entry;
				if (!ReadHash(reader, entry.Hash)) {
					return refuse();
				}
				entry.Bytes = reader.ReadUInt32();
				measured += entry.Bytes;
				asset.Chunks.push_back(entry);
			}

			// Two claims about one fact, so they are checked against each other
			// rather than one being trusted. A total that disagrees with its
			// chunks is a manifest that would size a buffer one way and fill it
			// another.
			if (measured != asset.TotalBytes) {
				return refuse();
			}

			// And the root is recomputed rather than believed. Taking the
			// written root on trust would let a manifest name content whose
			// chunks are something else entirely — which is the whole property
			// the hash tree exists to provide.
			if (HashTree::RootOf(ChunkHashes(asset.Chunks)) != asset.Root) {
				return refuse();
			}

			// Sorted by name is part of the format, so a file that is not
			// sorted is not this format. Accepting it would give two byte
			// sequences for one manifest and break the byte-stability
			// everything downstream caches on.
			if (index > 0 && !(manifest.AssetsByName.back().Name < asset.Name)) {
				return refuse();
			}

			manifest.AssetsByName.push_back(std::move(asset));
		}

		const uint32_t bundleCount = reader.ReadUInt32();
		if (bundleCount > MAXIMUM_BUNDLES) {
			return refuse();
		}
		manifest.BundlesByRoot.reserve(bundleCount);

		for (uint32_t index = 0; index < bundleCount; ++index) {
			BundleEntry bundle;
			if (!ReadHash(reader, bundle.Root)) {
				return refuse();
			}
			bundle.TotalBytes = reader.ReadUInt64();

			const uint32_t memberCount = reader.ReadUInt32();
			if (memberCount == 0 || memberCount > MAXIMUM_ASSETS) {
				return refuse();
			}
			bundle.Assets.reserve(memberCount);

			uint64_t measured = 0;
			for (uint32_t member = 0; member < memberCount; ++member) {
				ContentHash root;
				if (!ReadHash(reader, root)) {
					return refuse();
				}
				if (member > 0 && !(bundle.Assets.back() < root)) {
					return refuse();
				}

				const AssetEntry *asset = manifest.FindByRoot(root);
				if (asset == nullptr) {
					return refuse();
				}
				measured += asset->TotalBytes;
				bundle.Assets.push_back(root);
			}

			if (measured != bundle.TotalBytes) {
				return refuse();
			}
			if (HashTree::RootOf(bundle.Assets) != bundle.Root) {
				return refuse();
			}
			if (index > 0 && !(manifest.BundlesByRoot.back().Root < bundle.Root)) {
				return refuse();
			}

			manifest.BundlesByRoot.push_back(std::move(bundle));
		}

		// The reader reports its own overrun, and it has to be *asked*: every
		// Read above answers zero past the end rather than throwing, so a
		// truncated file would otherwise parse as a manifest of empty things.
		//
		// Failed() and not AtEnd(). Trailing bytes are legitimate here — a
		// signature follows the manifest in the published file — so leftover
		// input is the caller's business rather than evidence of a bad parse.
		if (reader.Failed()) {
			return refuse();
		}

		core::Metrics::Count("assets.manifest.parsed", 1.0);
		return manifest;
	}
}
