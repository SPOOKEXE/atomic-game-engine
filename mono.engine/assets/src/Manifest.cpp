#include <engine/assets/ContentHash.hpp>
#include <engine/assets/HashTree.hpp>
#include <engine/assets/Manifest.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>
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

		// What binds a name and a kind to the content they describe.
		//
		// Length-prefixed rather than concatenated, and that is the whole
		// reason this is a function rather than three appends at the call
		// site: `ab` + `c` and `a` + `bc` are the same bytes, so without a
		// length two different manifests could produce one descriptor and the
		// binding this exists to provide would not hold. The tag byte is the
		// same domain separation HashTree.hpp uses, for the same reason - a
		// descriptor must not be presentable as a chunk hash.
		ContentHash DescriptorOf(const AssetEntry &asset) {
			Hasher hasher;
			constexpr std::byte DESCRIPTOR_TAG{0x04};
			hasher.Update({&DESCRIPTOR_TAG, 1});

			const auto length = static_cast<uint64_t>(asset.Name.size());
			std::array<std::byte, 8> encoded{};
			for (size_t index = 0; index < encoded.size(); ++index) {
				encoded[index] = static_cast<std::byte>((length >> (index * 8)) & 0xFF);
			}
			hasher.Update(encoded);
			hasher.Update(std::as_bytes(std::span(asset.Name.data(), asset.Name.size())));

			const auto kind = static_cast<std::byte>(asset.Kind);
			hasher.Update({&kind, 1});
			hasher.Update(std::as_bytes(std::span(asset.Root.Digest.data(), asset.Root.Digest.size())));
			return hasher.Finish();
		}
	}

	bool VerifyAsset(const AssetEntry &asset, std::span<const std::byte> bytes) {
		if (bytes.size() != asset.TotalBytes) {
			return false;
		}

		std::vector<ContentHash> hashes;
		hashes.reserve(asset.Chunks.size());

		size_t offset = 0;
		for (const ChunkEntry &chunk : asset.Chunks) {
			if (offset + chunk.Bytes > bytes.size()) {
				return false;
			}
			// Each chunk against its own hash. Doing it per chunk rather than
			// over the whole is what lets a caller say *which* chunk was wrong,
			// and it is the only comparison the root's shape allows.
			if (Hasher::Of(bytes.subspan(offset, chunk.Bytes)) != chunk.Hash) {
				return false;
			}
			hashes.push_back(chunk.Hash);
			offset += chunk.Bytes;
		}

		// And the tree, so a run of chunks that each verified cannot be passed
		// off as a different asset made of the same pieces in another order.
		return offset == bytes.size() && HashTree::RootOf(hashes) == asset.Root;
	}

	ContentHash Manifest::AddAsset(std::string name, AssetKind kind, std::vector<ChunkEntry> chunks) {
		AssetEntry entry;
		entry.Name = std::move(name);
		entry.Kind = kind;
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

	ContentHash Manifest::DescriptorRoot() const {
		std::vector<ContentHash> descriptors;
		descriptors.reserve(AssetsByName.size());
		// In name order, which is the order the assets are already held in, so
		// the canonical arrangement of the file and the arrangement of the hash
		// are one thing rather than two that could drift.
		for (const AssetEntry &asset : AssetsByName) {
			descriptors.push_back(DescriptorOf(asset));
		}
		return HashTree::RootOf(descriptors);
	}

	ContentHash Manifest::BundleRoot() const {
		std::vector<ContentHash> roots;
		roots.reserve(BundlesByRoot.size());
		for (const BundleEntry &bundle : BundlesByRoot) {
			roots.push_back(bundle.Root);
		}
		return HashTree::RootOf(roots);
	}

	ContentHash Manifest::Root() const {
		// Two leaves, always both present, so an empty manifest still has a
		// root that is not the root of anything else.
		const std::array<ContentHash, 2> halves{DescriptorRoot(), BundleRoot()};
		return HashTree::RootOf(halves);
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

	const BundleEntry *Manifest::BundleFor(const ContentHash &assetRoot) const {
		for (const BundleEntry &bundle : BundlesByRoot) {
			// Members are sorted, so this is a binary search per bundle rather
			// than a scan. The bundle list itself is walked: an asset is in one
			// bundle and there is no index from asset to bundle in the format,
			// because building one would be a second structure to keep true.
			if (std::binary_search(bundle.Assets.begin(), bundle.Assets.end(), assetRoot)) {
				return &bundle;
			}
		}
		return nullptr;
	}

	std::optional<BundleSlice>
	Manifest::SliceOf(const BundleEntry &bundle, const ContentHash &assetRoot) const {
		uint64_t offset = 0;
		for (const ContentHash &member : bundle.Assets) {
			const AssetEntry *const asset = FindByRoot(member);
			if (asset == nullptr) {
				// A bundle naming an asset this manifest does not describe
				// cannot be cut up at all, and `AddBundle` and `Read` both
				// refuse to build one. Returning nothing rather than a partial
				// answer keeps that a refusal rather than a wrong offset.
				return std::nullopt;
			}
			if (member == assetRoot) {
				return BundleSlice{.Offset = offset, .Bytes = asset->TotalBytes};
			}
			offset += asset->TotalBytes;
		}
		return std::nullopt;
	}

	std::vector<const AssetEntry *> Manifest::OfKind(AssetKind kind) const {
		std::vector<const AssetEntry *> matching;
		for (const AssetEntry &asset : AssetsByName) {
			if (asset.Kind == kind) {
				matching.push_back(&asset);
			}
		}
		return matching;
	}

	void Manifest::Write(core::ByteWriter &writer) const {
		ENGINE_PROFILE_CAT("Manifest::Write", core::ProfileCategory::Assets);

		writer.WriteUInt32(MAGIC);
		writer.WriteUInt16(VERSION);

		writer.WriteUInt32(static_cast<uint32_t>(AssetsByName.size()));
		for (const AssetEntry &asset : AssetsByName) {
			writer.WriteString(asset.Name);
			writer.WriteUInt8(static_cast<uint8_t>(asset.Kind));
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
		ENGINE_PROFILE_CAT("Manifest::Read", core::ProfileCategory::Assets);

		// Every refusal below returns nothing rather than a partly built
		// manifest. A half-parsed manifest is the shape a caller uses by
		// accident, and this input arrives from an origin - repo_layout.md §1
		// says anyone can run one.
		// Marks the reader failed as well as returning nothing. ByteReader::Fail
		// exists for exactly this - a caller that has decided the contents are
		// wrong for a reason the reader cannot see - so that one flag carries
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

			// An unknown kind is read as `Unknown` rather than refused. The
			// list is append-only, so a manifest from a later build naming a
			// kind this one has not heard of is a legitimate document: its
			// content still delivers and verifies, and it is simply not
			// something a kind-filtered request here will return. Refusing
			// would make every kind added later a hard break for every client
			// already deployed.
			const uint8_t kind = reader.ReadUInt8();
			asset.Kind = kind <= static_cast<uint8_t>(AssetKind::Data) ? static_cast<AssetKind>(kind)
																	   : AssetKind::Unknown;

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
			// chunks are something else entirely - which is the whole property
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
		// Failed() and not AtEnd(). Trailing bytes are legitimate here - a
		// signature follows the manifest in the published file - so leftover
		// input is the caller's business rather than evidence of a bad parse.
		if (reader.Failed()) {
			return refuse();
		}

		core::Metrics::Count("assets.manifest.parsed", 1.0);
		return manifest;
	}
}
