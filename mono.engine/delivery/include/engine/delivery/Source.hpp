#pragma once

// Content sources are tried in list order. Source descriptors are untrusted and
// HTTP hosts must pass the configured allow-list.
//
// @tier L11 · shared

#include <engine/assets/Signature.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace engine::delivery {

	// Default development origin port.
	constexpr uint16_t DEFAULT_ORIGIN_PORT = 9080;

	// What kind of place a source is.
	//
	// @since v0.9
	enum class SourceKind : uint8_t {
		// A local published content tree.
		Directory,

		// An HTTP origin.
		Http,
	};

	// Returns a stable, human-readable name for a source kind.
	//
	// @param kind The kind to name.
	// @return A view valid for the lifetime of the process.
	const char *Describe(SourceKind kind);

	// Which direction a source is used in.
	//
	// **This is the whole of "one server takes the writes, another serves the
	// reads".** A deployment that splits them does not need two source lists
	// and two orderings - it needs one list where each entry says which
	// direction it participates in, because the ordering somebody wants is the
	// same ordering in both directions: nearest first.
	//
	// **What it separates is traffic, not trust.** Two entries may name the
	// same host and differ only in this field, which is the case the split
	// exists for: an operator who wants uploads and downloads on different
	// ports, interfaces or names of one machine can have that without either
	// side of the engine knowing it is one machine.
	//
	// **Replication between them is not this engine's.** A write origin that
	// mirrors to a read origin does it by whatever the operator already uses -
	// a shared volume, rsync, the storage layer. Nothing here waits for it, and
	// nothing here reports on it: content uploaded a moment ago is fetchable
	// when the read side has it and the publisher has signed a manifest naming
	// it, and no sooner.
	//
	// **A closed list whose ordinal is written to a preferences file**, so an
	// entry may be added at the end and never reordered - `SourceKind`'s rule.
	//
	// @since v0.10
	enum class SourceRole : uint8_t {
		// Fetched from and uploaded to. What a single-origin setup is, and the
		// default, because that is what every existing configuration meant
		// before there was a choice.
		Both = 0,

		// Fetched from only. Never receives an upload.
		Read = 1,

		// Uploaded to only. Never consulted for a fetch.
		//
		// **A write origin is invisible to `AssetClient`**, which is what makes
		// it safe for one to hold content nobody has published yet: a client
		// that fell back to it would fetch bytes no signed manifest describes,
		// and then fail verification in a way that looks like corruption.
		Write = 2,
	};

	// Returns a stable, human-readable name for a source role.
	//
	// @param role The role to name.
	// @return A view valid for the lifetime of the process.
	const char *Describe(SourceRole role);

	// One place content may be fetched from or sent to.
	//
	// @since v0.9
	struct Source {
		// Display name used by logs and preferences.
		std::string Name = {};

		// Source type.
		SourceKind Kind = SourceKind::Http;

		// Directory path or `host:port`.
		std::string Location = {};

		// Whether the client tries this source.
		bool Enabled = true;

		// Which direction this source is used in.
		//
		// @since v0.10
		SourceRole Role = SourceRole::Both;

		// The shared secret an upload sends as `x-atomic-ingest`.
		//
		// **Only ever read on a source this list writes to**, and empty on
		// every read source. An origin refuses an upload without it - see
		// `cdn::IngestSettings`, which also carries why a secret is enough here
		// and what it does not buy.
		//
		// @since v0.10
		std::string IngestKey = {};

		// Returns false for an empty name or location, or an invalid HTTP location.
		bool IsValid() const;

		// Whether this source participates in fetching.
		bool Readable() const {
			return Role != SourceRole::Write;
		}

		// Whether this source participates in uploading.
		//
		// **A `Directory` is writable without a key**, because writing to one
		// is writing to a filesystem the caller already has: the key exists to
		// admit a request to somebody else's origin, and there is no request
		// here to admit.
		bool Writable() const {
			return Role != SourceRole::Read && (Kind == SourceKind::Directory || !IngestKey.empty());
		}
	};

	// The whole of how a client fetches content.
	//
	// @since v0.9
	struct DeliverySettings {
		// Sources are tried in this order. Failed sources are skipped.
		std::vector<Source> Sources;

		// Cache for verified content. Empty disables the cache.
		std::filesystem::path CachePath;

		// What the cache may hold before it starts evicting.
		uint64_t CacheCapacityBytes = 1024ull * 1024ull * 1024ull;

		// Publisher key for manifest verification. Required for valid settings.
		assets::PublicKey Publisher;

		// Allowed HTTP hosts. Empty allows all hosts.
		std::vector<std::string> AllowedHosts;

		// Polls without progress before the client tries the next source.
		uint32_t IdlePolls = 6000;

		// The settings a client starts with: one origin, on this machine.
		//
		// @param cachePath Where to keep verified content.
		// @return Settings naming `127.0.0.1:9080` and nothing else.
		static DeliverySettings Default(const std::filesystem::path &cachePath = {});

		// Returns false without a usable source, publisher key, or permitted host.
		bool IsValid() const;

		// Returns enabled and valid **readable** sources in priority order.
		//
		// A `SourceRole::Write` entry is not in this list, which is what keeps
		// a write origin invisible to a fetch - see `SourceRole::Write`.
		std::vector<Source> Usable() const;

		// Returns enabled and valid **writable** sources in priority order.
		//
		// **The same order as `Usable`, filtered differently, and not a second
		// ordering.** Somebody who wants uploads to prefer a different origin
		// than downloads expresses it by which entries carry which role, not by
		// maintaining two lists that would drift.
		//
		// An upload goes to *every* source in this list rather than stopping at
		// the first that answers, which is the opposite of a fetch and is the
		// right opposite: a fetch wants one copy of the bytes and a publish
		// wants every write origin to end up holding them.
		//
		// @since v0.10
		std::vector<Source> Writable() const;
	};

	// Returns whether an HTTP location uses an allowed host.
	//
	// @param location The `host:port` to check.
	// @param allowed The permitted hosts, or empty for no restriction.
	// @return Whether a fetch may be made to it.
	bool HostPermitted(std::string_view location, const std::vector<std::string> &allowed);
}
