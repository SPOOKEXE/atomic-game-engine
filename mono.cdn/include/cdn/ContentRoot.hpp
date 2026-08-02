#pragma once

// The boundary between a content name and the filesystem.
//
// The origin serves bytes out of a directory, and every name it is asked for
// arrives from outside this process — a manifest, a session descriptor, a
// request. repo_layout.md §11 treats the descriptor as untrusted content like
// everything else, and the names inside it are untrusted for the same reason: a
// resolver that hands back `../../etc/passwd` is a file-disclosure primitive
// with a friendly name.
//
// So resolution refuses by default and the check lives here, once, rather than
// at the call sites. A call site added later is a call site that forgot.
//
// This is the local-filesystem half of §11's three sources — the one the server
// uses when it serves its own disk, and the one the origin uses when it is
// deployed on its own. Server-proxied and direct delivery are the same store
// reached over `net`, not a second implementation, and they arrive with
// `Engine::assets`.
//
// Every entry point here opens an `ENGINE_PROFILE` span, so an origin under
// load reads on the F5 overlay and in Tracy the same way a frame does. Content
// delivery is the one subsystem whose cost is almost entirely waiting on a
// filesystem, and a span is what separates "the disk is slow" from "we resolved
// the same name four thousand times".
//
// @tier shared

#include <filesystem>
#include <optional>
#include <string_view>

namespace cdn {

	// A directory content is served out of, and the boundary of what may leave it.
	//
	// Mount one at start-up and keep it. The directory is canonicalised on the
	// way in, so every later comparison is against a path with no symlink, no
	// `..` and no trailing separator still in it — which is what lets Resolve
	// decide containment by comparing two paths rather than by walking a tree.
	class ContentRoot {
	  public:
		// Canonicalises `directory` and takes it as the serving boundary.
		//
		// Refuses a directory that is missing or is not a directory. Mounting one
		// that is not there would turn a misconfigured deployment into a stream
		// of individually plausible per-request failures, at request rate, with
		// nothing anywhere saying the root was wrong — so it is refused once, at
		// start-up, where a person is still watching.
		//
		// @param directory The directory to serve from. A relative path resolves
		//        against the working directory.
		// @return The mounted root, or nothing. The reason is logged.
		static std::optional<ContentRoot> Mount(const std::filesystem::path &directory);

		// The canonical directory being served. Never empty on a mounted root.
		const std::filesystem::path &Directory() const {
			return Base;
		}

		// Turns a content name into a path inside this root, or refuses it.
		//
		// A name is a relative, forward-slash path — `meshes/rock.mesh`. It is
		// refused when it is empty, absolute, or carries any `.` or `..`
		// component, and the resolved path is then checked against the root as
		// well.
		//
		// **Both halves are deliberate.** The component check alone cannot see a
		// symlink inside the root that points out of it. The containment check
		// alone accepts a name that only lands inside by accident, and accepts
		// two spellings of one file — which defeats content addressing the day
		// the manifest starts keying on the name.
		//
		// A refusal is not a missing file. It says the name may not be served at
		// all; @ref ContentRoot::Exists is the question about the bytes.
		//
		// Every call counts itself as `cdn.resolve.served` or
		// `cdn.resolve.refused` and opens a profiling span. The two counters are
		// separate on purpose: a refusal rate that climbs is somebody walking
		// the origin, and that reads very differently from content going
		// missing.
		//
		// @param name The requested content name.
		// @return The path to read, or nothing if the name may not be served.
		std::optional<std::filesystem::path> Resolve(std::string_view name) const;

		// Whether `name` resolves and names a regular file that is there now.
		//
		// A directory is not content. Answering true for one would leave every
		// caller one failed open away from a problem this call could have
		// reported.
		//
		// @param name The requested content name.
		// @return True only for a name that resolves and is a regular file.
		bool Exists(std::string_view name) const;

	  private:
		explicit ContentRoot(std::filesystem::path directory);

		std::filesystem::path Base;
	};
}
