#pragma once

// Which content formats a deployment will handle, and the flags that say so.
//
// **Two verbs and one vocabulary.** A form can be refused in two places and
// they mean different things, so they are two flags over one set of names:
//
// - `content.<form>` — *this process will not decode, route or hand over this
//   form.* Read by the client's content pump, by `bake`'s import dispatch and
//   by the studio's pickers.
// - `cdn.publish.<form>` — *this form does not enter a publication.* Read by
//   `cdn::Publisher`, once, at publish time.
//
// **There is deliberately no serve-time gate**, and that is a fact about the
// origin rather than an omission: after a publish there are only hashes, and a
// hash cannot be walked back to a name — `cdn/Publisher.hpp` says so in its
// first paragraph, because it is what stops a request path being a directory
// walk. So the honest place for an origin to refuse a form is the one place it
// still knows what the file was called.
//
// **Everything is allowed by default**, which is the same argument
// `server::Options::Listening` makes for being off: a default that changed what
// an existing recipe does would be a behaviour change arriving by construction
// rather than by anybody asking for it. `just determinism` and
// `just replay-check` compare bytes, and a flag layer that shipped with an
// opinion would move them.
//
// **`Unknown` is always allowed.** An origin moves bytes it does not interpret,
// and a policy that refused what it cannot name would refuse the next format
// before it was added. A deployment that wants to allow only a list closes it
// with `cdn.publish.unknown` — which is the one form whose flag means "anything
// this build has no row for".
//
// @tier L8 · shared
// @since v0.15

#include <engine/assets/ContentForm.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace engine::assets {

	// What a policy governs, which is also its flag prefix.
	enum class ContentVerb : uint8_t {
		// `content.<form>` — decode, route and hand over.
		Handle,

		// `cdn.publish.<form>` — let it into a publication.
		Publish,
	};

	// The flag prefix a verb reads under, including the trailing dot.
	//
	// @param verb Which verb.
	// @return A view valid for the lifetime of the process.
	const char *Describe(ContentVerb verb);

	// Which forms are allowed, as one word per form.
	//
	// Cheap to copy and cheap to ask: a bitset over the forms plus one bit for
	// the unnamed. Held by whoever does the refusing rather than re-read from
	// the flags at each call, so a policy can also be built by hand in a test
	// without a process-wide table in the way.
	class ContentPolicy {
	  public:
		// Everything allowed, which is what this engine does today.
		ContentPolicy() = default;

		// Whether this form may be handled.
		//
		// @param form The form. `Unknown` answers whatever `Allow` was last told
		//        about it, and `true` by default.
		// @return `true` when it is allowed.
		bool Allows(ContentForm form) const;

		// Whether the form a name claims may be handled.
		//
		// @param name The content name.
		// @return `true` when it is allowed.
		bool AllowsName(std::string_view name) const {
			return Allows(FormOfName(name));
		}

		// Allows or refuses one form.
		//
		// @param form    The form.
		// @param allowed Whether it may be handled.
		void Allow(ContentForm form, bool allowed);

		// How many forms are refused.
		size_t RefusedCount() const;

		// The refused forms, comma-separated, or empty when none are.
		//
		// **What a program logs at startup**, because a policy that quietly
		// dropped content would produce exactly the "my texture never arrives"
		// report this layer is meant to make diagnosable in one line.
		std::string RefusedText() const;

		// Reads the flags for one verb.
		//
		// **For a caller that wants a policy of its own** — a test, or a tool
		// baking under a policy somebody passed it. Everything in a running
		// program wants `Process` instead.
		//
		// @param verb Which set of flags to read.
		// @return The policy they describe.
		static ContentPolicy FromFlags(ContentVerb verb);

		// This process's policy for one verb, read from the flags once.
		//
		// **Cached because the flags are frozen**, which is what makes one read
		// the answer for the life of the run — `core::Flags::Freeze` is called
		// by every program before it starts anything. Without the freeze this
		// would be a cache that can go stale; with it, re-reading per asset
		// would be a table scan per name for a value that cannot move.
		//
		// **Nothing is cached until the freeze**, so a caller reading this
		// during startup, while the settings are still being applied, gets the
		// current answer rather than a snapshot of a half-applied one. A
		// `Flags::Reset` — which only a test does — invalidates it too, so a
		// suite that declares a different table does not get the previous
		// one's answer.
		//
		// @param verb Which set of flags to read.
		// @return The policy, valid until the flags are reset.
		static const ContentPolicy &Process(ContentVerb verb);

	  private:
		// One bit per form, indexed by the enum's ordinal — which is why the
		// header says the ordinals are not on the wire. Forty-one forms fit in
		// two words with room to grow; a `static_assert` in the source is what
		// notices when they do not.
		uint64_t Words[2] = {~uint64_t{0}, ~uint64_t{0}};
	};

	// Declares one verb's flag for every form.
	//
	// **Generated from `AllForms` rather than written out**, so a format added
	// to the extension table gets its flags with no second edit and no second
	// list — which is the whole reason the table grew a form column.
	//
	// **A verb at a time, because a program declares what it does.** A client
	// publishes nothing, so a client carrying `cdn.publish.*` in its `--flags`
	// listing would be offering settings that cannot affect it — and a listing
	// that half means something is one nobody reads. A program that both serves
	// and consumes content calls this twice.
	//
	// Called by a program's startup, before it applies any settings. Calling it
	// twice for one verb is refused and named by `core::Flags::Declare`.
	//
	// @param verb Which set to declare.
	// @return `false` when a name collided, which is a bug in a table.
	bool DeclareContentFlags(ContentVerb verb);
}
