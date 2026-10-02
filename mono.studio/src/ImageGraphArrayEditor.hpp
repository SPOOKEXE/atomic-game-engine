#pragma once
#include <engine/imagegraph/Document.hpp>

#include <algorithm>
#include <imgui.h>

namespace studio::detail {
	using engine::imagegraph::ElementValue;
	using engine::imagegraph::SourceArrayItem;
	inline int ResizeImageGraphArrayText(ImGuiInputTextCallbackData *data) {
		if (data->EventFlag != ImGuiInputTextFlags_CallbackResize) return 0;
		auto *text = static_cast<std::string *>(data->UserData);
		text->resize(size_t(data->BufTextLen));
		data->Buf = text->data();
		return 0;
	}
	inline bool DrawImageGraphArrayLeaf(ElementValue &leaf) {
		using namespace engine::imagegraph;
		if (auto *value = std::get_if<bool>(&leaf)) return ImGui::Checkbox("##value", value);
		if (auto *value = std::get_if<int64_t>(&leaf))
			return ImGui::InputScalar("##value", ImGuiDataType_S64, value);
		if (auto *value = std::get_if<double>(&leaf))
			return ImGui::InputDouble("##value", value, 0.01, 1., "%.17g");
		if (auto *value = std::get_if<std::string>(&leaf))
			return ImGui::InputText(
				"##value",
				value->data(),
				value->capacity() + 1,
				ImGuiInputTextFlags_CallbackResize,
				ResizeImageGraphArrayText,
				value
			);
		if (auto *value = std::get_if<Colour>(&leaf)) {
			float colour[]{
				value->Red / 255.f, value->Green / 255.f, value->Blue / 255.f, value->Alpha / 255.f
			};
			if (!ImGui::ColorEdit4("##value", colour, ImGuiColorEditFlags_NoInputs)) return false;
			auto byte = [](float channel) { return uint8_t(std::clamp(channel, 0.f, 1.f) * 255.f + .5f); };
			*value = {byte(colour[0]), byte(colour[1]), byte(colour[2]), byte(colour[3])};
			return true;
		}
		if (auto *value = std::get_if<Vector2>(&leaf)) {
			double xy[]{value->X, value->Y};
			if (!ImGui::InputScalarN("##value", ImGuiDataType_Double, xy, 2, nullptr, nullptr, "%.17g"))
				return false;
			*value = {xy[0], xy[1]};
			return true;
		}
		ImGui::TextUnformatted("Retained source value");
		return false;
	}
	inline size_t ImageGraphArrayItemType(const SourceArrayItem &item) {
		if (std::holds_alternative<std::vector<SourceArrayItem>>(item.Data)) return 6;
		auto *leaf = std::get_if<ElementValue>(&item.Data);
		if (!leaf) return 7;
		if (std::holds_alternative<bool>(*leaf)) return 0;
		if (std::holds_alternative<int64_t>(*leaf)) return 1;
		if (std::holds_alternative<double>(*leaf)) return 2;
		if (std::holds_alternative<std::string>(*leaf)) return 3;
		if (std::holds_alternative<engine::imagegraph::Colour>(*leaf)) return 4;
		if (std::holds_alternative<engine::imagegraph::Vector2>(*leaf)) return 5;
		return 7;
	}
	inline SourceArrayItem ImageGraphArrayNewItem(size_t type) {
		using namespace engine::imagegraph;
		switch (type) {
		case 0:
			return {ElementValue{false}};
		case 1:
			return {ElementValue{int64_t{0}}};
		case 2:
			return {ElementValue{0.}};
		case 3:
			return {ElementValue{std::string{}}};
		case 4:
			return {ElementValue{Colour{}}};
		case 5:
			return {ElementValue{Vector2{}}};
		default:
			return {std::vector<SourceArrayItem>{}};
		}
	}
	inline size_t ImageGraphArrayPage(size_t count, size_t &page) {
		constexpr size_t pageSize = 32;
		const size_t last = count ? ((count - 1) / pageSize) * pageSize : 0;
		page = std::min(page, last);
		if (count > pageSize) {
			ImGui::BeginDisabled(page == 0);
			if (ImGui::SmallButton("Prev 32")) page -= pageSize;
			ImGui::EndDisabled();
			ImGui::SameLine();
			ImGui::Text("%zu / %zu", page + 1, count);
			ImGui::SameLine();
			ImGui::BeginDisabled(page == last);
			if (ImGui::SmallButton("Next 32")) page += pageSize;
			ImGui::EndDisabled();
		}
		return std::min(count, page + pageSize);
	}
	// Editing a heterogeneous source array preserves its item order and nested shape.
	inline bool DrawImageGraphArrayItems(
		std::vector<SourceArrayItem> &items, size_t depth, size_t &remaining, size_t *rootPage = nullptr
	) {
		using namespace engine::imagegraph;
		bool changed = false;
		if (depth >= Limits::MaximumArrayDepth || remaining == 0) {
			ImGui::TextUnformatted("Array display limit reached");
			return false;
		}
		constexpr const char *names[]{
			"Boolean", "Integer", "Scalar", "Text", "Colour", "Vector2", "Array", "Retained value"
		};

		auto *storage = ImGui::GetStateStorage();
		const auto pageKey = ImGui::GetID("##arrayPage");
		size_t page = rootPage ? *rootPage : size_t(std::max(0, storage->GetInt(pageKey)));
		const size_t end = ImageGraphArrayPage(items.size(), page);
		if (rootPage)
			*rootPage = page;
		else
			storage->SetInt(pageKey, int(page));
		for (size_t index = page; index < end && remaining; ++index) {
			--remaining;
			ImGui::PushID(int(index));
			auto &item = items[index];
			auto type = ImageGraphArrayItemType(item);
			ImGui::Text("%zu", index);
			ImGui::SameLine();
			ImGui::SetNextItemWidth(115);
			if (ImGui::BeginCombo("##type", names[type])) {
				for (size_t next = 0; next < 7; ++next)
					if (ImGui::Selectable(names[next], next == type) && next != type) {
						item = ImageGraphArrayNewItem(next);
						type = next;
						changed = true;
					}
				ImGui::EndCombo();
			}
			ImGui::SameLine();
			if (ImGui::SmallButton("Remove")) {
				items.erase(items.begin() + std::ptrdiff_t(index));
				changed = true;
				ImGui::PopID();
				break;
			}
			if (auto *leaf = std::get_if<ElementValue>(&item.Data))
				changed = DrawImageGraphArrayLeaf(*leaf) || changed;
			else if (auto *nested = std::get_if<std::vector<SourceArrayItem>>(&item.Data)) {
				if (ImGui::TreeNodeEx(
						"##nested", ImGuiTreeNodeFlags_DefaultOpen, "Array (%zu)", nested->size()
					)) {
					changed = DrawImageGraphArrayItems(*nested, depth + 1, remaining) || changed;
					ImGui::TreePop();
				}
			} else
				ImGui::TextUnformatted("Retained image");
			ImGui::PopID();
		}
		ImGui::BeginDisabled(items.size() >= Limits::MaximumArrayElements || remaining == 0);
		if (ImGui::SmallButton("Add element") && items.size() < Limits::MaximumArrayElements && remaining) {
			items.push_back(ImageGraphArrayNewItem(0));
			changed = true;
		}
		ImGui::EndDisabled();
		return changed;
	}
	inline bool DrawImageGraphSourceArray(engine::imagegraph::ArrayValue &array, size_t &page) {
		using namespace engine::imagegraph;
		bool changed = false;
		size_t remaining = 256;
		if (!array.Items.empty() || array.ElementType == ValueType::Any) {
			// Legacy Any arrays are converted only when the user first edits them.
			if (array.Items.empty() && (!array.Elements.empty() || !array.Nested.empty())) {
				auto staged = std::vector<SourceArrayItem>{};
				staged.reserve(array.Elements.size());
				for (const auto &leaf : array.Elements)
					staged.push_back({leaf});
				for (const auto &row : array.Nested) {
					std::vector<SourceArrayItem> nested;
					nested.reserve(row.size());
					for (const auto &leaf : row)
						nested.push_back({leaf});
					staged.push_back({std::move(nested)});
				}
				changed = DrawImageGraphArrayItems(staged, 0, remaining, &page);
				if (changed) {
					array.Items = std::move(staged);
					array.Elements.clear();
					array.Nested.clear();
				}
			} else
				changed = DrawImageGraphArrayItems(array.Items, 0, remaining, &page);
		} else {
			const size_t rowsEnd = ImageGraphArrayPage(array.Nested.size(), page);
			for (size_t row = page; row < rowsEnd && remaining; ++row) {
				ImGui::PushID(int(row));
				if (ImGui::TreeNodeEx(
						"##row",
						ImGuiTreeNodeFlags_DefaultOpen,
						"Array %zu (%zu)",
						row,
						array.Nested[row].size()
					)) {
					auto *storage = ImGui::GetStateStorage();
					const auto pageKey = ImGui::GetID("##rowPage");
					size_t rowPage = size_t(std::max(0, storage->GetInt(pageKey)));
					const size_t end = ImageGraphArrayPage(array.Nested[row].size(), rowPage);
					storage->SetInt(pageKey, int(rowPage));
					for (size_t index = rowPage; index < end && remaining; ++index) {
						--remaining;
						ImGui::PushID(int(index));
						changed = DrawImageGraphArrayLeaf(array.Nested[row][index]) || changed;
						ImGui::PopID();
					}
					ImGui::TreePop();
				}
				ImGui::PopID();
			}
		}
		return changed;
	}
}
