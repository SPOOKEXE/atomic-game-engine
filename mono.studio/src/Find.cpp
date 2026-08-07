// Finding instances by what they *are* rather than by what they are called.
//
// **This panel names no property, and that is the whole point.** The properties
// panel is generic because `PropertyDescriptor` is data — a name, a type, and a
// getter — and the Luau binding is generic for the same reason. A search built
// on the same three things inherits the same property: a component declared by
// any module tomorrow is searchable today, with nothing here changing.
//
// The alternative, and the reason this is worth stating: a search with a
// hand-written list of searchable fields is a list that goes stale the first
// time somebody adds a property and forgets it, and the failure is silent — the
// instance simply does not turn up.
//
// **Matching is over `FormatValue`, which is what makes one predicate work for
// every type.** `game::Values.hpp` already renders any property to text for the
// properties panel and the `.agame` writer; comparing against that string means
// `Transparency` `0.5`, `AlphaMode` `Clip` and `Anchored` `true` are all the
// same operation. Exact comparison goes the other way — `ParseValue` into the
// property's own type and `ValuesEqual` — so "0.5" does not match "0.50001"
// when somebody asks for exactness.

#include <engine/ecs/Classes.hpp>
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

			const auto lower = [](unsigned char value) {
				return static_cast<char>(std::tolower(value));
			};

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

	bool MatchesQuery(
		const Store &store,
		Entity instance,
		const FindQuery &query,
		std::string &matched
	) {
		const engine::ecs::ClassId id = store.ClassOf(instance);
		if (!id.IsValid()) {
			// Not an instance — an entity some module keeps for its own storage.
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
				// The instance does not carry what this getter reads — an
				// unanchored part asked for a `RigidBody` field. Not a match and
				// not an error.
				continue;
			}

			const std::string text = engine::game::FormatValue(value);

			if (query.Value.empty()) {
				// Property named, no value asked for: having the property at all
				// is the match.
				matched = std::string(Label(descriptor.Name)) + " = " + text;
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
				matched = std::string(Label(descriptor.Name)) + " = " + text;
				return true;
			}
		}

		return false;
	}

	void Editor::RunFind() {
		FindResults.clear();

		if (Universe == nullptr) {
			return;
		}

		// **Bounded, and it says so when it stops.** A predicate that matches
		// everything in a large place would otherwise build a list nobody can
		// scroll and cost a frame doing it. Truncation is reported rather than
		// silent — a result list that quietly stops is one somebody trusts to be
		// complete.
		FindTruncated = false;

		for (const WorldId world : Universe->Worlds()) {
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
					FindResults.push_back(std::move(result));
				});
			});
		}
	}

	void Editor::DrawFindInstances() {
		if (!ShowFindInstances) {
			return;
		}

		if (!ImGui::Begin("Find Instances", &ShowFindInstances)) {
			ImGui::End();
			return;
		}

		// Re-run on any change rather than behind a button. The search is a walk
		// of the scene and the scene is in memory; making somebody press Enter
		// to see what they have typed is a delay with nothing behind it.
		bool changed = false;

		ImGui::SetNextItemWidth(-1.0f);
		changed |= TextField("##find-class", Find.Class, "class — Part, BasePart, Script");

		ImGui::SetNextItemWidth(-1.0f);
		changed |= TextField("##find-name", Find.Name, "name contains");

		ImGui::SetNextItemWidth(-1.0f);
		changed |= TextField("##find-property", Find.Property, "property — Transparency, Anchored");

		ImGui::SetNextItemWidth(-1.0f);
		changed |= TextField("##find-value", Find.Value, "value contains");

		changed |= ImGui::Checkbox("exact", &Find.Exact);
		ImGui::SameLine();
		ImGui::TextDisabled("(?)");
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip(
				"Compare through the property's own type rather than its text,\n"
				"so 0.5 does not match 0.50001."
			);
		}

		ImGui::SameLine();
		if (ImGui::Button("Refresh")) {
			changed = true;
		}

		// **Every frame while the panel is open, not only on change.** A search
		// is a view of the world and the world moves — a result list that went
		// stale the moment a script deleted something would be a list of dead
		// handles, which is the exact thing `AGENTS.md` says a panel must not
		// cache.
		RunFind();
		(void)changed;

		ImGui::Separator();

		if (FindResults.empty()) {
			ImGui::TextDisabled("nothing matches");
			ImGui::End();
			return;
		}

		ImGui::Text("%zu match%s", FindResults.size(), FindResults.size() == 1 ? "" : "es");
		if (FindTruncated) {
			ImGui::SameLine();
			ImGui::TextColored(ImVec4(0.85f, 0.55f, 0.2f, 1.0f), "· stopped at %zu", FIND_LIMIT);
		}

		// Selecting from here is queued, not applied: `Select` is an action and
		// every action in this program happens outside `Universe::Enter`.
		WorldId pickWorld;
		Entity pick = NULL_ENTITY;
		bool add = false;

		if (ImGui::BeginChild("##find-rows")) {
			for (size_t index = 0; index < FindResults.size(); index++) {
				const FindResult &result = FindResults[index];

				ImGui::PushID(static_cast<int>(index));

				const bool selected = SelectionWorld == result.World && IsSelected(result.Instance);
				if (ImGui::Selectable("##row", selected, ImGuiSelectableFlags_AllowOverlap)) {
					pickWorld = result.World;
					pick = result.Instance;
					add = ImGui::GetIO().KeyCtrl;
				}

				ImGui::SameLine();
				ImGui::TextUnformatted(result.Name.c_str());
				ImGui::SameLine();
				ImGui::TextDisabled("%s", result.Class.c_str());

				if (!result.Matched.empty()) {
					ImGui::SameLine();
					ImGui::TextDisabled("· %s", result.Matched.c_str());
				}

				ImGui::PopID();
			}
		}
		ImGui::EndChild();

		ImGui::End();

		if (pick != NULL_ENTITY) {
			Select(pickWorld, pick, add);
		}
	}
}
