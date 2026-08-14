#include <engine/assets/ContentPolicy.hpp>
#include <engine/core/Flags.hpp>
#include <engine/core/Log.hpp>

#include <span>
#include <string>
#include <vector>

namespace engine::assets {
	namespace {
		constexpr size_t WORD_BITS = 64;

		// The highest ordinal the bitset can hold. Checked against the table
		// rather than trusted, because a form added past it would silently be
		// allowed for ever — the failure a policy must not have.
		constexpr size_t POLICY_BITS = 128;

		size_t OrdinalOf(ContentForm form) {
			return static_cast<size_t>(form);
		}

		// The flag rows for one verb, built once from the extension table.
		//
		// **In `AllForms` order with `unknown` on the end**, which is what lets
		// `FromFlags` walk the two together rather than looking each name up.
		core::FlagTableBuilder Build(ContentVerb verb) {
			core::FlagTableBuilder table;
			const std::string prefix = Describe(verb);
			const std::string action =
				verb == ContentVerb::Handle ? "Deliver, decode and route" : "Let a publish take";

			for (const ContentForm form : AllForms()) {
				table.Boolean(prefix + Describe(form), true, action + " ." + Describe(form) + " content");
			}

			// **The one form whose flag is not about a format.** `unknown` is
			// every extension this build has no row for, so turning it off is
			// how a deployment closes the list rather than naming the forms it
			// does not want.
			table.Boolean(
				prefix + "unknown", true, action + " content whose extension this build has no row for"
			);
			return table;
		}

		const core::FlagTableBuilder &Table(ContentVerb verb) {
			static const core::FlagTableBuilder handle = Build(ContentVerb::Handle);
			static const core::FlagTableBuilder publish = Build(ContentVerb::Publish);
			return verb == ContentVerb::Handle ? handle : publish;
		}
	}

	const char *Describe(ContentVerb verb) {
		switch (verb) {
		case ContentVerb::Handle:
			return "content.";
		case ContentVerb::Publish:
			return "cdn.publish.";
		}
		return "content.";
	}

	bool ContentPolicy::Allows(ContentForm form) const {
		const size_t ordinal = OrdinalOf(form);
		if (ordinal >= POLICY_BITS) {
			// Past what the bitset holds. Allowed, and the `static_assert` in
			// `DeclareContentFlags` is what stops this being reachable.
			return true;
		}
		return (Words[ordinal / WORD_BITS] & (uint64_t{1} << (ordinal % WORD_BITS))) != 0;
	}

	void ContentPolicy::Allow(ContentForm form, bool allowed) {
		const size_t ordinal = OrdinalOf(form);
		if (ordinal >= POLICY_BITS) {
			return;
		}

		const uint64_t bit = uint64_t{1} << (ordinal % WORD_BITS);
		if (allowed) {
			Words[ordinal / WORD_BITS] |= bit;
		} else {
			Words[ordinal / WORD_BITS] &= ~bit;
		}
	}

	size_t ContentPolicy::RefusedCount() const {
		size_t refused = 0;
		if (!Allows(ContentForm::Unknown)) {
			refused++;
		}
		for (const ContentForm form : AllForms()) {
			if (!Allows(form)) {
				refused++;
			}
		}
		return refused;
	}

	std::string ContentPolicy::RefusedText() const {
		std::string text;
		for (const ContentForm form : AllForms()) {
			if (Allows(form)) {
				continue;
			}
			if (!text.empty()) {
				text += ", ";
			}
			text += Describe(form);
		}

		if (!Allows(ContentForm::Unknown)) {
			if (!text.empty()) {
				text += ", ";
			}
			text += "unknown";
		}
		return text;
	}

	ContentPolicy ContentPolicy::FromFlags(ContentVerb verb) {
		ContentPolicy policy;
		const std::span<const core::FlagDescription> rows = Table(verb).Rows();

		// **Nothing declared them means everything is allowed, which is the
		// safe direction and not the convenient one.** A dead flag handle reads
		// `false`, so a program that never called `DeclareContentFlags` — a
		// tool, a test standing one part of the engine up — would otherwise
		// refuse every form it has and produce an empty bake with no
		// explanation. Allowing is what this engine did before the flags
		// existed, so a build that does not use them behaves as it always did.
		if (rows.empty() || !core::Flags::Has(rows.front().Name)) {
			return policy;
		}

		// The rows are in `AllForms` order with `unknown` appended, which is how
		// the two are kept in step without a second lookup by name per form.
		const std::span<const ContentForm> forms = AllForms();
		for (size_t index = 0; index < forms.size(); index++) {
			policy.Allow(forms[index], core::Flag(rows[index].Name).Boolean());
		}
		policy.Allow(ContentForm::Unknown, core::Flag(rows[forms.size()].Name).Boolean());
		return policy;
	}

	const ContentPolicy &ContentPolicy::Process(ContentVerb verb) {
		struct Cached {
			ContentPolicy Policy;

			// **A generation and not a bool**, because a test resetting the
			// flags and declaring a different table must not be handed the
			// previous suite's answer — and "have I read this yet" cannot tell
			// the two apart. Starts one behind, so the first call always reads.
			uint32_t ReadAt = ~uint32_t{0};
		};

		static Cached handled;
		static Cached published;
		Cached &cached = verb == ContentVerb::Handle ? handled : published;

		// **Not cached until the flags are frozen**, which is what makes the
		// cache correct rather than merely fast: before the freeze a value can
		// still move, and a policy read during startup and kept would be
		// whatever the settings happened to be halfway through being applied.
		// After it, nothing can move and one read is the answer.
		const uint32_t generation = core::Flags::Generation();
		if (!core::Flags::Frozen() || cached.ReadAt != generation) {
			cached.Policy = FromFlags(verb);
			cached.ReadAt = core::Flags::Frozen() ? generation : ~uint32_t{0};
		}
		return cached.Policy;
	}

	bool DeclareContentFlags(ContentVerb verb) {
		// A form whose ordinal is past the bitset would be allowed for ever with
		// nothing reporting it, so the count is checked here rather than trusted
		// — this is the one function every program calls before reading a
		// policy.
		static_assert(POLICY_BITS == 128, "the policy holds two words");

		bool ok = core::Flags::Declare(Table(verb).Rows());

		for (const ContentForm form : AllForms()) {
			if (OrdinalOf(form) < POLICY_BITS) {
				continue;
			}
			// Loud rather than silent: the alternative is a flag that exists,
			// reads `false`, and refuses nothing.
			ENGINE_ERROR(
				"content: '{}' sits past what ContentPolicy holds, so refusing it would do nothing",
				Describe(form)
			);
			ok = false;
		}
		return ok;
	}
}
