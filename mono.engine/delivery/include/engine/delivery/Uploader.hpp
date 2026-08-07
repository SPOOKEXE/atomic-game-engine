#pragma once

// Sending content the other way: local files up to the origins that take
// writes.
//
// **The mirror of `AssetClient`, and deliberately not part of it.** A fetch and
// a publish share a source list and nothing else. A fetch stops at the first
// source that answers, is driven from inside a tick, and is the hot path; an
// upload goes to *every* write origin, is driven by somebody pressing a button
// in an editor, and is allowed to be slow. Folding the second into the first
// would put a file-writing code path inside the class that runs every frame,
// for the sake of sharing a vector.
//
// **Every destination gets every file, which is the opposite of a fetch and the
// right opposite.** A fetch wants one copy of the bytes; a publish wants each
// write origin to end up holding them. So a failure against one destination is
// reported and the others still receive the file — `DeliverySettings::Writable`
// carries the same note from the other end.
//
// **One file in flight at a time.** An editor uploading a content tree is
// bounded by somebody's upstream link rather than by concurrency, and one file
// at a time means one file's bytes in memory rather than sixteen — which for a
// content store whose largest entries are tens of megabytes is the difference
// between a bounded process and a hopeful one.
//
// **Nothing here signs anything and nothing here publishes.** What arrives at
// an origin's inbox is raw content, and a client will not look at it until a
// publisher has signed a manifest naming it — CDN.md §1. Uploading is moving
// bytes; publishing is `cdn::Publish`, it needs a signing key, and it stays a
// separate act on purpose.
//
// @tier L11 · shared

#include <engine/assets/ContentHash.hpp>
#include <engine/delivery/Source.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace engine::delivery {

	// What an uploader has done, for a diagnostic and for a test.
	//
	// Counted per file *per destination*, so two write origins and ten files is
	// twenty of these — which is the number an operator watching a mirror
	// actually wants, and the one that makes "did it reach both" answerable.
	//
	// @since v0.10
	struct UploadCounters {
		// Files an origin did not have and now does.
		uint64_t Stored = 0;

		// Files a destination already held.
		//
		// **Not a failure and usually the majority.** The bytes are the
		// identity, so a tree uploaded twice is almost entirely this — and the
		// `HEAD` probe that produces the number is what makes the second
		// upload cost a round trip a file instead of the whole tree.
		uint64_t Skipped = 0;

		// Bytes that actually crossed, skipped files excluded.
		uint64_t SentBytes = 0;

		// Uploads a destination refused: no key, a wrong key, or a body it
		// would not take.
		//
		// **Counted apart from a failure**, because they point at different
		// people: a refusal is something to fix in this configuration, and a
		// failure is something to fix at the far end.
		uint64_t Refused = 0;

		// Uploads that did not complete — an unreachable origin, a file that
		// could not be read, a directory that could not be written.
		uint64_t Failed = 0;
	};

	// One file's outcome at one destination.
	//
	// @since v0.10
	struct UploadOutcome {
		// The file, as it was queued.
		std::filesystem::path File;

		// Which write source this concerns.
		std::string Destination;

		// The file's content address, which is also the name it is stored
		// under.
		assets::ContentHash Root;

		// What happened, ready to put in front of a person: `stored`,
		// `already there`, `refused`, or why it failed.
		std::string Detail;

		// Whether the bytes are at the destination now, whether or not this
		// upload is what put them there.
		bool Delivered = false;
	};

	// Pushes local files to every write source, without blocking.
	//
	// **One owner, one thread**, the same as `AssetClient` and for the same
	// reason.
	//
	// @since v0.10
	class Uploader {
	  public:
		virtual ~Uploader() = default;

		// Queues a file for every write destination.
		//
		// The bytes are read and hashed here rather than at send time, so a
		// file edited while a queue drains uploads as the thing that was
		// queued rather than as a mixture.
		//
		// @param file The file to send.
		// @return Whether it could be read. A file that could not is counted
		//         as failed and reported through `Take`, so a caller draining a
		//         directory does not have to check every call.
		virtual bool Add(const std::filesystem::path &file) = 0;

		// Drives the queue.
		//
		// @return How many file-destination pairs finished this call.
		virtual size_t Pump() = 0;

		// How many file-destination pairs are queued or in flight.
		virtual size_t Remaining() const = 0;

		// Takes everything that has finished since the last call.
		//
		// Drained rather than accumulated, so a long-running editor does not
		// grow a list it never reads.
		virtual std::vector<UploadOutcome> Take() = 0;

		// What this uploader has done.
		virtual const UploadCounters &Counters() const = 0;

		// The destinations this uploader resolved at construction.
		//
		// **Resolved once, so a source that is misconfigured says so at
		// start-up** rather than as a stream of individually plausible failures
		// at file rate — `ContentRoot::Mount`'s rule, the same one
		// `AssetClient` follows.
		virtual const std::vector<Source> &Destinations() const = 0;
	};

	// Builds an uploader over the write sources of a settings block.
	//
	// @param settings Where content may be sent. Only `Writable()` entries are
	//        used; the publisher key is not consulted, because nothing on this
	//        path verifies a manifest.
	// @return The uploader, or nothing when no source accepts writes — which is
	//         the ordinary state of a configuration that has never been told
	//         where its write origin is, and is worth being an absent object
	//         rather than one that silently does nothing.
	// @since v0.10
	std::unique_ptr<Uploader> MakeUploader(const DeliverySettings &settings);
}
