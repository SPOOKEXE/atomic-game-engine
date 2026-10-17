#pragma once

// Shared on-disk layout for locally authored and published content.
//
// `raw/` keeps author-named source files, `baked/` holds runtime formats and
// `processed/` holds content-addressed chunks, groups and a manifest. Raw files
// may be nested and are named relative to `raw/` when published.
//
// This module owns the layout, not baking or publishing policy. The append-only
// log is for people to inspect and is never used as a content index.
//
// @tier shared

#include <engine/assets/AssetKind.hpp>
#include <engine/assets/ContentHash.hpp>
#include <engine/assets/Signature.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace engine::assets {

	// Where the local store lives, and what it is called.
	//
	// @since v0.10
	struct LocalPaths {
		// The folder holding the other two and the log.
		std::filesystem::path Root;

		// What a person put in.
		std::filesystem::path Raw;

		// The same content in the formats a runtime reads. What gets published.
		//
		// @since v0.10
		std::filesystem::path Baked;

		// What the engine reads.
		std::filesystem::path Processed;

		// The record of what happened.
		std::filesystem::path Log;
	};

	// The default user-visible workspace location.
	//
	// `~/Documents/atomic-game-engine/cdn` is intentionally not a platform data
	// directory because authors work with these files directly.
	//
	// **The home directory is read from the environment and not assumed.** A
	// process with no `HOME` - a container, a service - falls back to the current
	// directory rather than to `/`, so a store is created somewhere writable
	// rather than failing at the first import.
	//
	// @return The four paths. Nothing is created; see `EnsureLocalStore`.
	LocalPaths DefaultLocalPaths();

	// The same, rooted somewhere else.
	//
	// **For tests, and for a machine with content on another disk.** Every
	// function here takes paths rather than finding them, so the default is one
	// caller's choice rather than a global - which is what lets a suite build a
	// store in a temporary directory without touching the developer's own.
	//
	// @param root Where the store lives.
	// @return The four paths under it.
	LocalPaths LocalPathsUnder(const std::filesystem::path &root);

	// Creates the folders if they are not there.
	//
	// Idempotent. Creating an existing store is not an error, which is what lets
	// every program call this at startup without asking first.
	//
	// @param paths Where to create.
	// @return `false` when a folder could not be created.
	bool EnsureLocalStore(const LocalPaths &paths);

	// What one entry in the log says.
	//
	// @since v0.10
	struct LogEntry {
		// Seconds since the Unix epoch.
		//
		// **Passed in by the caller and never read from a clock here**, which is
		// `assets::Grant`'s standing rule and `cdn::Service::Pump`'s: a module
		// that read the time would hold a notion of "now" of its own to drift,
		// and a test could not pin it.
		uint64_t Seconds = 0;

		// What happened: `import` or `publish`.
		std::string Action;

		// The name in `raw/`, or the manifest root for a publish.
		std::string Subject;

		// The content hash, as hex. Empty when there is none.
		std::string Hash;

		// How many bytes it was.
		uint64_t Bytes = 0;
	};

	// Appends one line to the log.
	//
	// **Appends and never rewrites**, so two programs writing at once interleave
	// lines rather than losing each other's - which is the whole reason this is a
	// text log rather than a document. It is not a database and must not become
	// one: the moment something *reads* it to make a decision, the folder has
	// stopped being the index.
	//
	// @param paths The store.
	// @param entry What to record.
	// @return `false` when the file could not be opened.
	bool AppendLog(const LocalPaths &paths, const LogEntry &entry);

	// Reads the log back.
	//
	// **For showing a person what is in the store**, which is the assets manager's
	// job and the only caller. Nothing in the engine's content path reads this.
	//
	// @param paths The store.
	// @return The entries, oldest first. Empty when there is no log.
	std::vector<LogEntry> ReadLog(const LocalPaths &paths);

	// What an import did.
	//
	// @since v0.10
	struct ImportReport {
		// Where it landed in `raw/`.
		std::filesystem::path Stored;

		// Its content hash, as hex.
		std::string Hash;

		// How big it was.
		uint64_t Bytes = 0;

		// Whether the store already had these exact bytes.
		//
		// **Reported rather than treated as an error**, because importing the
		// same file twice is what a person does and the right answer is "it is
		// already there". What it is *not* is a rename: two files with the same
		// bytes and different names are one file in `raw/`, under whichever name
		// arrived first - see `ImportFile` for why.
		bool Duplicate = false;
	};

	// Copies one file into `raw/`, named by its own hash.
	//
	// **Hash-named, which is `ROADMAP.md`'s "hash-based naming between the
	// folders".** The name kept is `<hash><extension>`: the hash is what makes
	// the two folders line up and makes a re-import free, and the extension is
	// what keeps the folder readable and lets a publisher know what it is looking
	// at without opening it.
	//
	// **The original name is not kept on disk and is kept in the log.** A flat
	// folder of original names collides the first time two projects both have a
	// `diffuse.png`, and resolving that means either a tree - which the roadmap
	// defers - or a suffix, which is a worse hash. The log is where "this came
	// from `~/art/fox/diffuse.png`" lives.
	//
	// **An empty file is refused, by name.** It can never bake and never
	// publish, so accepting one puts a file in `raw/` that nothing downstream
	// will ever account for - and since both halves report counts rather than
	// names, the only trace is that two totals differ by one. See the refusal in
	// the source for the hunt that cost.
	//
	// @param paths  The store.
	// @param source The file to bring in.
	// @param seconds The time to log it at.
	// @return What happened, or nothing when the file could not be read or was
	//         empty.
	std::optional<ImportReport>
	ImportFile(const LocalPaths &paths, const std::filesystem::path &source, uint64_t seconds);

	// One file sitting in `raw/`, waiting to be published.
	//
	// @since v0.10
	struct RawEntry {
		// The file, as it is on disk: `<hash><extension>`.
		std::filesystem::path Path;

		// The name somebody gave it before it was imported, from the log, or
		// the file name when the log does not say.
		//
		// **The log is used to label and never to enumerate.** The folder is
		// the index - this header opens by saying so - and `raw/` holds hashes,
		// so the only thing that can answer "what was this called" is the log.
		// A listing built *from* the log would show rows for files that are no
		// longer there and miss ones dropped in by hand.
		std::string Original;

		// What it weighs.
		uint64_t Bytes = 0;
	};

	// Where a named asset's bytes are on this machine.
	//
	// **`baked/` first, then `raw/`, and the order is the whole of it.** A
	// *published* name is a path under `baked/` - that is what `PublishLocal`
	// walks - and a name a raw listing produced is a path under `raw/`. Both
	// reach an editor wanting to show somebody a picture of an asset, and both
	// are just names by then.
	//
	// **This function is why previews broke when `baked/` arrived.** Everything
	// showing an asset read `raw/<name>` and said so in a comment that had been
	// true for exactly as long as the publisher walked `raw/`: the moment it
	// walked `baked/`, every name in every picker resolved to a file that was
	// not there, and each one silently became "no local pixels". One place that
	// knows the layout is the fix; two callers spelling it themselves is how it
	// went wrong.
	//
	// @param paths The store.
	// @param name  The asset's name, as a manifest or a raw listing gives it.
	// @return The file, or an empty path when neither folder has it - which is
	//         the honest answer for something published from another machine.
	// @since v0.10
	std::filesystem::path FindInStore(const LocalPaths &paths, std::string_view name);

	// The signing identity a local store uses when nobody supplies one.
	//
	// **A constant, in the source, and it is not a secret - that is the point.**
	// A signature answers "did the publisher I trust produce this", and for a
	// store on somebody's own disk, serving their own editor, the answer is
	// always yes and the key was pure friction: `assetc`, `cdn --publish` and
	// `client --publisher-key` all wanted the same sixty-four characters typed
	// again, and getting one wrong produced a client that refused every asset.
	//
	// **What it costs is real and is bounded by where it is used.** Anything
	// signed with this is trusted by anything that trusts this, so it is a
	// development identity for the well-known local store and nothing else. A
	// deployment supplies its own with `--signing-key` and `--publisher-key`,
	// which still work exactly as they did, and `cdn --publish` still *requires*
	// one - an origin serving other machines must not have a default identity
	// that everybody knows.
	//
	// @return The seed. Its public half is `DevelopmentPublisher()`.
	// @since v0.10
	engine::assets::SigningKey DevelopmentSigningKey();

	// The public half of `DevelopmentSigningKey`, for a client to trust.
	//
	// @return The key.
	// @since v0.10
	engine::assets::PublicKey DevelopmentPublisher();

	// What is actually in `raw/`, newest first.
	//
	// **The folder, labelled by the log** - see `RawEntry::Original`. Newest
	// first because somebody looking at this has just added something.
	//
	// @param paths The store.
	// @return The files. Empty when there is no store or nothing in it.
	// @since v0.10
	std::vector<RawEntry> RawContents(const LocalPaths &paths);

	// Resolves a model's texture references against the import log.
	//
	// **Because importing flattens, and flattening breaks models.** A `.pmx`
	// names its sheets as `tex/体.png`, relative to the folder it was authored
	// in. `ImportFile` renames every file to `<hash><extension>` in one flat
	// directory, so once a model and its sheets are in `raw/` nothing on disk
	// records that they belong together - and a bake over that folder joins the
	// reference lexically into `tex/体.atex`, a name no manifest carries. The
	// model publishes, arrives, draws, and has no textures.
	//
	// **The log is the only thing that still knows.** It records the path every
	// file had before it was imported, so the sheet's original path and the
	// model's original path share a directory exactly as the model expects. Walk
	// from one to the other and the hash falls out.
	//
	//     raw/<model hash>.pmx    ← was /art/char/model.pmx
	//     reference "tex/体.png"  → /art/char/tex/体.png
	//                             → raw/<sheet hash>.png
	//
	// **It is a labelling use of the log, which is the only kind this header
	// permits** - see `RawEntry::Original`. The folder is still the index: a
	// reference the log cannot place simply is not resolved, and the bake says so
	// rather than emitting a name that resolves to nothing.
	//
	// @param paths The store.
	// @return A resolver shaped for `assetc::Settings::ResolveTexture`, which
	//         answers with a name relative to `raw/`. It captures a snapshot of
	//         the log taken now - a store being written to while it bakes is not
	//         a case this tries to be live for.
	// @since v0.10
	std::function<bool(std::string_view model, std::string_view reference, std::string &out)>
	StoreTextureResolver(const LocalPaths &paths);

	// One asset the last publish put in `processed/`.
	//
	// @since v0.10
	struct PublishedEntry {
		// The name a game author writes, extension included - exactly the
		// string an emitter's `Texture` or a part's `Mesh` takes. AGENTS.md
		// rule 4: this is the thing that crosses, and there is no table
		// anywhere mapping it to anything.
		std::string Name;

		// What subsystem it belongs to, as the publisher decided.
		engine::assets::AssetKind Kind = engine::assets::AssetKind::Unknown;

		// Its content address.
		engine::assets::ContentHash Root;
	};

	// What the store has published, in name order.
	//
	// **Read from the signed manifest rather than from the folder**, because
	// `processed/` is content-addressed and a folder of hashes cannot say what
	// anything is called. The manifest is the only thing in the store that
	// knows both the name and the kind, which is exactly what a picker needs.
	//
	// **The signature is read and not checked.** This is the store on this
	// machine being shown to the person who owns it, not an origin's manifest
	// being trusted - `delivery::AssetClient` is where verification belongs and
	// it stays the only place, because two verifiers are two opinions.
	//
	// @param paths The store.
	// @return The assets. Empty when nothing has been published.
	// @since v0.10
	std::vector<PublishedEntry> PublishedContents(const LocalPaths &paths);
}
