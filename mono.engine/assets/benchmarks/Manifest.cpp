// The manifest's lookups, and the joins that are built out of them.
//
// **`Content.cpp` and `Store.cpp` measure bytes; this measures the index over
// them.** Nothing here hashes content. Every row is a search through a list of
// assets or bundles, which is the part of the format a frame pays for after the
// download has landed: a delivery client cutting a group into its members walks
// this structure once per member, inside `Pump`, at the tick barrier.
//
// **The shape that matters is how a row grows with the manifest, not its
// absolute figure.** A game with four thousand assets and a game with four
// hundred are the same code, and a row that is linear in the asset count is
// invisible at the second size and is a frame at the first. The `AddBundle` and
// the split rows are the two joins where that difference lives, and they are
// stated per asset so that doubling `ASSETS` and re-running says directly
// whether the cost per asset is flat.
//
// **The sizes are a real game's, not a stress test's.** Four thousand assets in
// bundles of thirty-two is what `mono.cdn`'s grouping produces for a project of
// a few gigabytes, and `cdn.bench.grouping` already measures ten thousand on the
// origin side. The origin has no tick and can afford a slow join; this side
// cannot, which is why the numbers are taken here as well.

#include <engine/assets/AssetKind.hpp>
#include <engine/assets/ContentHash.hpp>
#include <engine/assets/Manifest.hpp>
#include <engine/core/Bytes.hpp>
#include <engine/testing/Bench.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

TEST_SUITE_ID("engine.assets.bench.manifest")

using engine::assets::AssetEntry;
using engine::assets::AssetKind;
using engine::assets::BundleEntry;
using engine::assets::BundleSlice;
using engine::assets::ChunkEntry;
using engine::assets::ContentHash;
using engine::assets::Manifest;
using engine::testing::Consume;

namespace manifest_bench {
	// Assets in the manifest under measurement.
	constexpr size_t ASSETS = 4096;

	// Assets per bundle. `mono.cdn`'s grouping targets a group of a few
	// megabytes, which for ordinary meshes and textures lands here.
	constexpr size_t MEMBERS = 32;

	// Bundles that follows from the two above.
	constexpr size_t BUNDLES = ASSETS / MEMBERS;

	// Chunks per asset. Four 64 KiB chunks is a quarter-megabyte asset, and the
	// count matters here only because it decides how long the chunk list a
	// lookup skips over is.
	constexpr size_t CHUNKS = 4;

	// One asset's name and chunk list, without any content behind them.
	//
	// The chunk hashes are generated rather than hashed: the manifest never
	// looks at a chunk's bytes, so producing real ones would measure BLAKE3 in
	// the setup of every sample and nothing in the rows.
	struct Content {
		std::string Name;
		std::vector<ChunkEntry> Chunks;
	};

	// A hash that is distinct per `seed` and looks nothing like its neighbours,
	// so a binary search over roots gets no help from the generator.
	ContentHash Scrambled(uint64_t seed) {
		ContentHash hash;
		uint64_t state = seed * 6364136223846793005ull + 1442695040888963407ull;
		for (size_t byte = 0; byte < hash.Digest.size(); byte++) {
			state = state * 6364136223846793005ull + 1442695040888963407ull;
			hash.Digest[byte] = static_cast<uint8_t>(state >> 33);
		}
		return hash;
	}

	// The assets a manifest will be built from, in the order a publisher hands
	// them over.
	//
	// **Not in name order.** `AddAsset` inserts at a sorted position, so a
	// publisher that happened to walk its directory alphabetically would append
	// every time and never move anything. A real directory walk does not, and
	// the scrambled index below is what stops the row measuring the easy case.
	const std::vector<Content> &Prepared() {
		static const std::vector<Content> built = [] {
			std::vector<Content> content;
			content.reserve(ASSETS);
			for (size_t asset = 0; asset < ASSETS; asset++) {
				// A multiplier coprime with ASSETS, so the sequence is a
				// permutation of every index rather than a subset.
				const size_t scattered = (asset * 2654435761ull) % ASSETS;
				Content one;
				one.Name = "content/asset-" + std::to_string(scattered) + ".amesh";
				one.Chunks.reserve(CHUNKS);
				for (size_t chunk = 0; chunk < CHUNKS; chunk++) {
					one.Chunks.push_back(
						ChunkEntry{
							.Hash = Scrambled(scattered * CHUNKS + chunk),
							.Bytes = 64 * 1024,
						}
					);
				}
				content.push_back(std::move(one));
			}
			return content;
		}();
		return built;
	}

	// Every asset added, and nothing bundled.
	Manifest Assembled() {
		Manifest manifest;
		for (const Content &one : Prepared()) {
			manifest.AddAsset(one.Name, AssetKind::Mesh, one.Chunks);
		}
		return manifest;
	}

	// Every asset added, built once, for the rows that want a manifest with no
	// bundles in it without paying to build one.
	const Manifest &AssetsOnly() {
		static const Manifest built = Assembled();
		return built;
	}

	// Groups assets into bundles, striding through the manifest.
	//
	// **Strided rather than consecutive, and that is the whole honesty of the
	// `Split` row.** A group is a mesh with its textures and its material -
	// assets that are useful together and are not adjacent in name order. Taking
	// the first thirty-two entries instead would put every member of the
	// measured bundle at the front of the asset list, where a linear search
	// finds them immediately: the row would then say a scan costs almost
	// nothing, which is true only of the bundle nobody fetches.
	void Bundle(Manifest &manifest) {
		std::vector<ContentHash> members;
		members.reserve(MEMBERS);
		for (size_t bundle = 0; bundle < BUNDLES; bundle++) {
			members.clear();
			for (size_t member = 0; member < MEMBERS; member++) {
				members.push_back(manifest.Assets()[bundle + member * BUNDLES].Root);
			}
			manifest.AddBundle(members);
		}
	}

	// A finished manifest, built once and shared by every lookup row.
	const Manifest &Built() {
		static const Manifest built = [] {
			Manifest manifest = AssetsOnly();
			Bundle(manifest);
			return manifest;
		}();
		return built;
	}

	// The same manifest as bytes, for the parse row.
	const std::vector<std::byte> &Written() {
		static const std::vector<std::byte> built = [] {
			engine::core::ByteWriter writer;
			Built().Write(writer);
			const std::span<const std::byte> bytes = writer.Bytes();
			return std::vector<std::byte>(bytes.begin(), bytes.end());
		}();
		return built;
	}
}

using namespace manifest_bench;

// --- building -----------------------------------------------------------------

BENCH_PER_ITEM("Manifest::AddAsset · 4096 assets in publish order", ASSETS) {
	// The sorted insert, and the hash tree over four chunks that comes with it.
	// This row is what a publisher pays and no frame ever does.
	Manifest manifest = Assembled();
	Consume(manifest.Assets().size());
}

BENCH_PER_ITEM("Manifest::AddBundle · 4096 assets into 128 bundles of 32", ASSETS) {
	// **The first of the two joins.** `AddBundle` resolves every member root
	// back to its asset to total the bundle's bytes, so the whole pass does one
	// root lookup per asset in the manifest. Divided by `ASSETS` as it is here,
	// a flat figure means the lookup is logarithmic and a figure that doubles
	// when `ASSETS` doubles means it is linear.
	//
	// The manifest is **copied** rather than rebuilt, so the row is the
	// bundling and not the four thousand sorted inserts above it.
	Manifest manifest = AssetsOnly();
	Bundle(manifest);
	Consume(manifest.Bundles().size());
}

BENCH_PER_ITEM("Manifest::Read · 4096 assets, 128 bundles", ASSETS) {
	// Parsing does the same join: every bundle member is resolved against the
	// assets already read, because a bundle naming content the manifest does not
	// describe is refused at parse time rather than at delivery time. A client
	// pays this once per manifest, on the frame the catalogue lands.
	engine::core::ByteReader reader(Written());
	const std::optional<Manifest> parsed = Manifest::Read(reader);
	Consume(parsed.has_value() ? parsed->Assets().size() : 0);
}

// --- the fetch path -----------------------------------------------------------

BENCH_PER_ITEM("Split · locating all 32 members of one bundle", MEMBERS) {
	// **The second join, and the one that is inside the tick barrier.**
	// `delivery::Client::Split` runs exactly this per member of an arriving
	// group: resolve the member to its asset, then ask where it sits in the
	// payload. `SliceOf` walks the bundle from its first member adding lengths,
	// resolving each one it passes, so the pass over a whole bundle resolves
	// members quadratically in the bundle's size and each resolution searches
	// the manifest.
	//
	// A group of forty meshes arriving in one frame is the case
	// `delivery/AGENTS.md` describes under `IntakeBudget`, so this row is
	// frame time rather than background work.
	const Manifest &manifest = Built();
	const BundleEntry &bundle = manifest.Bundles().front();
	uint64_t offsets = 0;
	for (const ContentHash &member : bundle.Assets) {
		const AssetEntry *const asset = manifest.FindByRoot(member);
		const std::optional<BundleSlice> slice = manifest.SliceOf(bundle, member);
		offsets += slice ? slice->Offset : 0;
		offsets += asset != nullptr ? asset->TotalBytes : 0;
	}
	Consume(offsets);
}

BENCH_PER_ITEM("Manifest::FindByRoot · 4096 hits", ASSETS) {
	// The lookup both joins are built from, on its own.
	const Manifest &manifest = Built();
	size_t found = 0;
	for (const AssetEntry &asset : manifest.Assets()) {
		found += manifest.FindByRoot(asset.Root) != nullptr ? 1 : 0;
	}
	Consume(found);
}

BENCH_PER_ITEM("Manifest::FindByRoot · 4096 misses", ASSETS) {
	// A miss has to walk everything a hit might stop early in, so this is the
	// row that does not flatter a linear search. A client resolving a request
	// by root against a stale catalogue takes this path.
	const Manifest &manifest = Built();
	size_t found = 0;
	for (size_t asset = 0; asset < ASSETS; asset++) {
		found += manifest.FindByRoot(Scrambled(asset + ASSETS * 64)) != nullptr ? 1 : 0;
	}
	Consume(found);
}

BENCH_PER_ITEM("Manifest::BundleFor · 4096 hits", ASSETS) {
	// What a fetch needs: the asset was asked for, the bundle is what travels.
	// Called once per request in `Resolve` and once per pending request in
	// `AnyWaiting` and `Abandon`, all three inside `Pump`.
	const Manifest &manifest = Built();
	size_t found = 0;
	for (const AssetEntry &asset : manifest.Assets()) {
		found += manifest.BundleFor(asset.Root) != nullptr ? 1 : 0;
	}
	Consume(found);
}

BENCH_PER_ITEM("Manifest::FindBundle · 128 lookups by bundle root", BUNDLES) {
	// A bundle by its own root, which is what a job in flight has and what a
	// relay is asked for by route. Both were linear scans over the bundle list
	// until v0.19, written out twice, and the bundle list has been sorted by
	// root all along.
	const Manifest &manifest = Built();
	size_t found = 0;
	for (const BundleEntry &bundle : manifest.Bundles()) {
		found += manifest.FindBundle(bundle.Root) != nullptr ? 1 : 0;
	}
	Consume(found);
}

BENCH_PER_ITEM("Control · 128 bundle lookups as a scan over the list", BUNDLES) {
	// **Not a measurement of this code, and that is what makes it useful.** The
	// loop `delivery::Client` and `delivery::Relay` each held before v0.19,
	// against the binary search that replaced both. The bundle list was sorted
	// by root the whole time.
	const Manifest &manifest = Built();
	size_t found = 0;
	for (const BundleEntry &wanted : manifest.Bundles()) {
		for (const BundleEntry &bundle : manifest.Bundles()) {
			if (bundle.Root == wanted.Root) {
				++found;
				break;
			}
		}
	}
	Consume(found);
}

BENCH_PER_ITEM("Manifest::Find · 4096 hits by name", ASSETS) {
	// **The control.** Assets are stored in name order, so this has always been
	// a binary search. Any root-addressed row costing far more than this one is
	// paying for the ordering rather than for the lookup.
	const Manifest &manifest = Built();
	size_t found = 0;
	for (const AssetEntry &asset : manifest.Assets()) {
		found += manifest.Find(asset.Name) != nullptr ? 1 : 0;
	}
	Consume(found);
}
