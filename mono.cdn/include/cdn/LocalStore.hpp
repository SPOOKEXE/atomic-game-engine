#pragma once

// The content folder on this machine, at a path every program agrees on.
//
// **What this is for is making the cdn the default rather than a flag.** Before
// it, content only existed if somebody ran `assetc`, then `cdn --publish`, then
// passed `--cdn dir:...` to the client — three steps and a path, every time, on
// every machine. A test that wanted a texture had to do all three. So this is the
// well-known place: `~/Documents/atomic-game-engine/cdn`, with two folders under
// it, and every program looks there when nobody has said otherwise.
//
// ## The two folders, and why the split
//
// - **`raw/`** — what a person put in. A `.png`, a `.gltf`, a `.wav`, under its
//   own name. This is the half a human reads and drags files into.
// - **`processed/`** — what the engine reads: chunks, groups, a manifest.
//   Content-addressed, so a name here is a hash and nothing else.
//
// **The split is not tidiness, it is that the two have different identities.** A
// raw file is identified by what somebody called it and changes when they edit
// it; a processed chunk is identified by its bytes and never changes at all.
// Putting both in one folder would mean either hashing the names or naming the
// hashes, and each of those loses the half that made the other useful.
//
// **`ImportFile` is flat; `raw/` is not required to be.** `Publish` has always
// walked it recursively and named each asset by its path relative to the root, so
// a tool writing a tree there was always publishable — and v0.10's material
// import is the first thing that does, because a material has to *name* its
// texture and a hash rename gives it no name to write. `RawContents` was not
// recursive and showed such a store as empty; it is now. What the paragraph below
// is about is what a *person* drags in.
//
// **Flat, for now, and `ROADMAP.md` says so in as many words.** A tree under
// `raw/` is what an author eventually wants — `characters/`, `props/` — and it is
// a decision about how a manifest name is built, which is worth making once the
// assets manager exists to show the tree. Flat until then, and the log is what
// makes a flat folder navigable.
//
// ## The log
//
// One line per import and per publish, appended, never rewritten. **It is a
// record and not an index**: nothing reads it back to find a file, because the
// folder is the index. What it answers is "where did this come from and when",
// which is the question a content folder is asked six months later and which no
// amount of hashing can answer.
//
// @tier shared

#include <engine/assets/AssetKind.hpp>
#include <engine/assets/ContentHash.hpp>

#include <cdn/Publisher.hpp>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cdn {

	// Where the local store lives, and what it is called.
	//
	// @since v0.10
	struct LocalPaths {
		// The folder holding the other two and the log.
		std::filesystem::path Root;

		// What a person put in.
		std::filesystem::path Raw;

		// What the engine reads.
		std::filesystem::path Processed;

		// The record of what happened.
		std::filesystem::path Log;
	};

	// The default location, per `ROADMAP.md` v0.10.
	//
	// `~/Documents/atomic-game-engine/cdn` on every platform, because that is
	// what the roadmap names. **Not a platform-idiomatic data directory**, which
	// would be `~/.local/share` on Linux and `%APPDATA%` on Windows — and that is
	// a deliberate choice rather than an oversight: this folder is one a *person*
	// drags files into, and a hidden per-platform data directory is exactly where
	// somebody cannot find it.
	//
	// **The home directory is read from the environment and not assumed.** A
	// process with no `HOME` — a container, a service — falls back to the current
	// directory rather than to `/`, so a store is created somewhere writable
	// rather than failing at the first import.
	//
	// @return The four paths. Nothing is created; see `EnsureLocalStore`.
	LocalPaths DefaultLocalPaths();

	// The same, rooted somewhere else.
	//
	// **For tests, and for a machine with content on another disk.** Every
	// function here takes paths rather than finding them, so the default is one
	// caller's choice rather than a global — which is what lets a suite build a
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
	// lines rather than losing each other's — which is the whole reason this is a
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
		// arrived first — see `ImportFile` for why.
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
	// `diffuse.png`, and resolving that means either a tree — which the roadmap
	// defers — or a suffix, which is a worse hash. The log is where "this came
	// from `~/art/fox/diffuse.png`" lives.
	//
	// @param paths  The store.
	// @param source The file to bring in.
	// @param seconds The time to log it at.
	// @return What happened, or nothing when the file could not be read.
	std::optional<ImportReport>
	ImportFile(const LocalPaths &paths, const std::filesystem::path &source, uint64_t seconds);

	// Publishes everything in `raw/` into `processed/`.
	//
	// A thin wrapper over `cdn::Publish` that supplies the two paths and writes
	// the log line. **The signing key is the caller's**, because a store on disk
	// has no business holding one — `assets::Signature` is the whole reason a
	// manifest is trustworthy and a key beside the content it signs is a key that
	// signs anything anybody drops there.
	//
	// @param paths    The store.
	// @param signing  The key to sign the manifest with.
	// @param seconds  The time to log it at.
	// @param settings How to chunk and group.
	// @return The publish report, or nothing when it failed.
	std::optional<PublishReport> PublishLocal(
		const LocalPaths &paths,
		const engine::assets::SigningKey &signing,
		uint64_t seconds,
		const PublishSettings &settings = {}
	);

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
		// the index — this header opens by saying so — and `raw/` holds hashes,
		// so the only thing that can answer "what was this called" is the log.
		// A listing built *from* the log would show rows for files that are no
		// longer there and miss ones dropped in by hand.
		std::string Original;

		// What it weighs.
		uint64_t Bytes = 0;
	};

	// What is actually in `raw/`, newest first.
	//
	// **The folder, labelled by the log** — see `RawEntry::Original`. Newest
	// first because somebody looking at this has just added something.
	//
	// @param paths The store.
	// @return The files. Empty when there is no store or nothing in it.
	// @since v0.10
	std::vector<RawEntry> RawContents(const LocalPaths &paths);

	// One asset the last publish put in `processed/`.
	//
	// @since v0.10
	struct PublishedEntry {
		// The name a game author writes, extension included — exactly the
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
	// being trusted — `delivery::AssetClient` is where verification belongs and
	// it stays the only place, because two verifiers are two opinions.
	//
	// @param paths The store.
	// @return The assets. Empty when nothing has been published.
	// @since v0.10
	std::vector<PublishedEntry> PublishedContents(const LocalPaths &paths);
}
