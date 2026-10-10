// Finding instances by what they *are* rather than by what they are called.
//
// **This panel names no property, and that is the whole point.** The properties
// panel is generic because `PropertyDescriptor` is data - a name, a type, and a
// getter - and the Luau binding is generic for the same reason. A search built
// on the same three things inherits the same property: a component declared by
// any module tomorrow is searchable today, with nothing here changing.
//
// The alternative, and the reason this is worth stating: a search with a
// hand-written list of searchable fields is a list that goes stale the first
// time somebody adds a property and forgets it, and the failure is silent - the
// instance simply does not turn up.
//
// **Matching is over `FormatValue`, which is what makes one predicate work for
// every type.** `game::Values.hpp` already renders any property to text for the
// properties panel and the `.agame` writer; comparing against that string means
// `Transparency` `0.5`, `AlphaMode` `Clip` and `Anchored` `true` are all the
// same operation. Exact comparison goes the other way - `ParseValue` into the
// property's own type and `ValuesEqual` - so "0.5" does not match "0.50001"
// when somebody asks for exactness.

#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Instance.hpp>
#include <engine/game/Values.hpp>
#include <engine/ui/Theme.hpp>

#include <algorithm>
#include <cctype>
#include <imgui.h>
#include <string>
#include <studio/Editor.hpp>
#include <studio/Widgets.hpp>
#include <vector>

namespace studio {

	using engine::ecs::Classes;
	using engine::ecs::Entity;
	using engine::ecs::NULL_ENTITY;
	using engine::ecs::PropertyDescriptor;
	using engine::ecs::Store;
	using engine::game::PropertyValue;
	using engine::world::WorldId;

	namespace {
		// Case-insensitive substring. **Not `FuzzyMatch`**, deliberately: fuzzy
		// ranking is right for "which command did I mean" and wrong for "which
		// of these four hundred parts has this value", where a subsequence match
		// returns most of the scene and ranks the answer somewhere in it.
		bool Contains(std::string_view haystack, std::string_view needle) {
			if (needle.empty()) {
				return true;
			}
			if (needle.size() > haystack.size()) {
				return false;
			}

			const auto lower = [](unsigned char value) { return static_cast<char>(std::tolower(value)); };

			for (size_t start = 0; start + needle.size() <= haystack.size(); start++) {
				bool same = true;
				for (size_t index = 0; index < needle.size(); index++) {
					if (lower(static_cast<unsigned char>(haystack[start + index])) !=
						lower(static_cast<unsigned char>(needle[index]))) {
						same = false;
						break;
					}
				}
				if (same) {
					return true;
				}
			}
			return false;
		}
	}

	bool MatchesQuery(const Store &store, Entity instance, const FindQuery &query, std::string &matched) {
		const engine::ecs::ClassId id = store.ClassOf(instance);
		if (!id.IsValid()) {
			// Not an instance - an entity some module keeps for its own storage.
			// The explorer does not show these and neither does this.
			return false;
		}

		// The class filter is `IsA` rather than an exact name, which is what
		// makes "Part" find every `Part` and "BasePart" find all of them plus
		// anything else derived from it. Set inclusion is what the class tree
		// already means.
		if (!query.Class.empty()) {
			const engine::ecs::ClassId wanted = Classes::Find(engine::core::Name(query.Class));
			if (!wanted.IsValid() || !Classes::IsA(id, wanted)) {
				return false;
			}
		}

		if (!query.Name.empty() && !Contains(Label(store.InstanceNameOf(instance)), query.Name)) {
			return false;
		}

		// No property predicate: the class and name filters have already
		// answered, and requiring a property here would make "every Part" an
		// unanswerable question.
		if (query.Property.empty() && query.Value.empty()) {
			matched.clear();
			return true;
		}

		for (const PropertyDescriptor &descriptor : store.PropertiesOf(instance)) {
			if (!query.Property.empty() && !Contains(Label(descriptor.Name), query.Property)) {
				continue;
			}

			PropertyValue value;
			if (!engine::game::ReadProperty(store, instance, descriptor, value)) {
				// The instance does not carry what this getter reads - an
				// unanchored part asked for a `RigidBody` field. Not a match and
				// not an error.
				continue;
			}

			// Interned text already has stable storage. Formatting every rejected
			// Name allocates a string before the predicate has found any result.
			std::string formatted;
			std::string_view text;
			if (value.Type == engine::ecs::PropertyType::Name ||
				value.Type == engine::ecs::PropertyType::Enum)
				text = value.Name.IsValid() ? value.Name.Text() : std::string_view{};
			else {
				formatted = engine::game::FormatValue(value);
				text = formatted;
			}

			if (query.Value.empty()) {
				// Property named, no value asked for: having the property at all
				// is the match.
				matched = std::string(Label(descriptor.Name)) + " = " + std::string(text);
				return true;
			}

			bool hit = false;
			if (query.Exact) {
				// **Through the property's own type, not through the text.**
				// Exactness over a rendered string would make 0.5 and 0.50 two
				// different answers to the same question.
				PropertyValue wanted;
				std::string reason;
				hit = engine::game::ParseValue(descriptor.Type, query.Value, wanted, reason) &&
					  engine::game::ValuesEqual(value, wanted);
			} else {
				hit = Contains(text, query.Value);
			}

			if (hit) {
				matched = std::string(Label(descriptor.Name)) + " = " + std::string(text);
				return true;
			}
		}

		return false;
	}

	void Editor::RunFind() {
		if (Universe == nullptr) {
			FindResults.clear();
			FindFingerprintReady = false;
			return;
		}
		ENGINE_PROFILE("explorer property search");
		const auto worlds = Universe->Worlds();
		const bool sameQuery = LastFind.Class == Find.Class && LastFind.Name == Find.Name &&
							   LastFind.Property == Find.Property && LastFind.Value == Find.Value &&
							   LastFind.Exact == Find.Exact && LastFindWorld == ExplorerWorld;
		bool cacheable = Find.Property.empty() && Find.Value.empty();
		const auto wanted = Find.Class.empty() ? engine::ecs::ClassId{} : Classes::Find(Name(Find.Class));
		const auto rootId = Classes::Find(Name("Instance"));
		if (!cacheable && rootId.IsValid()) {
			const auto &root = Classes::Describe(rootId);
			const auto canonical =
				std::find_if(root.Properties.begin(), root.Properties.end(), [](const auto &property) {
					return property.Spelling == "Name";
				});
			cacheable = !Find.Property.empty() && canonical != root.Properties.end();
			for (size_t index = 0; cacheable && index < Classes::Count(); ++index) {
				const engine::ecs::ClassId klass{static_cast<uint32_t>(index)};
				if (!Find.Class.empty() && (!wanted.IsValid() || !Classes::IsA(klass, wanted))) continue;
				for (const auto &property : Classes::Describe(klass).Properties) {
					if (!Contains(Label(property.Name), Find.Property)) continue;
					if (property.Name != canonical->Name || property.Get != canonical->Get ||
						property.Kind != engine::ecs::PropertyKind::Field) {
						cacheable = false;
						break;
					}
				}
			}
		}
		uint64_t fingerprint = 1469598103934665603ull;
		const auto fold = [&](uint64_t value) { fingerprint = (fingerprint ^ value) * 1099511628211ull; };
		if (cacheable) {
			fold(Classes::Count());
			for (const WorldId world : worlds) {
				if (ExplorerWorld.IsValid() && ExplorerWorld != world) continue;
				fold(world.Index);
				Universe->Enter(world, [&](Store &store) {
					fold(reinterpret_cast<uintptr_t>(&store));
					store.EachEntity([&](Entity entity) {
						const auto klass = store.ClassOf(entity);
						fold(entity.Id);
						fold(store.InstanceNameOf(entity).Id());
						fold(klass.Index);
					});
				});
			}
		}
		if (cacheable && sameQuery && FindFingerprintReady && fingerprint == FindFingerprint) return;
		LastFind = Find;
		LastFindWorld = ExplorerWorld;
		FindFingerprint = fingerprint;
		FindFingerprintReady = cacheable;
		FindResults.clear();
		FindUniformRows = true;
		FindRows.Dirty = true;

		// **Bounded, and it says so when it stops.** A predicate that matches
		// everything in a large place would otherwise build a list nobody can
		// scroll and cost a frame doing it. Truncation is reported rather than
		// silent - a result list that quietly stops is one somebody trusts to be
		// complete.
		FindTruncated = false;

		for (const WorldId world : worlds) {
			if (ExplorerWorld.IsValid() && ExplorerWorld != world) {
				continue;
			}
			Universe->Enter(world, [&](Store &store) {
				store.EachEntity([&](Entity instance) {
					if (FindResults.size() >= FIND_LIMIT) {
						FindTruncated = true;
						return;
					}

					std::string matched;
					if (!MatchesQuery(store, instance, Find, matched)) {
						return;
					}

					FindResult result;
					result.World = world;
					result.Instance = instance;
					result.Name = Label(store.InstanceNameOf(instance));
					result.Class = Label(Classes::Describe(store.ClassOf(instance)).Name);
					result.Matched = std::move(matched);
					FindUniformRows = FindUniformRows && result.Name.find('\n') == std::string::npos &&
									  result.Class.find('\n') == std::string::npos;
					FindResults.push_back(std::move(result));
				});
			});
		}
	}

}
