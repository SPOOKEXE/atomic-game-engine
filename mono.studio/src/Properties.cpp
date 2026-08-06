#include <engine/ecs/Classes.hpp>
#include <engine/ecs/EnumTable.hpp>
#include <engine/game/Values.hpp>
#include <engine/ui/Theme.hpp>

#include <algorithm>
#include <imgui.h>
#include <studio/Editor.hpp>
#include <studio/Widgets.hpp>
#include <vector>

namespace studio {

	using engine::core::Name;
	using engine::ecs::ClassId;
	using engine::ecs::Classes;
	using engine::ecs::NULL_ENTITY;
	using engine::ecs::PropertyDescriptor;
	using engine::ecs::PropertyType;
	using engine::ecs::Store;
	using engine::game::FormatValue;
	using engine::game::ParseValue;
	using engine::game::PropertyValue;
	using engine::game::ReadProperty;
	using engine::game::WriteProperty;

	namespace {
		// Which class in an instance's ancestry first declares a property.
		//
		// **This is where the grouping comes from, and it costs nothing to
		// author.** Roblox groups properties by a hand-maintained category on
		// each one; this engine has no such field and adding one would be a
		// second declaration of something the class tree already knows. Walking
		// the ancestry from the root down and asking which class first exposes a
		// name gives "Instance / PVInstance / BasePart / Part" — which is both
		// the right grouping and impossible to forget to update.
		ClassId DeclaringClass(ClassId klass, Name property) {
			const engine::ecs::ClassInfo &info = Classes::Describe(klass);

			// `Ancestry` is nearest first, so walking it backwards is the class
			// tree from the root down — and the first class that has the
			// property is the one that introduced it.
			for (size_t index = info.Ancestry.size(); index > 0; index--) {
				const ClassId candidate = info.Ancestry[index - 1];
				for (const PropertyDescriptor &descriptor : Classes::Describe(candidate).Properties) {
					if (descriptor.Name == property) {
						return candidate;
					}
				}
			}
			return klass;
		}

		// Drags at a rate that suits how big the number already is.
		//
		// A fixed step makes a part's `Transparency` unusable at 0.01 per pixel
		// and its `Position` unusable at 1.0 per pixel. Roblox solves this with
		// per-property increments in the API dump; scaling with the value is the
		// version of that which needs no table.
		float StepFor(float value) {
			const float magnitude = std::abs(value);
			if (magnitude < 1.0f) {
				return 0.005f;
			}
			if (magnitude < 100.0f) {
				return 0.05f;
			}
			return 0.5f;
		}
	}

	void Editor::DrawProperties() {
		if (!ShowProperties) {
			return;
		}

		if (!ImGui::Begin("Properties", &ShowProperties)) {
			ImGui::End();
			return;
		}

		if (Selection.empty() || !SelectionWorld.IsValid()) {
			ImGui::TextDisabled("nothing selected");
			ImGui::End();
			return;
		}

		if (Selection.size() > 1) {
			// **Edits apply to every selected instance.** Stated rather than
			// left to be discovered, because the alternative reading — that it
			// edits the first one — is equally plausible and differs by however
			// many objects somebody just changed.
			ImGui::TextDisabled("%zu selected — an edit applies to all of them", Selection.size());
			ImGui::Separator();
		}

		// The first selected instance is the one whose properties are listed.
		// A multi-selection of different classes shows what the first one has,
		// and a write to a property another one does not have simply fails —
		// `Store::SetProperty` refuses it, and refusing per instance is more
		// honest than showing only the intersection and hiding the rest.
		const Entity primary = Selection.front();

		ImGui::SetNextItemWidth(-1.0f);
		TextField("##property-filter", PropertyFilter, "filter properties");
		ImGui::Separator();

		struct Edit {
			Name Property;
			PropertyValue Value;
			bool Wanted = false;
		};
		Edit edit;

		Universe->Enter(SelectionWorld, [&](Store &store) {
			if (!store.Alive(primary)) {
				ImGui::TextDisabled("the selection is gone");
				return;
			}

			const ClassId klass = store.ClassOf(primary);
			if (!klass.IsValid()) {
				ImGui::TextDisabled("this is not an instance");
				return;
			}

			const engine::ecs::ClassInfo &info = Classes::Describe(klass);

			ImGui::Text("%s", Label(info.Name));
			ImGui::SameLine();
			ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::MutedColour());
			ImGui::Text("in %s", Label(Universe->NameOf(SelectionWorld)));
			ImGui::PopStyleColor();
			ImGui::Separator();

			// Grouped by declaring class, root first, which reads the way the
			// class tree does: Instance's properties, then PVInstance's, then
			// BasePart's.
			std::vector<std::pair<ClassId, std::vector<const PropertyDescriptor *>>> groups;

			for (const PropertyDescriptor &descriptor : info.Properties) {
				if (descriptor.Name == Name("Parent")) {
					// The tree is what says where something is. A `Parent` field
					// beside it would be a second answer to a question the
					// explorer already answers, and rule 2 is about exactly
					// that.
					continue;
				}

				if (!PropertyFilter.empty()) {
					int score = 0;
					if (!FuzzyMatch(PropertyFilter, Label(descriptor.Name), score)) {
						continue;
					}
				}

				const ClassId owner = DeclaringClass(klass, descriptor.Name);

				auto found = std::find_if(groups.begin(), groups.end(), [owner](const auto &group) {
					return group.first == owner;
				});
				if (found == groups.end()) {
					groups.emplace_back(owner, std::vector<const PropertyDescriptor *>{});
					found = groups.end() - 1;
				}
				found->second.push_back(&descriptor);
			}

			// Root-first. `Ancestry` is nearest first, so the group order falls
			// out of how deep each declaring class sits.
			std::sort(groups.begin(), groups.end(), [&](const auto &left, const auto &right) {
				return Classes::Describe(left.first).Ancestry.size() <
					   Classes::Describe(right.first).Ancestry.size();
			});

			for (const auto &group : groups) {
				const engine::ecs::ClassInfo &owner = Classes::Describe(group.first);

				if (!ImGui::CollapsingHeader(Label(owner.Name), ImGuiTreeNodeFlags_DefaultOpen)) {
					continue;
				}

				if (!ImGui::BeginTable(
						Label(owner.Name), 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg
					)) {
					continue;
				}

				ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch, 0.42f);
				ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 0.58f);

				for (const PropertyDescriptor *descriptor : group.second) {
					PropertyValue value;
					const bool readable = ReadProperty(store, primary, *descriptor, value);

					ImGui::TableNextRow();
					ImGui::TableSetColumnIndex(0);
					ImGui::AlignTextToFramePadding();
					ImGui::TextUnformatted(Label(descriptor->Name));

					ImGui::TableSetColumnIndex(1);
					ImGui::PushID(Label(descriptor->Name));
					ImGui::SetNextItemWidth(-1.0f);

					if (!readable) {
						// The instance does not carry what the getter reads.
						// Shown as unavailable rather than hidden, because a
						// property that vanishes from the list when a part is
						// anchored reads as the editor losing it.
						ImGui::TextDisabled("—");
						ImGui::PopID();
						continue;
					}

					const bool locked = !descriptor->Writable;
					ImGui::BeginDisabled(locked);

					PropertyValue changed = value;
					bool wrote = false;

					switch (descriptor->Type) {
						case PropertyType::Bool:
							wrote = ImGui::Checkbox("##v", &changed.Bool);
							break;

						case PropertyType::Int32:
							wrote = ImGui::DragInt("##v", &changed.Int32);
							break;

						case PropertyType::Int64: {
							int narrowed = static_cast<int>(changed.Int64);
							if (ImGui::DragInt("##v", &narrowed)) {
								changed.Int64 = narrowed;
								wrote = true;
							}
							break;
						}

						case PropertyType::Float:
							wrote = ImGui::DragFloat("##v", &changed.Float, StepFor(changed.Float));
							break;

						case PropertyType::Double: {
							auto narrowed = static_cast<float>(changed.Double);
							if (ImGui::DragFloat("##v", &narrowed, StepFor(narrowed))) {
								changed.Double = narrowed;
								wrote = true;
							}
							break;
						}

						case PropertyType::Vector3: {
							float parts[3]{changed.Vector3.X, changed.Vector3.Y, changed.Vector3.Z};
							if (ImGui::DragFloat3("##v", parts, StepFor(parts[0]))) {
								changed.Vector3 = engine::core::Vector3{parts[0], parts[1], parts[2]};
								wrote = true;
							}
							break;
						}

						case PropertyType::Color3: {
							float parts[3]{changed.Color3.R, changed.Color3.G, changed.Color3.B};
							if (ImGui::ColorEdit3("##v", parts, ImGuiColorEditFlags_Float)) {
								changed.Color3 = engine::core::Color3{parts[0], parts[1], parts[2]};
								wrote = true;
							}
							break;
						}

						case PropertyType::CFrame: {
							// Position only. The rotation has its own property —
							// `Orientation`, in degrees, which is what an author
							// actually wants — and offering a raw quaternion
							// beside it would be four numbers nobody can edit by
							// hand next to three that are obvious.
							float parts[3]{
								changed.CFrame.Position.X,
								changed.CFrame.Position.Y,
								changed.CFrame.Position.Z,
							};
							if (ImGui::DragFloat3("##v", parts, StepFor(parts[0]))) {
								changed.CFrame.Position =
									engine::core::Vector3{parts[0], parts[1], parts[2]};
								wrote = true;
							}
							break;
						}

						case PropertyType::Vector2: {
							float parts[2]{changed.Vector2.X, changed.Vector2.Y};
							if (ImGui::DragFloat2("##v", parts, StepFor(parts[0]))) {
								changed.Vector2 = engine::core::Vector2{parts[0], parts[1]};
								wrote = true;
							}
							break;
						}

						case PropertyType::UDim: {
							// **Scale and offset get different steps.** Scale is
							// a fraction, so a drag in whole numbers moves it
							// past the parent in one pixel of mouse travel;
							// offset is pixels and a hundredth of one is a drag
							// that never arrives. Two controls rather than a
							// `DragFloat2` is what buys the two steps.
							float scale = changed.UDim.Scale;
							float offset = changed.UDim.Offset;
							const float half = ImGui::GetContentRegionAvail().x * 0.5f - 2.0f;

							ImGui::SetNextItemWidth(half);
							if (ImGui::DragFloat("##s", &scale, 0.01f)) {
								changed.UDim.Scale = scale;
								wrote = true;
							}
							ImGui::SameLine(0.0f, 4.0f);
							ImGui::SetNextItemWidth(half);
							if (ImGui::DragFloat("##o", &offset, 1.0f, 0.0f, 0.0f, "%.0f")) {
								changed.UDim.Offset = offset;
								wrote = true;
							}
							break;
						}

						case PropertyType::UDim2: {
							// Two rows of the pair above, X then Y — which is
							// the shape Roblox's own property grid uses, and the
							// one an author reading `UDim2.new(0.5, -8, 0, 24)`
							// already has in their head.
							const float half = ImGui::GetContentRegionAvail().x * 0.5f - 2.0f;
							float axes[4]{
								changed.UDim2.X.Scale,
								changed.UDim2.X.Offset,
								changed.UDim2.Y.Scale,
								changed.UDim2.Y.Offset,
							};

							ImGui::SetNextItemWidth(half);
							if (ImGui::DragFloat("##xs", &axes[0], 0.01f)) {
								wrote = true;
							}
							ImGui::SameLine(0.0f, 4.0f);
							ImGui::SetNextItemWidth(half);
							if (ImGui::DragFloat("##xo", &axes[1], 1.0f, 0.0f, 0.0f, "%.0f")) {
								wrote = true;
							}
							ImGui::SetNextItemWidth(half);
							if (ImGui::DragFloat("##ys", &axes[2], 0.01f)) {
								wrote = true;
							}
							ImGui::SameLine(0.0f, 4.0f);
							ImGui::SetNextItemWidth(half);
							if (ImGui::DragFloat("##yo", &axes[3], 1.0f, 0.0f, 0.0f, "%.0f")) {
								wrote = true;
							}

							if (wrote) {
								changed.UDim2 = engine::core::UDim2{axes[0], axes[1], axes[2], axes[3]};
							}
							break;
						}

						case PropertyType::Rect: {
							// Four pixel offsets into an image, so one step and
							// one control. `%.0f` because a fractional texel in
							// a slice centre is an author's typo rather than an
							// intent.
							float parts[4]{
								changed.Rect.Min.X,
								changed.Rect.Min.Y,
								changed.Rect.Max.X,
								changed.Rect.Max.Y,
							};
							if (ImGui::DragFloat4("##v", parts, 1.0f, 0.0f, 0.0f, "%.0f")) {
								changed.Rect = engine::core::Rect{parts[0], parts[1], parts[2], parts[3]};
								wrote = true;
							}
							break;
						}

						case PropertyType::Enum: {
							// **The registered set, not a text field.** That is
							// the whole reason `PropertyType::Enum` exists: a
							// typed `Material = "Plsatic"` is refused where it
							// was written rather than landing in the component
							// and surfacing as a part drawn with the default.
							// A combo makes the typo impossible rather than
							// caught.
							const std::vector<Name> members =
								engine::ecs::EnumTable::MembersOf(descriptor->EnumName);

							const char *current = changed.Name.IsValid() ? Label(changed.Name) : "";
							if (ImGui::BeginCombo("##v", current)) {
								for (const Name member : members) {
									if (ImGui::Selectable(Label(member), member == changed.Name)) {
										changed.Name = member;
										wrote = true;
									}
								}
								ImGui::EndCombo();
							}
							break;
						}

						case PropertyType::Name: {
							std::string text =
								changed.Name.IsValid() ? std::string(Label(changed.Name)) : std::string{};
							if (TextField("##v", text)) {
								changed.Name = text.empty() ? Name{} : Name(text);
								wrote = true;
							}
							break;
						}

						case PropertyType::Reference: {
							// Read-only for now, and it says so rather than
							// offering a control that does nothing. Picking a
							// reference means a target picker over the tree,
							// which is `mono.studio/AGENTS.md`'s deferred list.
							const Name target = changed.Reference == NULL_ENTITY
													? Name{}
													: store.InstanceNameOf(changed.Reference);
							ImGui::TextDisabled(
								"%s", target.IsValid() ? Label(target) : "(none)"
							);
							break;
						}

						case PropertyType::Opaque:
							ImGui::TextDisabled("(not readable as a value)");
							break;
					}

					ImGui::EndDisabled();

					if (wrote && !locked) {
						// Recorded rather than applied. Writing here works for
						// one instance and not for a multi-selection, and doing
						// it in two places is how the two get different rules.
						edit.Property = descriptor->Name;
						edit.Value = changed;
						edit.Wanted = true;
					}

					ImGui::PopID();
				}

				ImGui::EndTable();
			}
		});

		ImGui::End();

		if (!edit.Wanted) {
			return;
		}

		// Applied to every selected instance, in its own `Enter`. A property
		// another instance does not have is refused by `SetProperty` rather
		// than skipped here, which keeps the rule in one place.
		Universe->Enter(SelectionWorld, [&](Store &store) {
			for (const Entity instance : Selection) {
				if (!store.Alive(instance)) {
					continue;
				}

				const ClassId klass = store.ClassOf(instance);
				if (!klass.IsValid()) {
					continue;
				}

				for (const PropertyDescriptor &descriptor : Classes::Describe(klass).Properties) {
					if (descriptor.Name == edit.Property) {
						// **One command per instance, because the write is per
						// instance.** A multi-selection whose members held
						// different values before cannot be reversed by one
						// entry carrying one "before" — undo would give every
						// one of them whatever the first happened to have.
						//
						// `RecordProperty` drops a write that changed nothing,
						// which is what keeps the members that already agreed
						// off the stack.
						PropertyValue before;
						const bool had = ReadProperty(store, instance, descriptor, before);

						if (WriteProperty(store, instance, descriptor, edit.Value) && had &&
							Commands != nullptr) {
							Commands->RecordProperty(
								SelectionWorld,
								instance,
								descriptor.Name,
								before,
								edit.Value,
								"Set " + std::string(Label(descriptor.Name))
							);
						}
						break;
					}
				}
			}
		});

		MarkModified();
	}
}
