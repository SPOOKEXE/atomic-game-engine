// The tag-first view of the tags an instance already carries.
//
// **`Instance:AddTag` answers "what does this thing carry"; nothing answered
// "what carries this".** `scene::Tagging` has held both halves of the data
// since v0.9 - a `Tags` mask per row and one `TagTable` per world - and the
// render pipeline reads it with a mask test. A script could not: it had the
// three per-instance methods and no way to reach an instance it was not
// already holding, so a scene that wanted "every door" kept its own table of
// them beside the tags. That is rule 2's two copies of one fact, and the copy
// drifts the first time something is destroyed.
//
// **One mechanism, from the other side.** Every method here calls the same
// `scene::AddTag`/`RemoveTag`/`HasTag` the instance methods call, and
// `GetTagged` walks the same `Tags` masks a surface camera filters on. This
// file holds no list of its own; a list here is exactly what would make it a
// second answer.
//
// **Neutral since v0.16, and nothing in this file names a VM.** The three
// per-instance calls it mirrors moved to `ScriptMethods.cpp` at the same
// version, so the two spellings of `AddTag` are now literally one call reached
// two ways - which is what the file header claimed before there was a layer
// that could make it true in both languages.
//
// **`Tags` is a `BasePart` component**, so tagging a bare container answers
// `false` rather than raising - the same answer `Instance:AddTag` gives, for
// the same reason it gives it. Which classes can be tagged is `scene`'s
// decision and this service does not get a vote.
//
// **Every list is sorted, and the two orders are different because the two
// questions are.** Names sort by text. Instances sort by entity id, which is a
// pure function of the order a world was built in - where the order
// `Store::Each` hands rows over is a function of which *archetypes* exist, so
// adding an unrelated component to one tagged part would reorder a list the
// scene laying itself out from had no reason to expect to move.
//
// --- why there is no `GetInstanceAddedSignal` -------------------------------
//
// **Because nothing can fire it honestly, and a signal that never fires reads
// as a broken engine rather than as a missing feature** - the trade `v0.5`
// records for `Heartbeat` and `PumpGuiEvents` for `InputObject`.
//
// `SignalTable` could carry it: the key is a kind and a subject, and the tag
// name would ride on `Connection::Property` exactly as `PropertyChanged`'s
// does. What is missing is the other end. A tag is added by writing a bit into
// a `Tags` mask, and `scene::AddTag` records nothing - there is no queue like
// `ecs::TreeChange` for the barrier to drain, and `Store::OnChanged<Tags>`,
// which `AddTag`'s `GetMutable` write would trip once something observed the
// component, says an instance's tags changed without saying *which* tag
// changed or in which direction.
// Recovering that needs the previous mask, and a copy of every mask kept here
// is the state `script/AGENTS.md` says this façade must never acquire.
//
// So closing it is a `scene` change: `AddTag`/`RemoveTag` record a
// `(entity, tag, added)` row the way `ObserveTree` records a reparent, and a
// `PumpTags` beside `PumpTree` delivers them at the barrier. That also fixes
// the hole a diff here could never see - a system writing `Tags::Mask`
// directly, which is what `Part.cpp`'s tag-list property does.
//
// @tier L9 · shared

#include <engine/core/Name.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Tagging.hpp>
#include <engine/script/ScriptCall.hpp>
#include <engine/script/ServiceSurface.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace engine::script {
	namespace {
		// Answers names sorted by text.
		//
		// **By `Text()` and never by `Name::operator<`**, which orders by
		// interning order - the order names were first constructed in this
		// *process*. Two runs that load scenes in a different order would sort
		// the same tags differently, which is the whole failure sorting is here
		// to prevent.
		void ReturnSortedNames(ScriptCall &call, std::span<const core::Name> names) {
			std::vector<std::string_view> text;
			text.reserve(names.size());
			for (const core::Name &name : names) {
				text.emplace_back(name.Text());
			}

			std::sort(text.begin(), text.end());
			call.ReturnStrings(text);
		}

		// `CollectionService:AddTag(instance, tag)`
		//
		// **The same call `Instance:AddTag` makes**, so which side a script
		// spells it from cannot change what happens.
		//
		// Answers `false` for an instance with no `Tags` component and for a
		// world whose thirty-two tags are spoken for, rather than raising -
		// `ScriptMethods.cpp` argues that one out: a full tag table is a scene
		// mistake and not a script one. Roblox returns nothing here; a boolean
		// is a superset, and a script that ignores it reads identically.
		void ServiceAddTag(ScriptCall &call) {
			const ecs::Entity instance = call.AsInstance(0);
			call.ReturnBoolean(scene::AddTag(call.World(), instance, core::Name(call.AsString(1))));
		}

		// `CollectionService:RemoveTag(instance, tag)`
		//
		// The name stays in the world's table - see `scene::RemoveTag` for why
		// a bit is never freed - so `GetAllTags` still lists a tag nothing
		// carries any more.
		void ServiceRemoveTag(ScriptCall &call) {
			const ecs::Entity instance = call.AsInstance(0);
			call.ReturnBoolean(scene::RemoveTag(call.World(), instance, core::Name(call.AsString(1))));
		}

		// `CollectionService:HasTag(instance, tag)`
		//
		// `false` for a tag this world has never registered, which is the same
		// answer as "registered, and this instance does not carry it". A script
		// that had to tell those apart is asking about the tag table rather
		// than about the instance, and `GetAllTags` is that question.
		void ServiceHasTag(ScriptCall &call) {
			const ecs::Entity instance = call.AsInstance(0);
			call.ReturnBoolean(scene::HasTag(call.World(), instance, core::Name(call.AsString(1))));
		}

		// `CollectionService:GetTagged(tag)` -> `{ Instance }`
		//
		// **The one this service exists for.** Nothing else in the engine can
		// answer it from a script: a mask says what one row carries and only a
		// walk says who carries a bit.
		//
		// **An unregistered tag is an empty list, not an error**, which departs
		// from the ECS surface's rule that a query naming an undeclared
		// component raises. The two look alike and are not: a component is
		// declared by C++ and a typo can never become valid, where a tag is
		// created by whichever `AddTag` names it first - so "nothing carries
		// this yet" is an ordinary state a script polling on `Heartbeat` sits
		// in until another script has run. Raising there would make a correct
		// script fail on the frame ordering.
		//
		// **Sorted by entity id**, which is the order the world was built in
		// and does not move when something unrelated changes. The archetype
		// walk below is *an* order, but it is a function of which tables exist:
		// anchoring one tagged part moves it to a different table and reorders
		// every list it appears in.
		void ServiceGetTagged(ScriptCall &call) {
			const core::Name tag(call.AsString(0));
			ecs::Store &store = call.World();

			// `Resource` rather than `TagsOf`, which creates the table on first
			// use: asking what carries a tag is not a reason to give a world a
			// tag table it had no use for.
			const scene::TagTable *table = store.Resource<scene::TagTable>();
			const uint32_t bit = table != nullptr ? table->Find(tag) : 0;
			if (bit == 0) {
				call.ReturnInstances({});
				return;
			}

			std::vector<ecs::Entity> tagged;
			store.Each<const scene::Tags>([bit, &tagged](ecs::Entity entity, const scene::Tags &tags) {
				if ((tags.Mask & bit) != 0) {
					tagged.push_back(entity);
				}
			});

			std::sort(tagged.begin(), tagged.end(), [](ecs::Entity left, ecs::Entity right) {
				return left.Id < right.Id;
			});

			call.ReturnInstances(tagged);
		}

		// `CollectionService:GetTags(instance)` -> `{ string }`
		//
		// Empty for an instance whose class has no `Tags` component, which is
		// everything that is not a `BasePart`, and empty for a world nothing
		// has tagged. Both are "this instance carries nothing", which is what
		// was asked.
		void ServiceGetTags(ScriptCall &call) {
			const ecs::Entity instance = call.AsInstance(0);
			const ecs::Store &store = call.World();

			const scene::Tags *tags = store.Get<scene::Tags>(instance);
			const scene::TagTable *table = store.Resource<scene::TagTable>();
			if (tags == nullptr || table == nullptr) {
				call.ReturnStrings({});
				return;
			}

			// `Describe` hands them back in bit order, which is registration
			// order; sorting is what makes the answer independent of which
			// script ran first.
			ReturnSortedNames(call, table->Describe(tags->Mask));
		}

		// `CollectionService:GetAllTags()` -> `{ string }`
		//
		// Every name this world has registered, including one nothing carries:
		// a bit is never freed, so a tag added and removed again is still a
		// name the table holds. Reporting only the tags in use would mean
		// walking every row to answer a question about the table.
		void ServiceGetAllTags(ScriptCall &call) {
			const scene::TagTable *table = call.World().Resource<scene::TagTable>();
			if (table == nullptr) {
				call.ReturnStrings({});
				return;
			}
			ReturnSortedNames(call, table->Names);
		}

		constexpr std::array<ServiceMethod, 6> COLLECTION_METHODS{{
			{"AddTag", ServiceAddTag},
			{"RemoveTag", ServiceRemoveTag},
			{"HasTag", ServiceHasTag},
			{"GetTagged", ServiceGetTagged},
			{"GetTags", ServiceGetTags},
			{"GetAllTags", ServiceGetAllTags},
		}};
	}

	const ServiceSurface &CollectionServiceSurface() {
		// No signals. The file header says what firing one honestly would take
		// and why a signal that never fires is worse than an absent one.
		static const ServiceSurface SURFACE = [] {
			ServiceSurface surface;
			surface.Name = "CollectionService";
			surface.Methods = COLLECTION_METHODS;
			return surface;
		}();
		return SURFACE;
	}
}
