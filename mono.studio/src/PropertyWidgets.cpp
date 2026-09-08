#include "PropertyWidgets.hpp"

#include <engine/ecs/EnumTable.hpp>

#include <cmath>
#include <imgui.h>
#include <studio/Widgets.hpp>
#include <vector>

namespace studio {
	using engine::core::Name;
	using engine::ecs::Entity;
	using engine::ecs::FieldDescriptor;
	using engine::ecs::NULL_ENTITY;
	using engine::ecs::PropertyType;
	using engine::ecs::Store;
	using engine::game::FormatValue;
	using engine::game::ParseValue;
	using engine::game::PropertyValue;
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

	bool DrawReference(const Store &store, Entity &reference) {
		const std::string preview = reference == NULL_ENTITY ? "(none)" : store.GetFullName(reference);
		if (!ImGui::BeginCombo("##value", preview.empty() ? "(missing)" : preview.c_str())) return false;
		bool changed = false;
		if (ImGui::Selectable("(none)", reference == NULL_ENTITY)) {
			reference = NULL_ENTITY;
			changed = true;
		}
		std::vector<Entity> candidates;
		store.EachRoot([&](Entity root) { candidates.push_back(root); });
		for (size_t index = 0; index < candidates.size(); ++index) {
			const Entity candidate = candidates[index];
			store.EachChild(candidate, [&](Entity child) { candidates.push_back(child); });
			ImGui::PushID(std::to_string(candidate.Id).c_str());
			if (ImGui::Selectable(store.GetFullName(candidate).c_str(), candidate == reference)) {
				reference = candidate;
				changed = true;
			}
			ImGui::PopID();
		}
		ImGui::EndCombo();
		return changed;
	}

	bool DrawTransform(engine::core::CFrame &frame) {
		float position[]{frame.Position.X, frame.Position.Y, frame.Position.Z};
		bool changed = ImGui::DragFloat3("##position", position, StepFor(position[0]));
		if (changed) frame.Position = {position[0], position[1], position[2]};
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("position");
		const auto angles = frame.ToAngles();
		float degrees[]{glm::degrees(angles.X), glm::degrees(angles.Y), glm::degrees(angles.Z)};
		ImGui::SetNextItemWidth(-1.0f);
		if (ImGui::DragFloat3("##orientation", degrees, 0.5f)) {
			const auto rotated = engine::core::CFrame::Angles(
				glm::radians(degrees[0]), glm::radians(degrees[1]), glm::radians(degrees[2])
			);
			frame = engine::core::CFrame(frame.Position, rotated.Rotation());
			changed = true;
		}
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("orientation in degrees (X, Y, Z)");
		return changed;
	}
	bool DrawSchemaValue(
		const Store &store, const FieldDescriptor &field, PropertyValue &value, std::string &draft
	) {
		switch (field.Type) {
		case PropertyType::Bool:
			return ImGui::Checkbox("##value", &value.Bool);
		case PropertyType::Int32:
			return ImGui::DragInt("##value", &value.Int32);
		case PropertyType::Int64:
			return ImGui::InputScalar("##value", ImGuiDataType_S64, &value.Int64);
		case PropertyType::Float:
			return ImGui::DragFloat("##value", &value.Float, StepFor(value.Float));
		case PropertyType::Double: {
			const double step = static_cast<double>(StepFor(static_cast<float>(value.Double)));
			return ImGui::DragScalar(
				"##value", ImGuiDataType_Double, &value.Double, step, nullptr, nullptr, "%.17g"
			);
		}
		case PropertyType::Vector3: {
			float parts[]{value.Vector3.X, value.Vector3.Y, value.Vector3.Z};
			if (!ImGui::DragFloat3("##value", parts, StepFor(parts[0]))) {
				return false;
			}
			value.Vector3 = engine::core::Vector3{parts[0], parts[1], parts[2]};
			return true;
		}
		case PropertyType::CFrame:
			return DrawTransform(value.CFrame);
		case PropertyType::Color3: {
			float parts[]{value.Color3.R, value.Color3.G, value.Color3.B};
			if (!ImGui::ColorEdit3("##value", parts, ImGuiColorEditFlags_Float)) {
				return false;
			}
			value.Color3 = engine::core::Color3{parts[0], parts[1], parts[2]};
			return true;
		}
		case PropertyType::Vector2: {
			float parts[]{value.Vector2.X, value.Vector2.Y};
			if (!ImGui::DragFloat2("##value", parts, StepFor(parts[0]))) {
				return false;
			}
			value.Vector2 = engine::core::Vector2{parts[0], parts[1]};
			return true;
		}
		case PropertyType::Enum: {
			const char *current = value.Name.IsValid() ? Label(value.Name) : "";
			if (!ImGui::BeginCombo("##value", current)) {
				return false;
			}
			bool changed = false;
			for (const Name member : engine::ecs::EnumTable::MembersOf(field.Enum)) {
				if (ImGui::Selectable(Label(member), member == value.Name)) {
					value.Name = member;
					changed = true;
				}
			}
			ImGui::EndCombo();
			return changed;
		}
		case PropertyType::Name: {
			std::string text = value.Name.IsValid() ? std::string(Label(value.Name)) : std::string{};
			if (!TextField("##value", text)) {
				return false;
			}
			value.Name = text.empty() ? Name{} : Name(text);
			return true;
		}
		case PropertyType::String:
			return TextField("##value", value.String);
		case PropertyType::UDim:
		case PropertyType::UDim2:
		case PropertyType::Rect:
		case PropertyType::NumberRange:
		case PropertyType::NumberSequence:
		case PropertyType::ColorSequence: {
			draft = FormatValue(value);
			TextField("##value", draft);
			if (!ImGui::IsItemDeactivatedAfterEdit()) return false;
			PropertyValue parsed;
			std::string reason;
			if (!ParseValue(field.Type, draft, parsed, reason)) {
				ImGui::SetTooltip("%s", reason.c_str());
				return false;
			}
			value = std::move(parsed);
			draft.clear();
			return true;
		}
		case PropertyType::Reference:
			return DrawReference(store, value.Reference);
		case PropertyType::Opaque:
			ImGui::TextDisabled("read only");
			return false;
		}
		return false;
	}

}
