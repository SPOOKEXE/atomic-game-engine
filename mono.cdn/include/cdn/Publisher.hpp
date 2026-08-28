#pragma once

// Turning a directory of files into content an origin can serve.
//
// This is the *build* side, and keeping it apart from the request path is the
// whole reason `cdn::Gate` takes a content hash and never a path. Here names are
// real: a file is walked, read, chunked, hashed and given a kind. After here
// there are only hashes, and a hash cannot be walked out of a directory.
//
// What it does, in order, because the order matters:
//
// 1. **Walk and chunk.** Every file under the content directory becomes an
//    asset, cut into content-defined chunks and written into the store. Dedup
//    falls out for free - two files sharing bytes share chunks, and the second
//    write is a no-op.
// 2. **Name and classify.** `KindOfName` decides what each asset is, once,
//    here. Nothing downstream re-derives it.
// 3. **Group.** `Grouper` applies self-sufficiency, then the size mix, then
//    priority. Groups are what get compressed and streamed.
// 4. **Train a dictionary** over a sample of the content, because a game is
//    thousands of small files sharing a great deal and without one they
//    compress as thousands of independently mediocre blobs.
// 5. **Sign the manifest root**, once, at the top.
//
// **The signing key is the caller's and is used here and nowhere else.**
// `assets/AGENTS.md` records the convention the build cannot enforce: signing
// is the studio's and the CLI's, and a client, a server tick or a serving path
// calling `SigningKey` is a review failure. This function is the CLI half of
// that, which is why publishing is a separate call from serving rather than
// something an origin does when it starts.
//
// **Affinity is derived from the directory a file sits in.** A group has to be
// independently useful, and without an authoring tool to declare
// what belongs with what, the directory is the best available statement of it:
// somebody who put a mesh and its textures in one folder was saying they go
// together. Stated plainly because it is a heuristic standing in for a decision
// the import pipeline will eventually make properly.
//
// @tier shared

#include <engine/assets/Chunker.hpp>
#include <engine/assets/ContentHash.hpp>
#include <engine/assets/ContentPolicy.hpp>
#include <engine/assets/Signature.hpp>

#include <cdn/Grouper.hpp>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>

namespace cdn {

	// How a publish is tuned.
	//
	// @since v0.9
	struct PublishSettings {
		// How files are cut into chunks.
		engine::assets::ChunkLimits Chunking;

		// The size envelope groups are assembled within.
		GroupPolicy Grouping;

		// Whether to train a compression dictionary over the content.
		//
		// On by default. Zstd refuses when it has too little to learn from, and
		// that refusal is passed through rather than papered over - a
		// dictionary trained on too little costs bytes on every fetch and buys
		// nothing.
		bool TrainDictionary = true;

		// The largest dictionary to produce.
		size_t DictionaryBytes = 110 * 1024;

		// Which content forms may enter the publication.
		//
		// **The origin's only honest gate, and the reason there is no
		// serve-time one.** Step 2 above is where a name is real; after it
		// there are only hashes, and a hash cannot be walked back to an
		// extension. So a form refused here never becomes a chunk, never
		// appears in the manifest and is never served - where a serve-time
		// check would be looking at a hash that has already been decided.
		//
		// **Defaulted from the process's own settings**, so an origin is gated
		// by one line here rather than by every caller of `Publish` remembering
		// to fill it. A program that never declared the flags gets everything,
		// which is what this engine did before they existed.
		//
		// @since v0.15
		engine::assets::ContentPolicy Content =
			engine::assets::ContentPolicy::Process(engine::assets::ContentVerb::Publish);
	};

	// What a publish produced.
	//
	// @since v0.9
	struct PublishReport {
		// Files that became assets.
		size_t Assets = 0;

		// Distinct chunks in the store afterwards.
		size_t Chunks = 0;

		// Delivery groups.
		size_t Bundles = 0;

		// What the content weighs, uncompressed.
		uint64_t ContentBytes = 0;

		// What the chunk store weighs. **Smaller than `ContentBytes` when
		// content deduplicated**, which is the number that says whether
		// content addressing earned its keep on this content.
		uint64_t StoredBytes = 0;

		// Groups that exceeded the size ceiling because one affinity did.
		//
		// Reported rather than hidden: a bound quietly broken reads as a bound
		// that held, and the first anyone hears of it is a client stalling on a
		// group it cannot stream in time.
		size_t Oversized = 0;

		// Whether a dictionary was trained. False is ordinary on small content.
		bool DictionaryTrained = false;

		// Files `PublishSettings::Content` refused.
		//
		// **Reported for `Oversized`'s reason**: a publish that quietly produced
		// fewer assets than the directory holds is a client fetching a name that
		// is not there, and the first anyone hears of it is a missing texture.
		//
		// @since v0.15
		size_t Refused = 0;

		// The signed manifest root.
		engine::assets::ContentHash Root;
	};

	// Publishes a directory of files into a content store.
	//
	// The store is left ready to serve: chunks, a signed manifest and a
	// dictionary if one was trained. An existing store is added to rather than
	// replaced, which is what makes republishing cheap - unchanged content is
	// already there and every write of it is a no-op.
	//
	// @param contentDirectory The files to publish. Walked recursively; names
	//        in the manifest are relative to it, with forward slashes, so the
	//        same content published on two platforms produces one manifest.
	// @param storeDirectory Where to write the store. Created if missing.
	// @param key The key to sign the manifest root with.
	// @param settings How to chunk, group and compress.
	// @return What was published, or nothing when the content directory could
	//         not be read or the store could not be written.
	// @since v0.9
	std::optional<PublishReport> Publish(
		const std::filesystem::path &contentDirectory,
		const std::filesystem::path &storeDirectory,
		const engine::assets::SigningKey &key,
		const PublishSettings &settings = {}
	);
}
