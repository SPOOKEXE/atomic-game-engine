#include "GraphParams.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <imgui.h>

namespace studio {

	bool DrawGraphParam(const engine::ang::ParamDef &param, engine::ang::Value &value, bool multiline) {
		namespace ang = engine::ang;
		const char *label = param.Label.empty() ? param.Name.c_str() : param.Label.c_str();
		if (param.Widget == "toggle") {
			bool flag = value.Get<bool>() != nullptr && *value.Get<bool>();
			if (!ImGui::Checkbox(label, &flag)) return false;
			value = ang::Value(flag);
			return true;
		}
		if (param.Widget == "select") {
			const std::string current = value.Get<std::string>() != nullptr ? *value.Get<std::string>() : "";
			bool changed = false;
			if (ImGui::BeginCombo(label, current.c_str())) {
				for (const std::string &choice : param.Choices)
					if (ImGui::Selectable(choice.c_str(), current == choice) && choice != current) {
						value = ang::Value(choice);
						changed = true;
					}
				ImGui::EndCombo();
			}
			return changed;
		}
		if (param.Widget == "slider") {
			float number = static_cast<float>(value.AsNumber());
			if (!ImGui::SliderFloat(
					label, &number, static_cast<float>(param.Minimum), static_cast<float>(param.Maximum)
				))
				return false;
			value = ang::Value(static_cast<double>(number));
			return true;
		}
		if (param.Widget == "number") {
			double number = value.AsNumber();
			if (!ImGui::InputDouble(label, &number, 0.0, 0.0, "%.4g")) return false;
			if (param.Maximum > param.Minimum) number = std::clamp(number, param.Minimum, param.Maximum);
			value = ang::Value(number);
			return true;
		}
		std::array<char, 8192> buffer{};
		std::snprintf(
			buffer.data(),
			buffer.size(),
			"%s",
			value.Get<std::string>() != nullptr ? value.Get<std::string>()->c_str() : ""
		);
		const bool changed =
			multiline
				? ImGui::InputTextMultiline(
					  label, buffer.data(), buffer.size(), ImVec2(-1.0f, ImGui::GetTextLineHeight() * 8.0f)
				  )
				: ImGui::InputText(label, buffer.data(), buffer.size());
		if (changed) value = ang::Value(std::string(buffer.data()));
		return changed;
	}
}
