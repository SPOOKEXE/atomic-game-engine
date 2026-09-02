#include <engine/core/Clock.hpp>
#include <engine/core/Log.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/ecs/Schema.hpp>
#include <engine/gui/Components.hpp>
#include <engine/gui/Typing.hpp>
#include <engine/scripthost/Runtime.hpp>
#include <engine/ui/GuiPainter.hpp>
#include <engine/ui/Metrics.hpp>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <exception>
#include <fstream>
#include <imgui.h>
#include <imgui_internal.h>
#include <limits>
#include <nlohmann/json.hpp>
#include <sstream>
#include <studio/Config.hpp>
#include <studio/Editor.hpp>
#include <studio/Plugins.hpp>
#include <tuple>
#include <unordered_map>

namespace studio {

	using engine::ecs::Entity;
	using engine::ecs::Store;

	namespace {
		using nlohmann::json;

		// Whether a relative path stays inside the folder it is relative to.
		//
		// **The one piece of path handling here that is a decision.** A manifest
		// is a file somebody downloaded, and a `main` of `../../../../etc/passwd`
		// is that file reading one outside its plugin. Checked on the *lexical*
		// path rather than on the resolved one, so a folder that does not exist
		// yet is still refused rather than passing because nothing resolved.
		bool StaysInside(const std::string &relative) {
			if (relative.empty()) {
				return false;
			}

			const std::filesystem::path path(relative);
			if (path.is_absolute()) {
				return false;
			}

			int depth = 0;
			for (const std::filesystem::path &part : path) {
				if (part == "..") {
					depth--;
					if (depth < 0) {
						return false;
					}
				} else if (part != "." && !part.empty()) {
					depth++;
				}
			}
			return true;
		}

		bool ReadWhole(const std::filesystem::path &path, std::string &out) {
			std::ifstream in(path, std::ios::binary);
			if (!in) {
				return false;
			}

			std::ostringstream buffer;
			buffer << in.rdbuf();
			out = buffer.str();
			return true;
		}

		bool WriteWhole(const std::filesystem::path &path, std::string_view text, std::string &error) {
			std::error_code failed;
			if (!path.parent_path().empty()) {
				std::filesystem::create_directories(path.parent_path(), failed);
				if (failed) {
					error = "could not create " + path.parent_path().string();
					return false;
				}
			}

			const std::filesystem::path temporary = path.string() + ".tmp";
			{
				std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
				if (!out) {
					error = "could not write " + temporary.string();
					return false;
				}
				out.write(text.data(), static_cast<std::streamsize>(text.size()));
				if (!out) {
					error = "could not finish " + temporary.string();
					return false;
				}
			}

			std::filesystem::rename(temporary, path, failed);
			if (!failed) {
				return true;
			}

			// Windows does not replace an existing file with `rename`. The old
			// document stays valid until the complete temporary file exists.
			std::filesystem::remove(path, failed);
			failed.clear();
			std::filesystem::rename(temporary, path, failed);
			if (failed) {
				error = "could not replace " + path.string();
				return false;
			}
			return true;
		}

		bool LoadPluginState(
			const std::filesystem::path &path,
			std::map<std::string, bool, std::less<>> &enabled,
			std::string &error
		) {
			std::string text;
			if (!ReadWhole(path, text)) {
				error = "could not read " + path.string();
				return false;
			}

			const json document = json::parse(text, nullptr, false);
			if (document.is_discarded() || !document.is_object()) {
				error = "plugin state is not a JSON object";
				return false;
			}

			enabled.clear();
			for (const auto &[id, value] : document.items()) {
				if (value.is_boolean()) {
					enabled[id] = value.get<bool>();
				}
			}
			error.clear();
			return true;
		}

		bool SavePluginState(
			const std::filesystem::path &path,
			const std::map<std::string, bool, std::less<>> &enabled,
			std::string &error
		) {
			json document = json::object();
			for (const auto &[id, value] : enabled) {
				document[id] = value;
			}
			return WriteWhole(path, document.dump(2) + "\n", error);
		}

		// The language a plugin's entry file is written in.
		//
		// **From the extension, which is the same rule the Rojo sync uses one
		// file over.** A `.js` plugin and a `.luau` one are the same surface in
		// two languages, and the engine already runs both.
		engine::script::Language LanguageOf(const std::filesystem::path &main) {
			const std::string extension = main.extension().string();
			return extension == ".js" || extension == ".mjs" ? engine::script::Language::JavaScript
															 : engine::script::Language::Luau;
		}
	}

	const char *Describe(PluginDock dock) {
		switch (dock) {
		case PluginDock::Floating:
			return "Floating";
		case PluginDock::Centre:
			return "Centre";
		case PluginDock::Left:
			return "Left";
		case PluginDock::Right:
			return "Right";
		case PluginDock::Bottom:
			return "Bottom";
		}
		return "Floating";
	}

	std::optional<PluginDock> ParsePluginDock(std::string_view text) {
		if (text == "Floating" || text == "floating") {
			return PluginDock::Floating;
		}
		if (text == "Centre" || text == "Center" || text == "centre" || text == "center") {
			return PluginDock::Centre;
		}
		if (text == "Left" || text == "left") {
			return PluginDock::Left;
		}
		if (text == "Right" || text == "right") {
			return PluginDock::Right;
		}
		if (text == "Bottom" || text == "bottom") {
			return PluginDock::Bottom;
		}
		return std::nullopt;
	}

	const char *Describe(PluginToolbarPlacement placement) {
		switch (placement) {
		case PluginToolbarPlacement::Tabbed:
			return "Tabbed";
		case PluginToolbarPlacement::Pinned:
			return "Pinned";
		}
		return "Tabbed";
	}

	std::optional<PluginToolbarPlacement> ParsePluginToolbarPlacement(std::string_view text) {
		if (text == "Tabbed" || text == "tabbed") {
			return PluginToolbarPlacement::Tabbed;
		}
		if (text == "Pinned" || text == "pinned") {
			return PluginToolbarPlacement::Pinned;
		}
		return std::nullopt;
	}

	const char *Describe(PluginRunTarget target) {
		switch (target) {
		case PluginRunTarget::Studio:
			return "studio";
		case PluginRunTarget::PlaytestServer:
			return "playtest-server";
		case PluginRunTarget::PlaytestClient:
			return "playtest-client";
		}
		return "studio";
	}

	std::optional<PluginRunTarget> ParsePluginRunTarget(std::string_view text) {
		if (text == "studio") {
			return PluginRunTarget::Studio;
		}
		if (text == "playtest-server" || text == "server") {
			return PluginRunTarget::PlaytestServer;
		}
		if (text == "playtest-client" || text == "client") {
			return PluginRunTarget::PlaytestClient;
		}
		return std::nullopt;
	}

	float ClampPluginToolWidth(float width) {
		if (!std::isfinite(width)) {
			return 92.0f;
		}
		return std::clamp(width, PLUGIN_TOOL_MINIMUM_WIDTH, PLUGIN_TOOL_MAXIMUM_WIDTH);
	}

	std::string PluginIdentity(const PluginPresentation &plugin) {
		if (!plugin.Manifest.Id.empty()) {
			return plugin.Manifest.Id;
		}
		if (!plugin.Root.filename().empty()) {
			return plugin.Root.filename().string();
		}
		return plugin.Manifest.Name;
	}

	std::string PluginWidgetLabel(const PluginPresentation &plugin, const PluginWidget &widget) {
		return widget.Title + "###plugin." + PluginIdentity(plugin) + "." + widget.Id;
	}

	static void BlockCppOwnedPluginIdentities(
		std::vector<LoadedPlugin> &scripts, const std::vector<CppPluginDefinition> &native
	) {
		for (LoadedPlugin &script : scripts) {
			const auto duplicate = std::find_if(native.begin(), native.end(), [&](const auto &definition) {
				return definition.Manifest.Id == PluginIdentity(script);
			});
			if (duplicate == native.end()) {
				continue;
			}
			script.Error = "plugin id is already owned by C++ plugin '" + duplicate->Manifest.Name + "'";
			script.DefinitionValid = false;
		}
	}

	std::string
	PluginToolbarKey(const PluginPresentation &plugin, const PluginToolbar &toolbar, size_t index) {
		const std::string pluginId = PluginIdentity(plugin);
		const std::string toolbarId = toolbar.Id.empty() ? std::to_string(index + 1) : toolbar.Id;
		return std::to_string(pluginId.size()) + ":" + pluginId + std::to_string(toolbarId.size()) + ":" +
			   toolbarId;
	}

	std::string PluginToolKey(
		const PluginPresentation &plugin,
		const PluginToolbar &toolbar,
		size_t toolbarIndex,
		const PluginButton &button,
		size_t itemIndex
	) {
		const std::string toolbarKey = PluginToolbarKey(plugin, toolbar, toolbarIndex);
		const std::string itemId = button.Id.empty() ? std::to_string(itemIndex + 1) : button.Id;
		return toolbarKey + std::to_string(itemId.size()) + ":" + itemId;
	}

	static const char *LegacyBuiltinToolId(BuiltinStudioTool tool) {
		switch (tool) {
		case BuiltinStudioTool::SelectMode:
		case BuiltinStudioTool::MoveMode:
		case BuiltinStudioTool::RotateMode:
		case BuiltinStudioTool::ScaleMode:
			return "transform";
		case BuiltinStudioTool::SnapToggle:
		case BuiltinStudioTool::SnapDistance:
		case BuiltinStudioTool::SnapDegrees:
		case BuiltinStudioTool::ScaleFaces:
			return "snap";
		case BuiltinStudioTool::Anchor:
		case BuiltinStudioTool::Lock:
		case BuiltinStudioTool::Align:
		case BuiltinStudioTool::Facing:
			return "selection";
		case BuiltinStudioTool::EditPivot:
		case BuiltinStudioTool::ResetPivot:
		case BuiltinStudioTool::PivotNotice:
			return "pivot";
		case BuiltinStudioTool::Duplicate:
		case BuiltinStudioTool::Delete:
		case BuiltinStudioTool::Deselect:
		case BuiltinStudioTool::Undo:
		case BuiltinStudioTool::Redo:
		case BuiltinStudioTool::SelectionCount:
			return "selection";
		case BuiltinStudioTool::CreateScript:
		case BuiltinStudioTool::CreateLocalScript:
		case BuiltinStudioTool::CreateModuleScript:
		case BuiltinStudioTool::ScriptDestination:
			return "create";
		case BuiltinStudioTool::ScriptEditorPanel:
		case BuiltinStudioTool::DebuggerPanel:
		case BuiltinStudioTool::CommandBarPanel:
			return "panels";
		case BuiltinStudioTool::Grid:
		case BuiltinStudioTool::Particles:
			return "viewport";
		case BuiltinStudioTool::ExplorerPanel:
		case BuiltinStudioTool::PropertiesPanel:
		case BuiltinStudioTool::OutputPanel:
		case BuiltinStudioTool::AssetsPanel:
		case BuiltinStudioTool::StatisticsPanel:
		case BuiltinStudioTool::FrameGraphPanel:
		case BuiltinStudioTool::HeapPanel:
		case BuiltinStudioTool::DatasetEditorPanel:
			return "panels";
		case BuiltinStudioTool::PluginReload:
		case BuiltinStudioTool::PluginManage:
		case BuiltinStudioTool::ToolbarEditor:
		case BuiltinStudioTool::DockWidgetEditor:
		case BuiltinStudioTool::PluginStatus:
			return "manage";
		case BuiltinStudioTool::DemoNodes:
		case BuiltinStudioTool::DemoDescription:
			return "demo";
		default:
			return nullptr;
		}
	}

	static LoadedPlugin *ScriptOwner(PluginPresentation &plugin) {
		return plugin.Native ? nullptr : static_cast<LoadedPlugin *>(&plugin);
	}

	static ToolbarLayoutView ComposeToolbarImpl(
		const std::vector<const PluginPresentation *> &plugins, const ToolbarPreferences &preferences
	) {
		ToolbarLayoutView layout;
		std::vector<bool> tabVisible;
		std::vector<size_t> tabOrder;
		std::unordered_map<std::string, const ToolbarTabPreference *> tabPreferences;
		std::unordered_map<std::string, const ToolbarItemPreference *> itemPreferences;
		std::unordered_map<std::string, size_t> rowOrders;
		std::unordered_map<std::string, size_t> columnOrders;
		std::unordered_map<std::string, PluginToolbarPlacement> placements;
		std::unordered_map<std::string, bool> toolbarVisibility;

		for (const ToolbarTabPreference &preference : preferences.Tabs) {
			if (!preference.Id.empty()) {
				tabPreferences.insert_or_assign(preference.Id, &preference);
				placements.insert_or_assign(preference.Id, preference.Placement);
				toolbarVisibility.insert_or_assign(preference.Id, preference.Visible);
			}
		}
		for (const PluginPresentation *pluginPointer : plugins) {
			const PluginPresentation &plugin = *pluginPointer;
			if (!plugin.Running) {
				continue;
			}
			for (size_t toolbarIndex = 0; toolbarIndex < plugin.Toolbars.size(); toolbarIndex++) {
				const PluginToolbar &toolbar = plugin.Toolbars[toolbarIndex];
				const std::string id = PluginToolbarKey(plugin, toolbar, toolbarIndex);
				if (!placements.contains(id)) {
					placements.emplace(id, toolbar.Placement);
				}
				const auto preference = tabPreferences.find(id);
				const bool visible =
					toolbar.Visible && (preference == tabPreferences.end() || preference->second->Visible);
				toolbarVisibility.insert_or_assign(id, visible);
			}
		}
		for (const ToolbarItemPreference &preference : preferences.Items) {
			if (!preference.Key.empty()) {
				itemPreferences.insert_or_assign(preference.Key, &preference);
			}
		}

		const auto ensureTab = [&](std::string id, std::string name, bool shown, bool userCreated) -> size_t {
			for (size_t index = 0; index < layout.Tabs.size(); index++) {
				if (layout.Tabs[index].Id == id) {
					tabVisible[index] = tabVisible[index] && shown;
					return index;
				}
			}

			size_t order = layout.Tabs.size();
			if (const auto found = tabPreferences.find(id); found != tabPreferences.end()) {
				const ToolbarTabPreference &preference = *found->second;
				if (!preference.Name.empty()) {
					name = preference.Name;
				}
				shown = shown && preference.Visible;
				userCreated = preference.UserCreated;
				order = preference.Order;
			}
			ToolbarTabView tab;
			tab.Id = std::move(id);
			tab.Name = std::move(name);
			tab.UserCreated = userCreated;
			layout.Tabs.push_back(std::move(tab));
			tabVisible.push_back(shown);
			tabOrder.push_back(order);
			return layout.Tabs.size() - 1;
		};

		const auto place = [](std::vector<ToolbarRowView> &rows,
							  std::string rowId,
							  std::string columnId,
							  ToolbarItemLocation location) {
			auto row = std::find_if(rows.begin(), rows.end(), [&](const ToolbarRowView &candidate) {
				return candidate.Id == rowId;
			});
			if (row == rows.end()) {
				rows.push_back(ToolbarRowView{std::move(rowId), {}});
				row = std::prev(rows.end());
			}

			auto cell =
				std::find_if(row->Cells.begin(), row->Cells.end(), [&](const ToolbarCellView &candidate) {
					return candidate.Column == columnId;
				});
			if (cell == row->Cells.end()) {
				row->Cells.push_back(ToolbarCellView{std::move(columnId), {}});
				cell = std::prev(row->Cells.end());
			}
			cell->Items.push_back(std::move(location));
		};

		for (const ToolbarTabPreference &preference : preferences.Tabs) {
			if (preference.UserCreated && !preference.Id.empty() && !preference.Name.empty() &&
				preference.Placement == PluginToolbarPlacement::Tabbed) {
				ensureTab(preference.Id, preference.Name, preference.Visible, true);
			}
		}

		for (size_t pluginIndex = 0; pluginIndex < plugins.size(); pluginIndex++) {
			const PluginPresentation &plugin = *plugins[pluginIndex];
			if (!plugin.Running) {
				continue;
			}

			for (size_t toolbarIndex = 0; toolbarIndex < plugin.Toolbars.size(); toolbarIndex++) {
				const PluginToolbar &toolbar = plugin.Toolbars[toolbarIndex];
				const std::string defaultTab = PluginToolbarKey(plugin, toolbar, toolbarIndex);
				if (toolbar.Rows.empty()) {
					rowOrders.insert_or_assign(defaultTab + "/row-1", 0);
				} else {
					for (size_t row = 0; row < toolbar.Rows.size(); row++) {
						rowOrders.insert_or_assign(defaultTab + "/" + toolbar.Rows[row].Id, row);
					}
				}
				for (size_t column = 0; column < toolbar.Columns.size(); column++) {
					columnOrders.insert_or_assign(defaultTab + "/" + toolbar.Columns[column].Id, column);
				}
				PluginToolbarPlacement placement = toolbar.Placement;
				if (const auto found = tabPreferences.find(defaultTab); found != tabPreferences.end()) {
					placement = found->second->Placement;
				}
				size_t tabIndex = 0;
				if (placement == PluginToolbarPlacement::Tabbed) {
					tabIndex = ensureTab(defaultTab, toolbar.Name, toolbar.Visible, false);
				}

				for (size_t itemIndex = 0; itemIndex < toolbar.Buttons.size(); itemIndex++) {
					const PluginButton &button = toolbar.Buttons[itemIndex];
					const std::string key = PluginToolKey(plugin, toolbar, toolbarIndex, button, itemIndex);
					const auto foundPreference = itemPreferences.find(key);
					const ToolbarItemPreference *preference =
						foundPreference == itemPreferences.end() ? nullptr : foundPreference->second;
					if (preference == nullptr && plugin.Builtin) {
						if (const char *legacyId = LegacyBuiltinToolId(button.Builtin); legacyId != nullptr) {
							PluginButton legacy;
							legacy.Id = legacyId;
							const std::string legacyKey =
								PluginToolKey(plugin, toolbar, toolbarIndex, legacy, itemIndex);
							if (const auto legacyPreference = itemPreferences.find(legacyKey);
								legacyPreference != itemPreferences.end()) {
								preference = legacyPreference->second;
							}
						}
					}

					const bool itemVisible = preference == nullptr ? button.Visible : preference->Visible;
					if (!itemVisible) {
						continue;
					}

					const std::string declaredRow =
						!button.Row.empty() ? button.Row
											: (!toolbar.Rows.empty() ? toolbar.Rows.front().Id : "row-1");
					const std::string declaredColumn =
						!button.Column.empty() ? button.Column
											   : (itemIndex < toolbar.Columns.size()
													  ? toolbar.Columns[itemIndex].Id
													  : "column-" + std::to_string(itemIndex + 1));
					float width = preference == nullptr ? ClampPluginToolWidth(button.Width)
														: ClampPluginToolWidth(preference->Width);
					if (preference == nullptr) {
						const auto declared = std::find_if(
							toolbar.Columns.begin(),
							toolbar.Columns.end(),
							[&](const PluginToolbarTrack &track) { return track.Id == declaredColumn; }
						);
						if (declared != toolbar.Columns.end() && declared->Width > 0.0f) {
							width = ClampPluginToolWidth(declared->Width);
						}
					}
					const std::string row = preference != nullptr && !preference->Row.empty()
												? preference->Row
												: defaultTab + "/" + declaredRow;
					const std::string column = preference != nullptr && !preference->Column.empty()
												   ? preference->Column
												   : defaultTab + "/" + declaredColumn;
					const size_t order = preference == nullptr ? itemIndex : preference->Order;
					ToolbarItemLocation location{
						pluginIndex,
						toolbarIndex,
						itemIndex,
						key,
						width,
						order,
						button.Name + "###control",
					};

					if (preference != nullptr && !preference->Tab.empty() && preference->Tab != defaultTab) {
						const auto target = placements.find(preference->Tab);
						if (target != placements.end() && target->second == PluginToolbarPlacement::Pinned) {
							const auto visible = toolbarVisibility.find(preference->Tab);
							if (visible == toolbarVisibility.end() || visible->second) {
								place(layout.PinnedRows, row, column, std::move(location));
							}
						} else {
							tabIndex = ensureTab(preference->Tab, preference->Tab, true, true);
							place(layout.Tabs[tabIndex].Rows, row, column, std::move(location));
						}
					} else if (placement == PluginToolbarPlacement::Pinned) {
						if (toolbarVisibility.at(defaultTab)) {
							place(layout.PinnedRows, row, column, std::move(location));
						}
					} else {
						place(layout.Tabs[tabIndex].Rows, row, column, std::move(location));
					}
				}
			}
		}

		std::vector<ToolbarTabView> composed;
		std::vector<size_t> composedOrder;
		for (size_t index = 0; index < layout.Tabs.size(); index++) {
			if (tabVisible[index] && (!layout.Tabs[index].Rows.empty() || layout.Tabs[index].UserCreated)) {
				composed.push_back(std::move(layout.Tabs[index]));
				composedOrder.push_back(tabOrder[index]);
			}
		}
		std::vector<size_t> indices(composed.size());
		for (size_t index = 0; index < indices.size(); index++) {
			indices[index] = index;
		}
		std::stable_sort(indices.begin(), indices.end(), [&](size_t left, size_t right) {
			return composedOrder[left] < composedOrder[right];
		});
		layout.Tabs.clear();
		layout.Tabs.reserve(composed.size());
		for (size_t index : indices) {
			layout.Tabs.push_back(std::move(composed[index]));
		}
		const auto sortCells = [&](std::vector<ToolbarRowView> &rows) {
			std::stable_sort(rows.begin(), rows.end(), [&](const auto &left, const auto &right) {
				const auto leftOrder = rowOrders.find(left.Id);
				const auto rightOrder = rowOrders.find(right.Id);
				const size_t leftValue =
					leftOrder == rowOrders.end() ? std::numeric_limits<size_t>::max() : leftOrder->second;
				const size_t rightValue =
					rightOrder == rowOrders.end() ? std::numeric_limits<size_t>::max() : rightOrder->second;
				return leftValue < rightValue;
			});
			for (ToolbarRowView &row : rows) {
				std::stable_sort(
					row.Cells.begin(), row.Cells.end(), [&](const auto &left, const auto &right) {
						const auto leftOrder = columnOrders.find(left.Column);
						const auto rightOrder = columnOrders.find(right.Column);
						const size_t leftValue = leftOrder == columnOrders.end()
													 ? std::numeric_limits<size_t>::max()
													 : leftOrder->second;
						const size_t rightValue = rightOrder == columnOrders.end()
													  ? std::numeric_limits<size_t>::max()
													  : rightOrder->second;
						return leftValue < rightValue;
					}
				);
				for (ToolbarCellView &cell : row.Cells) {
					std::stable_sort(
						cell.Items.begin(), cell.Items.end(), [](const auto &left, const auto &right) {
							return left.Order < right.Order;
						}
					);
				}
			}
		};
		sortCells(layout.PinnedRows);
		for (ToolbarTabView &tab : layout.Tabs) {
			sortCells(tab.Rows);
			tab.Label = tab.Name + "###toolbar." + tab.Id;
			tab.Context = "toolbar-context." + tab.Id;
		}
		size_t tabRows = 0;
		for (const ToolbarTabView &tab : layout.Tabs) {
			tabRows = std::max(tabRows, tab.Rows.size());
		}
		const size_t pinnedRows = layout.PinnedRows.empty() ? 0 : layout.PinnedRows.size() - 1;
		layout.VisualRows = std::max<size_t>(1, 1 + pinnedRows + tabRows);
		return layout;
	}

	ToolbarLayoutView
	ComposeToolbar(const std::vector<LoadedPlugin> &plugins, const ToolbarPreferences &preferences) {
		std::vector<const PluginPresentation *> views;
		views.reserve(plugins.size());
		for (const LoadedPlugin &plugin : plugins) {
			views.push_back(&plugin);
		}
		return ComposeToolbarImpl(views, preferences);
	}

	ToolbarLayoutView
	ComposeToolbar(const std::vector<PluginPresentation *> &plugins, const ToolbarPreferences &preferences) {
		std::vector<const PluginPresentation *> views(plugins.begin(), plugins.end());
		return ComposeToolbarImpl(views, preferences);
	}

	bool
	LoadToolbarPreferences(const std::filesystem::path &path, ToolbarPreferences &out, std::string &error) {
		std::string text;
		if (!ReadWhole(path, text)) {
			error = "could not read " + path.string();
			return false;
		}

		const json document = json::parse(text, nullptr, false);
		if (document.is_discarded() || !document.is_object()) {
			error = "toolbar preferences are not a JSON object";
			return false;
		}

		ToolbarPreferences loaded;
		if (const auto found = document.find("tabs"); found != document.end() && found->is_array()) {
			for (const json &entry : *found) {
				if (!entry.is_object()) {
					continue;
				}
				ToolbarTabPreference tab;
				if (const auto value = entry.find("id"); value != entry.end() && value->is_string()) {
					tab.Id = value->get<std::string>();
				}
				if (const auto value = entry.find("name"); value != entry.end() && value->is_string()) {
					tab.Name = value->get<std::string>();
				}
				if (const auto value = entry.find("visible"); value != entry.end() && value->is_boolean()) {
					tab.Visible = value->get<bool>();
				}
				if (const auto value = entry.find("user"); value != entry.end() && value->is_boolean()) {
					tab.UserCreated = value->get<bool>();
				}
				if (const auto value = entry.find("placement"); value != entry.end() && value->is_string()) {
					if (const auto placement = ParsePluginToolbarPlacement(value->get<std::string>());
						placement.has_value()) {
						tab.Placement = *placement;
					}
				}
				if (const auto value = entry.find("order");
					value != entry.end() && value->is_number_unsigned()) {
					tab.Order = value->get<size_t>();
				}
				if (!tab.Id.empty() && !tab.Name.empty()) {
					loaded.Tabs.push_back(std::move(tab));
				}
			}
		}

		if (const auto found = document.find("items"); found != document.end() && found->is_array()) {
			for (const json &entry : *found) {
				if (!entry.is_object()) {
					continue;
				}
				ToolbarItemPreference item;
				if (const auto value = entry.find("key"); value != entry.end() && value->is_string()) {
					item.Key = value->get<std::string>();
				}
				if (const auto value = entry.find("tab"); value != entry.end() && value->is_string()) {
					item.Tab = value->get<std::string>();
				}
				if (const auto value = entry.find("visible"); value != entry.end() && value->is_boolean()) {
					item.Visible = value->get<bool>();
				}
				if (const auto value = entry.find("width"); value != entry.end() && value->is_number()) {
					item.Width = ClampPluginToolWidth(value->get<float>());
				}
				if (const auto value = entry.find("row"); value != entry.end() && value->is_string()) {
					item.Row = value->get<std::string>();
				}
				if (const auto value = entry.find("column"); value != entry.end() && value->is_string()) {
					item.Column = value->get<std::string>();
				}
				if (const auto value = entry.find("order");
					value != entry.end() && value->is_number_unsigned()) {
					item.Order = value->get<size_t>();
				}
				if (!item.Key.empty()) {
					loaded.Items.push_back(std::move(item));
				}
			}
		}

		out = std::move(loaded);
		error.clear();
		return true;
	}

	bool SaveToolbarPreferences(
		const std::filesystem::path &path, const ToolbarPreferences &preferences, std::string &error
	) {
		json document;
		document["tabs"] = json::array();
		for (const ToolbarTabPreference &tab : preferences.Tabs) {
			if (tab.Id.empty() || tab.Name.empty()) {
				continue;
			}
			document["tabs"].push_back({
				{"id", tab.Id},
				{"name", tab.Name},
				{"visible", tab.Visible},
				{"user", tab.UserCreated},
				{"placement", Describe(tab.Placement)},
				{"order", tab.Order},
			});
		}

		document["items"] = json::array();
		for (const ToolbarItemPreference &item : preferences.Items) {
			if (item.Key.empty()) {
				continue;
			}
			document["items"].push_back(
				{{"key", item.Key},
				 {"tab", item.Tab},
				 {"visible", item.Visible},
				 {"width", ClampPluginToolWidth(item.Width)},
				 {"row", item.Row},
				 {"column", item.Column},
				 {"order", item.Order}}
			);
		}

		return WriteWhole(path, document.dump(2) + "\n", error);
	}

	CppPluginDefinition MakeDefaultStudioPlugin() {
		CppPluginDefinition definition;
		definition.Manifest.Name = "Default Studio";
		definition.Manifest.Description = "The standard Studio toolbar and management surfaces.";
		definition.Manifest.Id = "atomic.default-studio";
		definition.Manifest.Version = "1";
		definition.Manifest.Author = "Atomic Game Engine";
		definition.Manifest.Runs = PluginTarget(PluginRunTarget::Studio);
		definition.Open = [](CppPluginContext &context, std::string &error) {
			if (context.Presentation == nullptr) {
				error = "Default Studio has no presentation";
				return false;
			}
			PluginPresentation &plugin = *context.Presentation;
			plugin.Root = "@builtin/default-studio";
			plugin.Builtin = true;

			const auto addToolbar =
				[&](std::string id, std::string name, PluginToolbarPlacement placement, auto controls) {
					PluginToolbar toolbar;
					toolbar.Name = std::move(name);
					toolbar.Id = std::move(id);
					toolbar.Placement = placement;
					toolbar.Rows.push_back(PluginToolbarTrack{"row-1"});
					for (const auto &[controlId, label, tool] : controls) {
						PluginButton item;
						item.Name = label;
						item.Id = controlId;
						item.Kind = PluginControlKind::Builtin;
						item.Builtin = tool;
						item.Width = PLUGIN_TOOL_MAXIMUM_WIDTH;
						item.Row = "row-1";
						item.Column = controlId;
						toolbar.Columns.push_back(PluginToolbarTrack{controlId});
						toolbar.Buttons.push_back(std::move(item));
					}
					plugin.Toolbars.push_back(std::move(toolbar));
				};

			using Row = std::tuple<const char *, const char *, BuiltinStudioTool>;
			addToolbar(
				"transport",
				"Transport",
				PluginToolbarPlacement::Pinned,
				std::initializer_list<Row>{
					{"play", "Play", BuiltinStudioTool::Play},
					{"play-here", "Play Here", BuiltinStudioTool::PlayHere},
					{"run", "Run", BuiltinStudioTool::Run},
					{"pause", "Pause", BuiltinStudioTool::Pause},
					{"stop", "Stop", BuiltinStudioTool::Stop},
					{"spawn-player", "Spawn Player", BuiltinStudioTool::SpawnPlayer},
					{"remove-player", "Remove Player", BuiltinStudioTool::RemovePlayer},
					{"player-count", "Player Count", BuiltinStudioTool::PlayerCount},
					{"viewport-name", "Viewport Name", BuiltinStudioTool::ViewportName},
					{"scene", "Scene", BuiltinStudioTool::SceneSelector},
					{"world-state", "World State", BuiltinStudioTool::WorldState},
				}
			);
			addToolbar(
				"home",
				"Home",
				PluginToolbarPlacement::Tabbed,
				std::initializer_list<Row>{
					{"insert", "Insert Object", BuiltinStudioTool::InsertObject},
					{"select", "Select", BuiltinStudioTool::SelectMode},
					{"move", "Move", BuiltinStudioTool::MoveMode},
					{"rotate", "Rotate", BuiltinStudioTool::RotateMode},
					{"scale", "Scale", BuiltinStudioTool::ScaleMode},
					{"snap-toggle", "Snap", BuiltinStudioTool::SnapToggle},
					{"snap-distance", "Stud Amount", BuiltinStudioTool::SnapDistance},
					{"snap-degrees", "Rotate Amount", BuiltinStudioTool::SnapDegrees},
					{"scale-faces", "Faces", BuiltinStudioTool::ScaleFaces},
					{"anchor", "Anchor", BuiltinStudioTool::Anchor},
					{"lock", "Lock", BuiltinStudioTool::Lock},
					{"align", "Align", BuiltinStudioTool::Align},
					{"facing", "Facing", BuiltinStudioTool::Facing},
				}
			);
			addToolbar(
				"model",
				"Model",
				PluginToolbarPlacement::Tabbed,
				std::initializer_list<Row>{
					{"edit-pivot", "Edit Pivot", BuiltinStudioTool::EditPivot},
					{"reset-pivot", "Reset Pivot", BuiltinStudioTool::ResetPivot},
					{"pivot-notice", "Pivot Notice", BuiltinStudioTool::PivotNotice},
					{"duplicate", "Duplicate", BuiltinStudioTool::Duplicate},
					{"delete", "Delete", BuiltinStudioTool::Delete},
					{"deselect", "Deselect", BuiltinStudioTool::Deselect},
					{"undo", "Undo", BuiltinStudioTool::Undo},
					{"redo", "Redo", BuiltinStudioTool::Redo},
					{"selection-count", "Selection Count", BuiltinStudioTool::SelectionCount},
				}
			);
			addToolbar(
				"script",
				"Script",
				PluginToolbarPlacement::Tabbed,
				std::initializer_list<Row>{
					{"create-script", "Script", BuiltinStudioTool::CreateScript},
					{"create-local-script", "LocalScript", BuiltinStudioTool::CreateLocalScript},
					{"create-module-script", "ModuleScript", BuiltinStudioTool::CreateModuleScript},
					{"destination", "Script Destination", BuiltinStudioTool::ScriptDestination},
					{"script-editor", "Script Editor", BuiltinStudioTool::ScriptEditorPanel},
					{"debugger", "Debugger", BuiltinStudioTool::DebuggerPanel},
					{"command-bar", "Command Bar", BuiltinStudioTool::CommandBarPanel},
				}
			);
			addToolbar(
				"view",
				"View",
				PluginToolbarPlacement::Tabbed,
				std::initializer_list<Row>{
					{"grid", "Grid", BuiltinStudioTool::Grid},
					{"particles", "Particles", BuiltinStudioTool::Particles},
					{"indicator", "Direction Gizmo", BuiltinStudioTool::ViewportIndicator},
					{"cursor", "3D Cursor", BuiltinStudioTool::Cursor3D},
					{"orbit", "Orbit Around Cursor", BuiltinStudioTool::OrbitAroundCursor},
					{"direction-lock", "Lock Direction", BuiltinStudioTool::DirectionLock},
					{"explorer", "Explorer", BuiltinStudioTool::ExplorerPanel},
					{"properties", "Properties", BuiltinStudioTool::PropertiesPanel},
					{"output", "Output", BuiltinStudioTool::OutputPanel},
					{"assets", "Assets", BuiltinStudioTool::AssetsPanel},
					{"statistics", "Statistics", BuiltinStudioTool::StatisticsPanel},
					{"frame-graph", "Frame Graph", BuiltinStudioTool::FrameGraphPanel},
					{"heap", "Heap", BuiltinStudioTool::HeapPanel},
					{"datasets", "DataStore", BuiltinStudioTool::DatasetEditorPanel},
					{"camera", "Camera Speed", BuiltinStudioTool::CameraSpeed},
				}
			);
			addToolbar(
				"plugins",
				"Plugins",
				PluginToolbarPlacement::Tabbed,
				std::initializer_list<Row>{
					{"reload", "Reload", BuiltinStudioTool::PluginReload},
					{"plugin-manage", "Manage", BuiltinStudioTool::PluginManage},
					{"toolbar", "Toolbar", BuiltinStudioTool::ToolbarEditor},
					{"dock-widgets", "Dock Widgets", BuiltinStudioTool::DockWidgetEditor},
					{"status", "Plugin Status", BuiltinStudioTool::PluginStatus},
				}
			);
			const auto addPanel = [&](std::string id,
									  std::string title,
									  BuiltinStudioPanel panel,
									  PluginDock dock,
									  bool open = true) {
				PluginWidget widget;
				widget.Id = std::move(id);
				widget.Title = std::move(title);
				widget.Open = open;
				widget.SynchronizedOpen = open;
				widget.BuiltinPanel = panel;
				widget.Dock = dock;
				plugin.Widgets.push_back(std::move(widget));
			};
			addPanel("explorer", "Explorer", BuiltinStudioPanel::Explorer, PluginDock::Left);
			addPanel("properties", "Properties", BuiltinStudioPanel::Properties, PluginDock::Right);
			addPanel(
				"component-inspector", "Components", BuiltinStudioPanel::ComponentInspector, PluginDock::Right
			);
			addPanel("script-editor", "Script Editor", BuiltinStudioPanel::ScriptEditor, PluginDock::Centre);
			addPanel(
				"dataset-editor",
				"Dataset Editor",
				BuiltinStudioPanel::DatasetEditor,
				PluginDock::Bottom,
				false
			);
			addPanel(
				"roblox-import", "Roblox Import", BuiltinStudioPanel::RobloxImport, PluginDock::Bottom, false
			);
			error.clear();
			return true;
		};
		return definition;
	}

	bool RegisterSelectionComponent() {
		// A tag: no fields, matched by a query and nothing else. Registering it
		// twice agrees rather than conflicting, which is what makes this
		// callable from wherever the editor happens to reach first.
		const engine::ecs::Schemas::Result result = engine::ecs::Schemas::Register(SELECTED_COMPONENT, {});

		if (result.Why != engine::ecs::Schemas::Status::Ok) {
			ENGINE_WARN("plugins: could not register {}", SELECTED_COMPONENT);
			return false;
		}
		return true;
	}

	std::filesystem::path PluginRoot() {
		return ConfigPath("plugins");
	}

	bool ParsePluginManifest(std::string_view json_, PluginManifest &out, std::string &error) {
		const json document = json::parse(json_, nullptr, false);
		if (document.is_discarded() || !document.is_object()) {
			error = "not a JSON object";
			return false;
		}
		out = PluginManifest{};

		const auto name = document.find("name");
		if (name == document.end() || !name->is_string() || name->get<std::string>().empty()) {
			// **A name is required and the rest is not.** Everything else has a
			// sensible default; a plugin with no name is a row in a list nobody
			// can identify, and a list of "(unnamed)" is worse than a refusal.
			error = "no 'name' - this is not a plugin manifest";
			return false;
		}

		out.Name = name->get<std::string>();

		if (const auto found = document.find("description"); found != document.end() && found->is_string()) {
			out.Description = found->get<std::string>();
		}
		if (const auto found = document.find("main"); found != document.end() && found->is_string()) {
			out.Main = found->get<std::string>();
		}
		if (const auto found = document.find("enabled"); found != document.end() && found->is_boolean()) {
			out.Enabled = found->get<bool>();
		}
		if (const auto found = document.find("id"); found != document.end()) {
			if (!found->is_string() || found->get<std::string>().empty()) {
				error = "'id' has to be non-empty text";
				return false;
			}
			out.Id = found->get<std::string>();
		}
		if (const auto found = document.find("version"); found != document.end() && found->is_string()) {
			out.Version = found->get<std::string>();
		}
		if (const auto found = document.find("author"); found != document.end() && found->is_string()) {
			out.Author = found->get<std::string>();
		}
		if (const auto found = document.find("runs"); found != document.end()) {
			PluginRunTargets targets = 0;
			const auto addTarget = [&](const json &value) {
				if (!value.is_string()) {
					error = "'runs' entries have to be text";
					return false;
				}
				const std::string text = value.get<std::string>();
				if (text == "all") {
					targets |= PluginTarget(PluginRunTarget::Studio) |
							   PluginTarget(PluginRunTarget::PlaytestServer) |
							   PluginTarget(PluginRunTarget::PlaytestClient);
					return true;
				}
				const auto target = ParsePluginRunTarget(text);
				if (!target.has_value()) {
					error = "unknown plugin run target '" + text + "'";
					return false;
				}
				targets |= PluginTarget(*target);
				return true;
			};

			if (found->is_string()) {
				if (!addTarget(*found)) {
					return false;
				}
			} else if (found->is_array()) {
				for (const json &value : *found) {
					if (!addTarget(value)) {
						return false;
					}
				}
			} else {
				error = "'runs' has to be text or an array of text";
				return false;
			}
			if (targets == 0) {
				error = "'runs' needs at least one target";
				return false;
			}
			out.Runs = targets;
		}

		if (!StaysInside(out.Main)) {
			error = "'main' has to name a file inside the plugin's own folder";
			return false;
		}
		return true;
	}

	std::vector<LoadedPlugin> DiscoverPlugins(const std::filesystem::path &root) {
		std::vector<LoadedPlugin> found;

		std::error_code failed;
		if (!std::filesystem::is_directory(root, failed)) {
			// No plugins folder is the ordinary state of a fresh install, not an
			// error to report every time the editor starts.
			return found;
		}

		std::vector<std::filesystem::path> folders;
		for (const auto &entry : std::filesystem::directory_iterator(root, failed)) {
			if (entry.is_directory(failed)) {
				folders.push_back(entry.path());
			}
		}
		if (failed) {
			return found;
		}

		// Sorted, because a directory walk is not ordered and plugins run in
		// this order - one may build on what another left in the world.
		std::sort(folders.begin(), folders.end());

		for (const std::filesystem::path &folder : folders) {
			const std::filesystem::path manifest = folder / "plugin.json";
			if (!std::filesystem::is_regular_file(manifest, failed)) {
				// Somebody's notes, not a broken plugin.
				continue;
			}

			LoadedPlugin plugin;
			plugin.Root = folder;

			std::string text;
			if (!ReadWhole(manifest, text)) {
				plugin.Error = "could not read " + manifest.string();
				plugin.DefinitionValid = false;
				plugin.Manifest.Name = folder.filename().string();
				found.push_back(std::move(plugin));
				continue;
			}

			if (!ParsePluginManifest(text, plugin.Manifest, plugin.Error)) {
				// **Returned rather than skipped.** A folder with no manifest is
				// not a plugin; a folder whose manifest is broken is one, and
				// saying so is the whole point of walking it.
				plugin.DefinitionValid = false;
				plugin.Manifest.Name = folder.filename().string();
				found.push_back(std::move(plugin));
				continue;
			}

			if (plugin.Manifest.Id.empty()) {
				plugin.Manifest.Id = folder.filename().string();
			}

			const auto duplicate = std::find_if(found.begin(), found.end(), [&](const LoadedPlugin &other) {
				return PluginIdentity(other) == plugin.Manifest.Id;
			});
			if (duplicate != found.end()) {
				plugin.Error = "duplicate plugin id '" + plugin.Manifest.Id + "'";
				duplicate->Error = plugin.Error;
				plugin.DefinitionValid = false;
				duplicate->DefinitionValid = false;
			}

			found.push_back(std::move(plugin));
		}

		return found;
	}

	static void StartPlugin(
		LoadedPlugin &plugin,
		engine::ecs::Store &store,
		const std::function<std::unique_ptr<engine::script::HostSurface>(LoadedPlugin &)> &surface,
		PluginRunTarget target,
		engine::world::WorldId world,
		PluginBindingRegistry *bindings
	) {
		plugin.Vm.reset();
		plugin.Surface.reset();
		plugin.Running = false;
		plugin.Faults = 0;
		plugin.OnUndo = {};
		plugin.OnRedo = {};
		plugin.OnRecordingStarted = {};
		plugin.OnRecordingFinished = {};
		plugin.Target = target;
		plugin.World = world;
		plugin.Bindings = bindings;

		if (!plugin.DefinitionValid || !plugin.Error.empty()) {
			return;
		}
		if (!plugin.Manifest.Enabled) {
			plugin.Error = "switched off";
			return;
		}
		if (!RunsIn(plugin.Manifest.Runs, target)) {
			plugin.Error = std::string("does not run in ") + Describe(target);
			return;
		}
		plugin.Toolbars.clear();
		plugin.Widgets.clear();

		const std::filesystem::path main = plugin.Root / plugin.Manifest.Main;
		std::error_code missing;
		if (!std::filesystem::is_regular_file(main, missing)) {
			plugin.Error = "no such file: " + plugin.Manifest.Main;
			return;
		}

		std::string source;
		if (!ReadWhole(main, source)) {
			plugin.Error = "could not read " + plugin.Manifest.Main;
			return;
		}

		plugin.Language = LanguageOf(main);
		engine::script::RuntimeLimits limits;
		limits.Role.Server = target == PluginRunTarget::PlaytestServer;
		limits.Role.Client = target == PluginRunTarget::PlaytestClient;
		limits.Role.Studio = true;
		limits.Origin = engine::script::ScriptOrigin::Plugin;

		plugin.Vm = engine::script::MakeRuntime(store, plugin.Language, limits);
		if (plugin.Vm == nullptr) {
			plugin.Error = "could not start a runtime";
			return;
		}

		if (surface) {
			plugin.Surface = surface(plugin);
			if (plugin.Surface != nullptr) {
				plugin.Vm->SetHost(plugin.Surface.get());
			}
		}

		if (!plugin.Vm->Run(source, plugin.Manifest.Name)) {
			plugin.Error = plugin.Vm->LastError();
			plugin.Vm.reset();
			plugin.Surface.reset();
			return;
		}

		plugin.Running = true;
		plugin.Error.clear();
		ENGINE_INFO("plugin '{}' started", plugin.Manifest.Name);
	}

	void StartPlugins(
		std::vector<LoadedPlugin> &plugins,
		engine::ecs::Store &store,
		const std::function<std::unique_ptr<engine::script::HostSurface>(LoadedPlugin &)> &surface,
		PluginRunTarget target,
		engine::world::WorldId world,
		PluginBindingRegistry *bindings
	) {
		for (LoadedPlugin &plugin : plugins) {
			StartPlugin(plugin, store, surface, target, world, bindings);
		}
	}
	size_t BeatPlugins(std::vector<LoadedPlugin> &plugins, float delta) {
		size_t beaten = 0;

		for (LoadedPlugin &plugin : plugins) {
			if (!plugin.Running || plugin.Vm == nullptr) {
				continue;
			}

			beaten++;
			if (plugin.Vm->Heartbeat(delta)) {
				continue;
			}

			plugin.Faults++;
			plugin.Error = plugin.Vm->LastError();

			// **Switched off rather than logged every frame.** A plugin whose
			// heartbeat raises does it again next frame and every frame after,
			// which is a log nobody can read and a profile nobody can use.
			if (plugin.Faults >= PLUGIN_FAULT_LIMIT) {
				plugin.Running = false;
				ENGINE_ERROR(
					"plugin '{}' switched off after {} fault(s): {}",
					plugin.Manifest.Name,
					plugin.Faults,
					plugin.Error
				);
			}
		}

		return beaten;
	}

	void StartCppPlugins(
		std::vector<LoadedCppPlugin> &plugins,
		const std::vector<CppPluginDefinition> &definitions,
		engine::ecs::Store &store,
		PluginBindingRegistry &bindings,
		PluginRunTarget target,
		engine::world::WorldId world,
		Editor *owner
	) {
		StopCppPlugins(plugins);
		plugins.reserve(definitions.size());

		for (const CppPluginDefinition &definition : definitions) {
			plugins.emplace_back();
			LoadedCppPlugin &plugin = plugins.back();
			plugin.Definition = definition;
			plugin.Manifest = definition.Manifest;
			plugin.Root = "@cpp/" + definition.Manifest.Id;
			plugin.Native = true;
			plugin.Context = CppPluginContext{owner, &store, world, target, &plugin, &plugin.Bindings};

			if (!plugin.Manifest.Enabled) {
				plugin.Error = "switched off";
				continue;
			}
			if (!RunsIn(plugin.Manifest.Runs, target)) {
				plugin.Error = std::string("does not run in ") + Describe(target);
				continue;
			}
			if (!plugin.Definition.Open) {
				plugin.Error = "native plugin has no open function";
				plugin.DefinitionValid = false;
				continue;
			}

			plugin.Bindings = bindings.OpenScope();
			try {
				if (!plugin.Definition.Open(plugin.Context, plugin.Error)) {
					plugin.Bindings.Close();
					continue;
				}
			} catch (const std::exception &failure) {
				plugin.Error = failure.what();
				plugin.Bindings.Close();
				continue;
			} catch (...) {
				plugin.Error = "native plugin raised an unknown exception while opening";
				plugin.Bindings.Close();
				continue;
			}

			plugin.Running = true;
			plugin.Error.clear();
			ENGINE_INFO("C++ plugin '{}' started in {}", plugin.Manifest.Name, Describe(target));
		}
	}

	static void CloseCppPlugin(LoadedCppPlugin &plugin) {
		if (plugin.Running && plugin.Definition.Close) {
			try {
				plugin.Definition.Close(plugin.Context);
			} catch (const std::exception &failure) {
				ENGINE_ERROR("C++ plugin '{}' close failed: {}", plugin.Manifest.Name, failure.what());
			} catch (...) {
				ENGINE_ERROR("C++ plugin '{}' close failed with an unknown exception", plugin.Manifest.Name);
			}
		}
		plugin.Running = false;
		plugin.Bindings.Close();
	}

	size_t BeatCppPlugins(std::vector<LoadedCppPlugin> &plugins, float delta) {
		size_t beaten = 0;
		for (LoadedCppPlugin &plugin : plugins) {
			if (!plugin.Running || !plugin.Definition.Heartbeat) {
				continue;
			}
			beaten++;
			bool ok = false;
			try {
				ok = plugin.Definition.Heartbeat(plugin.Context, delta, plugin.Error);
			} catch (const std::exception &failure) {
				plugin.Error = failure.what();
			} catch (...) {
				plugin.Error = "native plugin raised an unknown exception while beating";
			}
			if (ok) {
				continue;
			}
			plugin.Faults++;
			if (plugin.Faults >= PLUGIN_FAULT_LIMIT) {
				ENGINE_ERROR(
					"C++ plugin '{}' switched off after {} fault(s): {}",
					plugin.Manifest.Name,
					plugin.Faults,
					plugin.Error
				);
				CloseCppPlugin(plugin);
			}
		}
		return beaten;
	}

	void StopCppPlugins(std::vector<LoadedCppPlugin> &plugins) {
		for (auto at = plugins.rbegin(); at != plugins.rend(); ++at) {
			CloseCppPlugin(*at);
		}
		plugins.clear();
	}

	PluginRuntimeSet::~PluginRuntimeSet() {
		Bindings.OnChanged({});
		Scripts.clear();
		StopCppPlugins(Cpp);
	}

	PluginRuntimeSet::PluginRuntimeSet(PluginRunTarget target, WorldId world)
		: World(world), Target(target), Bindings(target) {}

	// --- the editor's half ---------------------------------------------------

	void Editor::StopPlaytestPlugins(WorldId world) {
		for (auto at = PlaytestPluginSets.begin(); at != PlaytestPluginSets.end();) {
			PluginRuntimeSet &set = **at;
			if (set.World != world) {
				at++;
				continue;
			}
			set.Bindings.OnChanged({});
			set.Scripts.clear();
			StopCppPlugins(set.Cpp);
			at = PlaytestPluginSets.erase(at);
		}
	}

	void Editor::StopAllPlaytestPlugins() {
		for (const std::unique_ptr<PluginRuntimeSet> &set : PlaytestPluginSets) {
			set->Bindings.OnChanged({});
			set->Scripts.clear();
			StopCppPlugins(set->Cpp);
		}
		PlaytestPluginSets.clear();
	}

	void Editor::StartPlaytestPlugins(WorldId world, PluginRunTarget target) {
		if (Universe == nullptr || !world.IsValid() || target == PluginRunTarget::Studio) {
			return;
		}
		StopPlaytestPlugins(world);

		auto set = std::make_unique<PluginRuntimeSet>(target, world);

		std::vector<CppPluginDefinition> native = RegisteredCppPlugins();
		set->Scripts = DiscoverPlugins(PluginRoot());
		BlockCppOwnedPluginIdentities(set->Scripts, native);
		std::erase_if(native, [&](const CppPluginDefinition &definition) {
			return !RunsIn(definition.Manifest.Runs, target);
		});
		for (CppPluginDefinition &definition : native) {
			if (const auto found = PluginEnabled.find(definition.Manifest.Id); found != PluginEnabled.end()) {
				definition.Manifest.Enabled = found->second;
			}
		}

		std::erase_if(set->Scripts, [&](const LoadedPlugin &plugin) {
			return !RunsIn(plugin.Manifest.Runs, target);
		});
		for (LoadedPlugin &plugin : set->Scripts) {
			if (const auto found = PluginEnabled.find(PluginIdentity(plugin)); found != PluginEnabled.end()) {
				plugin.Manifest.Enabled = found->second;
			}
		}

		PluginRuntimeSet *runtimeSet = set.get();
		Universe->Enter(world, [this, runtimeSet, &native, world, target](Store &store) {
			StartCppPlugins(runtimeSet->Cpp, native, store, runtimeSet->Bindings, target, world, this);
			StartPlugins(
				runtimeSet->Scripts,
				store,
				[this, &store](LoadedPlugin &plugin) { return MakePluginSurface(*this, plugin, store); },
				target,
				world,
				&runtimeSet->Bindings
			);
		});

		runtimeSet->Bindings.OnChanged([runtimeSet] {
			for (LoadedPlugin &plugin : runtimeSet->Scripts) {
				if (plugin.Vm != nullptr && plugin.Surface != nullptr) {
					plugin.Vm->SetHost(plugin.Surface.get());
				}
			}
		});
		for (const LoadedCppPlugin &plugin : runtimeSet->Cpp) {
			if (!plugin.Running && !plugin.Error.empty()) {
				Say("C++ plugin '" + plugin.Manifest.Name + "' in " + Describe(target) + ": " + plugin.Error,
					engine::core::LogLevel::Warning);
			}
		}
		for (const LoadedPlugin &plugin : runtimeSet->Scripts) {
			if (!plugin.Running && !plugin.Error.empty()) {
				Say("plugin '" + plugin.Manifest.Name + "' in " + Describe(target) + ": " + plugin.Error,
					engine::core::LogLevel::Warning);
			}
		}
		PlaytestPluginSets.push_back(std::move(set));
	}

	void Editor::LoadPlugins() {
		// Every plugin holds a `Store &`. Stopping them before the discovery
		// replaces the list is what stops a runtime outliving the world it was
		// started against.
		std::vector<std::pair<WorldId, PluginRunTarget>> playtestContexts;
		playtestContexts.reserve(PlaytestPluginSets.size());
		for (const std::unique_ptr<PluginRuntimeSet> &set : PlaytestPluginSets) {
			playtestContexts.emplace_back(set->World, set->Target);
		}
		StopAllPlaytestPlugins();
		StudioPluginBindings.OnChanged({});
		Plugins.clear();
		ScriptPlugins.clear();
		StopCppPlugins(CppPlugins);
		PublishedSelection.clear();
		InvalidateToolbarLayout();
		PluginReloader.Reset();
		PluginReloadRoots.clear();
		NextPluginRootScanSeconds = 0.0;

		if (!ToolbarPreferencesLoaded) {
			ToolbarPreferencesLoaded = true;
			std::string error;
			const std::filesystem::path path = ConfigPath("toolbar.json");
			if (std::filesystem::exists(path) && !LoadToolbarPreferences(path, ToolbarPrefs, error)) {
				Say(error, engine::core::LogLevel::Warning);
			}
		}
		if (!PluginStateLoaded) {
			PluginStateLoaded = true;
			std::string error;
			const std::filesystem::path path = ConfigPath("plugins.json");
			if (std::filesystem::exists(path) && !LoadPluginState(path, PluginEnabled, error)) {
				Say(error, engine::core::LogLevel::Warning);
			}
		}

		if (Universe == nullptr || !Active.IsValid()) {
			return;
		}

		RegisterSelectionComponent();
		ScriptPlugins = DiscoverPlugins(PluginRoot());
		for (LoadedPlugin &plugin : ScriptPlugins) {
			if (PluginIdentity(plugin) == "atomic.default-studio") {
				plugin.Error = "plugin id is reserved by Default Studio";
				plugin.DefinitionValid = false;
			}
			if (const auto found = PluginEnabled.find(PluginIdentity(plugin)); found != PluginEnabled.end()) {
				plugin.Manifest.Enabled = found->second;
			}
			PluginReloadRoots.push_back(
				PluginReloadRoot{plugin.Root.lexically_normal().generic_string(), plugin.Root}
			);
		}

		std::vector<CppPluginDefinition> native;
		native.push_back(MakeDefaultStudioPlugin());
		std::vector<CppPluginDefinition> registered = RegisteredCppPlugins();
		native.insert(native.end(), registered.begin(), registered.end());
		BlockCppOwnedPluginIdentities(ScriptPlugins, native);
		for (CppPluginDefinition &definition : native) {
			if (const auto found = PluginEnabled.find(definition.Manifest.Id); found != PluginEnabled.end()) {
				definition.Manifest.Enabled = found->second;
			}
		}

		Universe->Enter(Active, [this, &native](Store &store) {
			StartCppPlugins(
				CppPlugins, native, store, StudioPluginBindings, PluginRunTarget::Studio, Active, this
			);
			StartPlugins(
				ScriptPlugins,
				store,
				[this, &store](LoadedPlugin &plugin) { return MakePluginSurface(*this, plugin, store); },
				PluginRunTarget::Studio,
				Active,
				&StudioPluginBindings
			);
		});

		Plugins.reserve(CppPlugins.size() + ScriptPlugins.size());
		for (LoadedCppPlugin &plugin : CppPlugins) {
			Plugins.push_back(&plugin);
		}
		for (LoadedPlugin &plugin : ScriptPlugins) {
			Plugins.push_back(&plugin);
		}
		StudioPluginBindings.OnChanged([this] {
			for (LoadedPlugin &plugin : ScriptPlugins) {
				if (plugin.Vm != nullptr && plugin.Surface != nullptr) {
					plugin.Vm->SetHost(plugin.Surface.get());
				}
			}
			if (CommandHost.Vm != nullptr && CommandHost.Surface != nullptr) {
				CommandHost.Vm->SetHost(CommandHost.Surface.get());
			}
		});
		SeenCppPluginRegistryRevision = CppPluginRegistryRevision();
		for (const auto &[world, target] : playtestContexts) {
			StartPlaytestPlugins(world, target);
		}

		size_t running = 0;
		for (const PluginPresentation *plugin : Plugins) {
			if (plugin->Running) {
				running++;
				continue;
			}
			if (!RunsIn(plugin->Manifest.Runs, PluginRunTarget::Studio)) {
				continue;
			}
			// Named on the way in rather than only in the panel, because the
			// output pane is where somebody is already looking when a plugin
			// they just installed does nothing.
			Say("plugin '" + plugin->Manifest.Name + "': " + plugin->Error, engine::core::LogLevel::Warning);
		}

		Say("plugins: " + std::to_string(running) + " of " + std::to_string(Plugins.size()) + " running");
	}

	std::string Editor::ActiveWorldName() const {
		if (Universe == nullptr || !Active.IsValid()) {
			return {};
		}
		return std::string(Universe->NameOf(Active).Text());
	}

	bool Editor::HasActiveWorld() const {
		return Universe != nullptr && Active.IsValid();
	}

	void Editor::WithSelectionWorld(const std::function<void(engine::ecs::Store &)> &body) {
		if (Universe == nullptr || !SelectionWorld.IsValid()) {
			// **A no-op rather than a refusal**, because "nothing is open" is an
			// ordinary state for a plugin's heartbeat to run in and a plugin
			// should not have to guard every call against it.
			return;
		}
		Universe->Enter(SelectionWorld, body);
	}

	void Editor::PublishSelection() {
		if (Universe == nullptr || !SelectionWorld.IsValid()) {
			return;
		}

		std::vector<Entity> wanted(Selection.begin(), Selection.end());
		std::sort(wanted.begin(), wanted.end(), [](Entity left, Entity right) { return left.Id < right.Id; });

		// **Nothing written when nothing changed**, which is not a
		// micro-optimisation: a tag written every frame moves
		// `Store::ChangeVersion` every frame, and `physics::SyncBroadphase`
		// reads that counter to decide whether static geometry moved. Publishing
		// unconditionally would rebuild the static index every tick, forever -
		// the exact failure `physics/AGENTS.md` names.
		if (wanted == PublishedSelection) {
			return;
		}

		const engine::ecs::ComponentId id =
			engine::ecs::Components::Find(engine::core::Name(std::string(SELECTED_COMPONENT).c_str()));
		if (!id.IsValid()) {
			return;
		}

		Universe->Enter(SelectionWorld, [&](Store &store) {
			for (const Entity entity : PublishedSelection) {
				if (store.Alive(entity)) {
					store.RemoveComponent(entity, id);
				}
			}
			for (const Entity entity : wanted) {
				if (store.Alive(entity)) {
					store.SetComponent(entity, id, nullptr);
				}
			}
		});

		PublishedSelection = std::move(wanted);
	}

	void Editor::PumpPlugins(float delta) {
		ENGINE_PROFILE_CAT("plugins", engine::core::ProfileCategory::Render);

		if (SeenCppPluginRegistryRevision != CppPluginRegistryRevision()) {
			LoadPlugins();
			return;
		}
		if (Plugins.empty()) {
			return;
		}

		const double nowSeconds = engine::core::Clock::Seconds();
		if (nowSeconds >= NextPluginRootScanSeconds) {
			NextPluginRootScanSeconds = nowSeconds + 0.25;
			std::vector<std::filesystem::path> onDisk;
			std::error_code error;
			for (std::filesystem::directory_iterator entry(PluginRoot(), error), end; !error && entry != end;
				 entry.increment(error)) {
				if (entry->is_directory(error) &&
					std::filesystem::is_regular_file(entry->path() / "plugin.json", error)) {
					onDisk.push_back(entry->path().lexically_normal());
				}
			}
			std::sort(onDisk.begin(), onDisk.end());
			std::vector<std::filesystem::path> loaded;
			loaded.reserve(PluginReloadRoots.size());
			for (const PluginReloadRoot &root : PluginReloadRoots) {
				loaded.push_back(root.Path.lexically_normal());
			}
			std::sort(loaded.begin(), loaded.end());
			if (!error && onDisk != loaded) {
				PluginReloader.RequestRescan(nowSeconds);
			}
		}

		const PluginReloadBatch reload = PluginReloader.Pump(PluginReloadRoots, nowSeconds);
		for (const PluginReloadIssue &issue : reload.Issues) {
			Say("plugin '" + issue.PluginId + "': hot reload could not inspect " + issue.Root.string() +
					": " + issue.Error.message(),
				engine::core::LogLevel::Warning);
		}
		if (reload.Action != PluginReloadAction::None) {
			// One source definition may have Studio, server, and several client
			// instances. Rebuilding all contexts keeps those instances on the
			// same source revision and closes their old bindings first.
			LoadPlugins();
			return;
		}

		// **The selection first, so a plugin's heartbeat sees this frame's.** A
		// plugin that read a selection one frame stale would act on what was
		// selected before the click that ran it.
		PublishSelection();

		const size_t runningBefore = static_cast<size_t>(std::count_if(
			Plugins.begin(), Plugins.end(), [](const PluginPresentation *plugin) { return plugin->Running; }
		));
		size_t beaten = BeatPlugins(ScriptPlugins, delta) + BeatCppPlugins(CppPlugins, delta);
		for (const std::unique_ptr<PluginRuntimeSet> &set : PlaytestPluginSets) {
			beaten += BeatPlugins(set->Scripts, delta);
			beaten += BeatCppPlugins(set->Cpp, delta);
		}
		(void)beaten;
		const size_t runningAfter = static_cast<size_t>(std::count_if(
			Plugins.begin(), Plugins.end(), [](const PluginPresentation *plugin) { return plugin->Running; }
		));
		if (runningAfter != runningBefore) {
			InvalidateToolbarLayout();
		}
	}

	void Editor::InvokePlugin(
		LoadedPlugin &plugin,
		engine::script::HostCallback callback,
		bool drawing,
		engine::script::HostArguments arguments
	) {
		if (!plugin.Running || plugin.Vm == nullptr || !callback.Valid()) {
			return;
		}

		// **The gate is opened around the call and closed after it**, so
		// "am I drawing" is a fact about where a host call came from rather than
		// a promise the plugin makes. A `plugin.Label` from a heartbeat would
		// otherwise draw into whatever window the editor was building.
		if (plugin.Surface != nullptr && drawing) {
			SetPluginDrawing(*plugin.Surface, true);
		}

		const bool ok = plugin.Vm->Invoke(callback, arguments);

		if (plugin.Surface != nullptr && drawing) {
			SetPluginDrawing(*plugin.Surface, false);
		}

		if (ok) {
			return;
		}

		// **A handler that raised is counted with the heartbeat's faults**, and
		// for the same reason: a render callback that throws does it every frame
		// its window is open, which is a log nobody can read.
		plugin.Faults++;
		plugin.Error = plugin.Vm->LastError();

		if (plugin.Faults >= PLUGIN_FAULT_LIMIT) {
			plugin.Running = false;
			InvalidateToolbarLayout();
			Say("plugin '" + plugin.Manifest.Name + "' switched off: " + plugin.Error,
				engine::core::LogLevel::Error);
		}
	}

	void Editor::DrawPluginWidgets() {
		size_t viewportImageSlot = PreviewSlot() + 1;
		for (PluginPresentation *pluginPointer : Plugins) {
			PluginPresentation &plugin = *pluginPointer;
			if (!plugin.Running) {
				continue;
			}

			const size_t widgetCount = plugin.Widgets.size();
			for (size_t widgetIndex = 0; widgetIndex < widgetCount; widgetIndex++) {
				PluginWidget &widget = plugin.Widgets[widgetIndex];
				LoadedPlugin *script = ScriptOwner(plugin);
				if (script != nullptr && Universe != nullptr && script->World.IsValid()) {
					Universe->Enter(script->World, [&](Store &store) {
						if (const engine::gui::Layer *layer = store.Get<engine::gui::Layer>(widget.Gui)) {
							widget.Open = layer->Enabled;
						}
					});
				}
				bool *builtinOpen = nullptr;
				switch (widget.BuiltinPanel) {
				case BuiltinStudioPanel::Explorer:
					builtinOpen = &ShowExplorer;
					break;
				case BuiltinStudioPanel::Properties:
					builtinOpen = &ShowProperties;
					break;
				case BuiltinStudioPanel::ComponentInspector:
					builtinOpen = &ShowComponents;
					break;
				case BuiltinStudioPanel::ScriptEditor:
					builtinOpen = &ShowScripts;
					break;
				case BuiltinStudioPanel::DatasetEditor:
					builtinOpen = &ShowDatasets;
					break;
				case BuiltinStudioPanel::RobloxImport:
					builtinOpen = &ShowRobloxImport;
					break;
				case BuiltinStudioPanel::None:
					break;
				}

				if (builtinOpen != nullptr) {
					// A changed widget value came from the plugin/menu side. Otherwise
					// native actions such as Open Script and toolbar buttons win.
					if (widget.Open != widget.SynchronizedOpen) {
						*builtinOpen = widget.Open;
					} else {
						widget.Open = *builtinOpen;
					}
					widget.SynchronizedOpen = widget.Open;
				}
				if (!widget.Open) {
					widget.GuiRouter.Forget();
					continue;
				}

				// **The plugin's name in the id and not in the title.** Two
				// plugins may both call a panel "Settings", and ImGui keys a
				// window on its whole label - so the id suffix keeps them apart
				// without putting a prefix in front of what a person reads.
				const std::string label = PluginWidgetLabel(plugin, widget);

				const ImVec2 minimum(
					engine::ui::Scaled(std::max(1.0f, widget.MinimumWidth)),
					engine::ui::Scaled(std::max(1.0f, widget.MinimumHeight))
				);
				const ImVec2 maximum(
					widget.MaximumWidth > 0.0f
						? engine::ui::Scaled(std::max(widget.MaximumWidth, widget.MinimumWidth))
						: FLT_MAX,
					widget.MaximumHeight > 0.0f
						? engine::ui::Scaled(std::max(widget.MaximumHeight, widget.MinimumHeight))
						: FLT_MAX
				);
				ImGui::SetNextWindowSizeConstraints(minimum, maximum);

				const char *dockWindow = nullptr;
				switch (widget.Dock) {
				case PluginDock::Centre:
					dockWindow = "Viewport 1";
					break;
				case PluginDock::Left:
					dockWindow = "Explorer";
					break;
				case PluginDock::Right:
					dockWindow = "Properties";
					break;
				case PluginDock::Bottom:
					dockWindow = "Output";
					break;
				case PluginDock::Floating:
					break;
				}
				if (dockWindow != nullptr) {
					if (const ImGuiWindow *target = ImGui::FindWindowByName(dockWindow);
						target != nullptr && target->DockId != 0) {
						ImGui::SetNextWindowDockID(target->DockId, ImGuiCond_FirstUseEver);
					}
				}

				// **Around `Begin` and `End`, not inside them.** A window's
				// background is read at `Begin`, so colours pushed within the
				// window would tint everything in it except the window itself -
				// which reads as a bug in the theme rather than as a widget that
				// was coloured wrong. Same bracket the editor's own panels get
				// from `Editor::Skinned`.
				if (builtinOpen != nullptr) {
					switch (widget.BuiltinPanel) {
					case BuiltinStudioPanel::Explorer:
						Skinned(widget.Title.c_str(), [&] { DrawExplorer(); });
						break;
					case BuiltinStudioPanel::Properties:
						Skinned(widget.Title.c_str(), [&] { DrawProperties(); });
						break;
					case BuiltinStudioPanel::ComponentInspector:
						Skinned(widget.Title.c_str(), [&] { DrawComponents(); });
						break;
					case BuiltinStudioPanel::ScriptEditor:
						Skinned(widget.Title.c_str(), [&] { DrawScripts(); });
						break;
					case BuiltinStudioPanel::DatasetEditor:
						Skinned(widget.Title.c_str(), [&] { DrawDatasets(); });
						break;
					case BuiltinStudioPanel::RobloxImport:
						Skinned(widget.Title.c_str(), [&] { DrawRobloxImport(); });
						break;
					case BuiltinStudioPanel::None:
						break;
					}
					widget.Open = *builtinOpen;
					widget.SynchronizedOpen = widget.Open;
					continue;
				}

				const engine::ui::ScopedColours skin(widget.Colours);
				if (ImGui::Begin(label.c_str(), &widget.Open)) {
					if (widget.NativeRender) {
						widget.NativeRender();
					} else if (script != nullptr && Universe != nullptr && script->World.IsValid()) {
						Universe->Enter(script->World, [&](Store &store) {
							InvokePlugin(*script, widget.Render, true);

							if (!store.Alive(widget.Gui)) {
								widget.GuiRouter.Forget();
								return;
							}

							const ImVec2 origin = ImGui::GetCursorScreenPos();
							const ImVec2 available = ImGui::GetContentRegionAvail();
							const ImVec2 canvas{std::max(available.x, 1.0f), std::max(available.y, 1.0f)};
							ImGui::PushID(static_cast<int>(widgetIndex));
							ImGui::InvisibleButton(
								"##DockWidgetPluginGui", canvas, ImGuiButtonFlags_MouseButtonLeft
							);
							const bool hovered = ImGui::IsItemHovered();
							ImGui::PopID();

							engine::gui::CompileRequest request;
							request.Display.Width = canvas.x;
							request.Display.Height = canvas.y;
							request.Hovered = widget.GuiRouter.Hovered();
							request.Pressed = widget.GuiRouter.Pressed();
							request.Seconds = engine::core::Clock::Seconds();
							widget.GuiList.RebuildCollector(store, widget.Gui, request);

							const size_t renderedViewports = ViewportImages.Render(
								Renderer, store, widget.GuiList.Commands(), viewportImageSlot
							);
							viewportImageSlot += renderedViewports;

							engine::ui::ImageSource images;
							images.Resolve = [this](const engine::core::Name &name) {
								engine::ui::ImageSource::Resolved resolved;
								resolved.Texture =
									reinterpret_cast<ImTextureID>(Renderer.TextureHandle(name));
								uint32_t width = 0;
								uint32_t height = 0;
								(void)Renderer.TextureSize(name, width, height);
								resolved.Size = ImVec2(static_cast<float>(width), static_cast<float>(height));
								const engine::render::FlipbookCell cell =
									Renderer.TextureCell(name, AnimationSeconds);
								resolved.CellMin = ImVec2(cell.OffsetU, cell.OffsetV);
								resolved.CellMax =
									ImVec2(cell.OffsetU + cell.Scale, cell.OffsetV + cell.Scale);
								return resolved;
							};
							images.ResolveViewport = [this](Entity instance) {
								const engine::render::InterfaceImage image = ViewportImages.Resolve(instance);
								engine::ui::ImageSource::Resolved resolved;
								resolved.Texture = reinterpret_cast<ImTextureID>(image.Texture);
								resolved.Size =
									ImVec2(static_cast<float>(image.Width), static_cast<float>(image.Height));
								resolved.CellMax = ImVec2(image.UVMax.X, image.UVMax.Y);
								return resolved;
							};
							(void)engine::ui::PaintGui(
								widget.GuiList.Commands(),
								ImGui::GetWindowDrawList(),
								engine::ui::PaintTarget{origin, 1.0f},
								images
							);

							const ImGuiIO &io = ImGui::GetIO();
							engine::gui::Pointer pointer;
							pointer.Position =
								engine::core::Vector2{io.MousePos.x - origin.x, io.MousePos.y - origin.y};
							pointer.Down = ImGui::IsMouseDown(ImGuiMouseButton_Left);
							pointer.Inside = hovered;
							pointer.Collector = widget.Gui;
							pointer.Wheel = hovered ? io.MouseWheel : 0.0f;
							const std::span<const engine::gui::GuiEvent> routed =
								widget.GuiRouter.Update(store, widget.GuiList.Commands(), pointer);
							std::vector<engine::gui::GuiEvent> events(routed.begin(), routed.end());

							if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
								!io.WantTextInput) {
								engine::gui::Typing typing;
								std::string entered;
								for (const int character : io.InputQueueCharacters) {
									if (character > 0 && character < 0x80) {
										entered.push_back(static_cast<char>(character));
									} else if (character < 0x800) {
										entered.push_back(static_cast<char>(0xC0 | (character >> 6)));
										entered.push_back(static_cast<char>(0x80 | (character & 0x3F)));
									} else if (character < 0x10000) {
										entered.push_back(static_cast<char>(0xE0 | (character >> 12)));
										entered.push_back(
											static_cast<char>(0x80 | ((character >> 6) & 0x3F))
										);
										entered.push_back(static_cast<char>(0x80 | (character & 0x3F)));
									}
								}
								typing.Text = entered;
								typing.Backspace = ImGui::IsKeyPressed(ImGuiKey_Backspace, true);
								typing.Submit = ImGui::IsKeyPressed(ImGuiKey_Enter, false);
								typing.Extend = io.KeyShift;
								if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true)) {
									typing.Caret = -1;
								} else if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, true)) {
									typing.Caret = 1;
								}
								const engine::gui::TypeResult typed = engine::gui::Type(store, typing);
								if (typed.Released) {
									engine::gui::GuiEvent released;
									released.Kind = engine::gui::EventKind::FocusReleased;
									released.Instance = typed.Instance;
									released.Entered = true;
									events.push_back(released);
								}
							}

							if (!events.empty() && script->Vm != nullptr) {
								script->Vm->DeliverGuiEvents(events);
							}
						});
					}
				}
				ImGui::End();
				if (script != nullptr && Universe != nullptr && script->World.IsValid()) {
					Universe->Enter(script->World, [&](Store &store) {
						if (engine::gui::Layer *layer = store.GetMutable<engine::gui::Layer>(widget.Gui)) {
							layer->Enabled = widget.Open;
						}
					});
				}
			}
		}
	}

	void Editor::DrawBuiltinStudioTool(BuiltinStudioTool tool) {
		DrawingBuiltinTool = tool;
		switch (tool) {
		case BuiltinStudioTool::Play:
		case BuiltinStudioTool::PlayHere:
		case BuiltinStudioTool::Run:
		case BuiltinStudioTool::Pause:
		case BuiltinStudioTool::Stop:
		case BuiltinStudioTool::SpawnPlayer:
		case BuiltinStudioTool::RemovePlayer:
		case BuiltinStudioTool::PlayerCount:
		case BuiltinStudioTool::ViewportName:
		case BuiltinStudioTool::SceneSelector:
		case BuiltinStudioTool::WorldState:
			DrawTransportTools();
			break;
		case BuiltinStudioTool::InsertObject:
		case BuiltinStudioTool::SelectMode:
		case BuiltinStudioTool::MoveMode:
		case BuiltinStudioTool::RotateMode:
		case BuiltinStudioTool::ScaleMode:
		case BuiltinStudioTool::SnapToggle:
		case BuiltinStudioTool::SnapDistance:
		case BuiltinStudioTool::SnapDegrees:
		case BuiltinStudioTool::ScaleFaces:
		case BuiltinStudioTool::Anchor:
		case BuiltinStudioTool::Lock:
		case BuiltinStudioTool::Align:
		case BuiltinStudioTool::Facing:
			DrawHomeTools();
			break;
		case BuiltinStudioTool::EditPivot:
		case BuiltinStudioTool::ResetPivot:
		case BuiltinStudioTool::PivotNotice:
		case BuiltinStudioTool::Duplicate:
		case BuiltinStudioTool::Delete:
		case BuiltinStudioTool::Deselect:
		case BuiltinStudioTool::Undo:
		case BuiltinStudioTool::Redo:
		case BuiltinStudioTool::SelectionCount:
			DrawModelTools();
			break;
		case BuiltinStudioTool::CreateScript:
		case BuiltinStudioTool::CreateLocalScript:
		case BuiltinStudioTool::CreateModuleScript:
		case BuiltinStudioTool::ScriptDestination:
		case BuiltinStudioTool::ScriptEditorPanel:
		case BuiltinStudioTool::DebuggerPanel:
		case BuiltinStudioTool::CommandBarPanel:
			DrawScriptTools();
			break;
		case BuiltinStudioTool::Grid:
		case BuiltinStudioTool::Particles:
		case BuiltinStudioTool::ExplorerPanel:
		case BuiltinStudioTool::PropertiesPanel:
		case BuiltinStudioTool::OutputPanel:
		case BuiltinStudioTool::AssetsPanel:
		case BuiltinStudioTool::StatisticsPanel:
		case BuiltinStudioTool::FrameGraphPanel:
		case BuiltinStudioTool::HeapPanel:
		case BuiltinStudioTool::DatasetEditorPanel:
			DrawViewTools();
			break;
		case BuiltinStudioTool::ViewportIndicator:
			DrawViewTools();
			break;
		case BuiltinStudioTool::Cursor3D:
			DrawViewTools();
			break;
		case BuiltinStudioTool::OrbitAroundCursor:
			DrawViewTools();
			break;
		case BuiltinStudioTool::DirectionLock:
			DrawViewTools();
			break;
		case BuiltinStudioTool::CameraSpeed:
			DrawViewTools();
			break;
		case BuiltinStudioTool::PluginReload:
		case BuiltinStudioTool::PluginManage:
		case BuiltinStudioTool::ToolbarEditor:
		case BuiltinStudioTool::DockWidgetEditor:
		case BuiltinStudioTool::PluginStatus:
			DrawPluginTools();
			break;
		case BuiltinStudioTool::DemoNodes:
		case BuiltinStudioTool::DemoDescription:
			DrawDemoTools();
			break;
		case BuiltinStudioTool::None:
			break;
		}
		DrawingBuiltinTool = BuiltinStudioTool::None;
	}

	void Editor::InvalidateToolbarLayout() {
		ToolbarLayoutDirty = true;
	}

	void Editor::DrawPluginToolbar() {
		if (ToolbarLayoutDirty) {
			ToolbarLayout = ComposeToolbar(Plugins, ToolbarPrefs);
			ToolbarLayoutDirty = false;
		}
		const auto savePreferences = [this]() {
			InvalidateToolbarLayout();
			std::string error;
			if (!SaveToolbarPreferences(ConfigPath("toolbar.json"), ToolbarPrefs, error)) {
				Say(error, engine::core::LogLevel::Warning);
			}
		};
		const auto ensureTabPreference = [&](const ToolbarTabView &tab,
											 size_t order) -> ToolbarTabPreference & {
			const auto found = std::find_if(
				ToolbarPrefs.Tabs.begin(),
				ToolbarPrefs.Tabs.end(),
				[&](const ToolbarTabPreference &candidate) { return candidate.Id == tab.Id; }
			);
			if (found != ToolbarPrefs.Tabs.end()) {
				return *found;
			}
			ToolbarPrefs.Tabs.push_back(
				ToolbarTabPreference{
					tab.Id, tab.Name, true, tab.UserCreated, PluginToolbarPlacement::Tabbed, order
				}
			);
			return ToolbarPrefs.Tabs.back();
		};
		const auto addTab = [&]() {
			size_t serial = 1;
			for (;;) {
				const std::string candidate = "user/" + std::to_string(serial++);
				if (std::any_of(ToolbarPrefs.Tabs.begin(), ToolbarPrefs.Tabs.end(), [&](const auto &tab) {
						return tab.Id == candidate;
					})) {
					continue;
				}
				ToolbarPrefs.Tabs.push_back(
					ToolbarTabPreference{
						candidate,
						"New Tab",
						true,
						true,
						PluginToolbarPlacement::Tabbed,
						ToolbarLayout.Tabs.size(),
					}
				);
				ToolbarRenamingTab = candidate;
				std::snprintf(ToolbarRenameDraft, sizeof(ToolbarRenameDraft), "%s", "New Tab");
				savePreferences();
				break;
			}
		};

		if (ToolbarLayout.PinnedRows.empty() && ToolbarLayout.Tabs.empty()) {
			if (ImGui::Button("Manage Plugins")) {
				ShowPlugins = true;
			}
			ImGui::SameLine();
			if (ImGui::Button("Edit Toolbar")) {
				ShowToolbarEditor = true;
			}
			if (ImGui::BeginPopupContextWindow(
					"empty-toolbar-context",
					ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems
				)) {
				if (ImGui::MenuItem("Add Tab")) {
					addTab();
				}
				ImGui::EndPopup();
			}
			return;
		}

		const auto drawItem = [&](const ToolbarItemLocation &location) {
			if (location.Plugin >= Plugins.size()) {
				return;
			}
			PluginPresentation &plugin = *Plugins[location.Plugin];
			if (location.Toolbar >= plugin.Toolbars.size()) {
				return;
			}
			PluginToolbar &toolbar = plugin.Toolbars[location.Toolbar];
			if (location.Item >= toolbar.Buttons.size()) {
				return;
			}
			PluginButton &button = toolbar.Buttons[location.Item];
			const std::string &tooltip = button.Tooltip;

			ImGui::PushID(location.Key.c_str());
			if (button.Kind == PluginControlKind::Builtin) {
				DrawBuiltinStudioTool(button.Builtin);
			} else {
				if (button.Kind == PluginControlKind::Button) {
					const bool pressed =
						button.Active
							? ImGui::Selectable(
								  location.ControlLabel.c_str(), true, 0, ImVec2(location.Width, 0.0f)
							  )
							: ImGui::Button(location.ControlLabel.c_str(), ImVec2(location.Width, 0.0f));
					if (pressed) {
						if (button.NativeOnClick) {
							button.NativeOnClick({});
						} else if (LoadedPlugin *script = ScriptOwner(plugin); script != nullptr) {
							InvokePlugin(*script, button.OnClick, false);
						}
					}
				} else if (button.Kind == PluginControlKind::Toggle) {
					const bool before = button.Active;
					ImGui::Checkbox(location.ControlLabel.c_str(), &button.Active);
					if (before != button.Active) {
						const engine::script::HostValue value = engine::script::HostValue::Of(button.Active);
						const engine::script::HostArguments arguments(&value, 1);
						if (button.NativeOnChanged) {
							button.NativeOnChanged(arguments);
						} else if (LoadedPlugin *script = ScriptOwner(plugin); script != nullptr) {
							InvokePlugin(*script, button.OnChanged, false, arguments);
						}
					}
				} else if (button.Kind == PluginControlKind::Dropdown) {
					ImGui::SetNextItemWidth(location.Width);
					const char *preview = button.Selected < button.Options.size()
											  ? button.Options[button.Selected].c_str()
											  : "(none)";
					if (ImGui::BeginCombo(location.ControlLabel.c_str(), preview)) {
						for (size_t option = 0; option < button.Options.size(); option++) {
							if (!ImGui::Selectable(
									button.Options[option].c_str(), option == button.Selected
								)) {
								continue;
							}
							button.Selected = option;
							const engine::script::HostValue arguments[] = {
								engine::script::HostValue::Of(static_cast<double>(option + 1)),
								engine::script::HostValue::Of(std::string_view(button.Options[option])),
							};
							const engine::script::HostArguments values(arguments, 2);
							if (button.NativeOnChanged) {
								button.NativeOnChanged(values);
							} else if (LoadedPlugin *script = ScriptOwner(plugin); script != nullptr) {
								InvokePlugin(*script, button.OnChanged, false, values);
							}
							break;
						}
						ImGui::EndCombo();
					}
				} else if (button.Kind == PluginControlKind::Label) {
					ImGui::TextUnformatted(button.Name.c_str());
				}
			}

			const bool hovered = ImGui::IsItemHovered();
			const float renderedWidth = ImGui::GetItemRectSize().x;
			if (!tooltip.empty() && hovered) {
				ImGui::SetTooltip("%s", tooltip.c_str());
			}
			if ((button.Kind == PluginControlKind::Toggle || button.Kind == PluginControlKind::Label) &&
				location.Width > renderedWidth) {
				ImGui::SameLine(0.0f, 0.0f);
				ImGui::Dummy(ImVec2(location.Width - renderedWidth, ImGui::GetFrameHeight()));
			}
			ImGui::PopID();
		};

		const auto drawRows = [&](const std::vector<ToolbarRowView> &rows, size_t begin) {
			for (size_t rowIndex = begin; rowIndex < rows.size(); rowIndex++) {
				if (rowIndex > begin) {
					ImGui::NewLine();
				}
				ImGui::PushID(static_cast<int>(rowIndex));
				bool shown = false;
				for (const ToolbarCellView &cell : rows[rowIndex].Cells) {
					for (const ToolbarItemLocation &location : cell.Items) {
						if (shown) {
							ImGui::SameLine();
							ImGui::TextDisabled("|");
							ImGui::SameLine();
						}
						drawItem(location);
						shown = true;
					}
				}
				ImGui::PopID();
			}
		};

		if (!ToolbarLayout.PinnedRows.empty()) {
			const ToolbarRowView &first = ToolbarLayout.PinnedRows.front();
			bool shown = false;
			for (const ToolbarCellView &cell : first.Cells) {
				for (const ToolbarItemLocation &location : cell.Items) {
					if (shown) {
						ImGui::SameLine();
						ImGui::TextDisabled("|");
						ImGui::SameLine();
					}
					drawItem(location);
					shown = true;
				}
			}
			if (shown && !ToolbarLayout.Tabs.empty()) {
				ImGui::SameLine();
				ImGui::TextDisabled("|");
				ImGui::SameLine();
			}
		}

		int selected = -1;
		if (!ToolbarLayout.Tabs.empty() &&
			ImGui::BeginTabBar("ribbon", ImGuiTabBarFlags_FittingPolicyScroll)) {
			for (size_t index = 0; index < ToolbarLayout.Tabs.size(); index++) {
				const ToolbarTabView &tab = ToolbarLayout.Tabs[index];
				if (ImGui::BeginTabItem(tab.Label.c_str())) {
					selected = static_cast<int>(index);
					ImGui::EndTabItem();
				}
				if (ImGui::BeginPopupContextItem(tab.Context.c_str())) {
					for (size_t order = 0; order < ToolbarLayout.Tabs.size(); order++) {
						ensureTabPreference(ToolbarLayout.Tabs[order], order).Order = order;
					}
					ToolbarTabPreference &preference = ensureTabPreference(tab, index);
					if (ToolbarRenamingTab != tab.Id) {
						ToolbarRenamingTab = tab.Id;
						std::snprintf(
							ToolbarRenameDraft, sizeof(ToolbarRenameDraft), "%s", preference.Name.c_str()
						);
					}
					ImGui::SetNextItemWidth(engine::ui::Scaled(180.0f));
					ImGui::InputText("##rename-toolbar-tab", ToolbarRenameDraft, sizeof(ToolbarRenameDraft));
					if (ImGui::MenuItem("Apply Rename", nullptr, false, ToolbarRenameDraft[0] != '\0')) {
						preference.Name = ToolbarRenameDraft;
						savePreferences();
					}

					if (ImGui::MenuItem("Move Left", nullptr, false, index > 0)) {
						ToolbarTabPreference &left =
							ensureTabPreference(ToolbarLayout.Tabs[index - 1], index - 1);
						std::swap(preference.Order, left.Order);
						savePreferences();
					}
					if (ImGui::MenuItem(
							"Move Right", nullptr, false, index + 1 < ToolbarLayout.Tabs.size()
						)) {
						ToolbarTabPreference &right =
							ensureTabPreference(ToolbarLayout.Tabs[index + 1], index + 1);
						std::swap(preference.Order, right.Order);
						savePreferences();
					}
					ImGui::Separator();
					const char *removeLabel = preference.UserCreated ? "Delete Tab" : "Hide Tab";
					if (ImGui::MenuItem(removeLabel)) {
						for (ToolbarItemPreference &item : ToolbarPrefs.Items) {
							if (item.Tab == tab.Id) {
								item.Tab.clear();
							}
						}
						if (preference.UserCreated) {
							ToolbarPrefs.Tabs.erase(
								std::find_if(
									ToolbarPrefs.Tabs.begin(),
									ToolbarPrefs.Tabs.end(),
									[&](const ToolbarTabPreference &candidate) {
										return candidate.Id == tab.Id;
									}
								)
							);
						} else {
							preference.Visible = false;
						}
						savePreferences();
					}
					ImGui::EndPopup();
				}
			}
			ImGui::EndTabBar();
		}
		if (ImGui::BeginPopupContextWindow(
				"toolbar-strip-context", ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems
			)) {
			if (ImGui::MenuItem("Add Tab")) {
				addTab();
			}
			ImGui::EndPopup();
		}

		ImGui::BeginChild("toolbar-rows", ImVec2(0.0f, 0.0f), false, ImGuiWindowFlags_HorizontalScrollbar);
		if (ToolbarLayout.PinnedRows.size() > 1) {
			drawRows(ToolbarLayout.PinnedRows, 1);
		}
		if (selected < 0 && !ToolbarLayout.Tabs.empty()) {
			selected = 0;
		}
		if (selected >= 0) {
			drawRows(ToolbarLayout.Tabs[static_cast<size_t>(selected)].Rows, 0);
		}
		ImGui::EndChild();
	}

	void Editor::DrawPluginTools() {
		const bool all = DrawingBuiltinTool == BuiltinStudioTool::None;
		// **Reload first, and always present.** It is the one control that is
		// useful when *nothing* is running, which is exactly when somebody is on
		// this tab - a plugin they have just written and just fixed.
		if (all || DrawingBuiltinTool == BuiltinStudioTool::PluginReload) {
			if (ImGui::Button("Reload", ImVec2(84.0f, 0.0f))) {
				LoadPlugins();
			}
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip("Restarts Studio and active playtest plugin instances");
			}
		}

		if (all || DrawingBuiltinTool == BuiltinStudioTool::PluginManage) {
			if (all) {
				ImGui::SameLine();
			}
			if (ImGui::Button("Manage", ImVec2(84.0f, 0.0f))) {
				ShowPlugins = true;
			}
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip("%s", PluginRoot().string().c_str());
			}
		}

		if (all) {
			ImGui::SameLine();
			ImGui::TextDisabled("|");
			ImGui::SameLine();
		}
		if (all || DrawingBuiltinTool == BuiltinStudioTool::ToolbarEditor) {
			if (ImGui::Button("Toolbar", ImVec2(84.0f, 0.0f))) {
				ShowToolbarEditor = !ShowToolbarEditor;
			}
		}
		if (all || DrawingBuiltinTool == BuiltinStudioTool::DockWidgetEditor) {
			if (all) {
				ImGui::SameLine();
			}
			if (ImGui::Button("Dock Widgets", ImVec2(104.0f, 0.0f))) {
				ShowDockWidgetEditor = !ShowDockWidgetEditor;
			}
		}
		if (all || DrawingBuiltinTool == BuiltinStudioTool::PluginStatus) {
			if (all) {
				ImGui::SameLine();
			}
			const size_t running = static_cast<size_t>(
				std::count_if(Plugins.begin(), Plugins.end(), [](const PluginPresentation *plugin) {
					return plugin->Running;
				})
			);
			ImGui::TextDisabled("%zu of %zu running", running, Plugins.size());
		}
	}

	void Editor::DrawToolbarEditor() {
		if (!ShowToolbarEditor) {
			return;
		}
		if (!ImGui::Begin("Toolbar Editor", &ShowToolbarEditor)) {
			ImGui::End();
			return;
		}

		const auto save = [this]() {
			InvalidateToolbarLayout();
			std::string error;
			if (!SaveToolbarPreferences(ConfigPath("toolbar.json"), ToolbarPrefs, error)) {
				Say(error, engine::core::LogLevel::Warning);
			}
		};

		ImGui::SetNextItemWidth(engine::ui::Scaled(220.0f));
		ImGui::InputTextWithHint(
			"##new-toolbar-tab", "New tab name", ToolbarTabDraft, sizeof(ToolbarTabDraft)
		);
		ImGui::SameLine();
		if (ImGui::Button("Add Tab") && ToolbarTabDraft[0] != '\0') {
			size_t serial = 1;
			for (;;) {
				const std::string candidate = "user/" + std::to_string(serial++);
				const bool used = std::any_of(
					ToolbarPrefs.Tabs.begin(), ToolbarPrefs.Tabs.end(), [&](const ToolbarTabPreference &tab) {
						return tab.Id == candidate;
					}
				);
				if (used) {
					continue;
				}
				ToolbarPrefs.Tabs.push_back(ToolbarTabPreference{candidate, ToolbarTabDraft, true, true});
				ToolbarTabDraft[0] = '\0';
				save();
				break;
			}
		}
		ImGui::SameLine();
		if (ImGui::Button("Reset")) {
			ToolbarPrefs = ToolbarPreferences{};
			save();
		}

		if (!ToolbarPrefs.Tabs.empty()) {
			ImGui::SeparatorText("Tabs");
			for (size_t index = 0; index < ToolbarPrefs.Tabs.size();) {
				ToolbarTabPreference &tab = ToolbarPrefs.Tabs[index];
				ImGui::PushID(tab.Id.c_str());
				bool changed = ImGui::Checkbox("##visible", &tab.Visible);
				ImGui::SameLine();
				ImGui::TextUnformatted(tab.Name.c_str());
				if (tab.UserCreated) {
					ImGui::SameLine();
				}
				if (tab.UserCreated && ImGui::SmallButton("Remove")) {
					for (ToolbarItemPreference &item : ToolbarPrefs.Items) {
						if (item.Tab == tab.Id) {
							item.Tab.clear();
						}
					}
					ToolbarPrefs.Tabs.erase(ToolbarPrefs.Tabs.begin() + static_cast<std::ptrdiff_t>(index));
					save();
					ImGui::PopID();
					continue;
				}
				if (changed) {
					save();
				}
				ImGui::PopID();
				index++;
			}
		}

		struct TabChoice {
			std::string Id;
			std::string Name;
		};
		std::vector<TabChoice> choices;
		for (const ToolbarTabPreference &tab : ToolbarPrefs.Tabs) {
			choices.push_back(TabChoice{tab.Id, tab.Name});
		}
		for (const PluginPresentation *pluginPointer : Plugins) {
			const PluginPresentation &plugin = *pluginPointer;
			for (size_t toolbarIndex = 0; toolbarIndex < plugin.Toolbars.size(); toolbarIndex++) {
				const PluginToolbar &toolbar = plugin.Toolbars[toolbarIndex];
				const std::string id = PluginToolbarKey(plugin, toolbar, toolbarIndex);
				if (std::none_of(choices.begin(), choices.end(), [&](const TabChoice &choice) {
						return choice.Id == id;
					})) {
					choices.push_back(TabChoice{id, toolbar.Name});
				}
			}
		}

		ImGui::SeparatorText("Tools");
		if (ImGui::BeginTable(
				"toolbar-items",
				4,
				ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY
			)) {
			ImGui::TableSetupColumn("Visible", ImGuiTableColumnFlags_WidthFixed, engine::ui::Scaled(58.0f));
			ImGui::TableSetupColumn("Tool", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn("Tab", ImGuiTableColumnFlags_WidthFixed, engine::ui::Scaled(170.0f));
			ImGui::TableSetupColumn("Width", ImGuiTableColumnFlags_WidthFixed, engine::ui::Scaled(130.0f));
			ImGui::TableHeadersRow();

			for (size_t pluginIndex = 0; pluginIndex < Plugins.size(); pluginIndex++) {
				PluginPresentation &plugin = *Plugins[pluginIndex];
				for (size_t toolbarIndex = 0; toolbarIndex < plugin.Toolbars.size(); toolbarIndex++) {
					PluginToolbar &toolbar = plugin.Toolbars[toolbarIndex];
					const std::string defaultTab = PluginToolbarKey(plugin, toolbar, toolbarIndex);
					for (size_t itemIndex = 0; itemIndex < toolbar.Buttons.size(); itemIndex++) {
						PluginButton &button = toolbar.Buttons[itemIndex];
						const std::string key =
							PluginToolKey(plugin, toolbar, toolbarIndex, button, itemIndex);
						auto found = std::find_if(
							ToolbarPrefs.Items.begin(),
							ToolbarPrefs.Items.end(),
							[&](const ToolbarItemPreference &item) { return item.Key == key; }
						);
						const ToolbarItemPreference *inherited = nullptr;
						if (found == ToolbarPrefs.Items.end() && plugin.Builtin) {
							if (const char *legacyId = LegacyBuiltinToolId(button.Builtin);
								legacyId != nullptr) {
								PluginButton legacy;
								legacy.Id = legacyId;
								const std::string legacyKey =
									PluginToolKey(plugin, toolbar, toolbarIndex, legacy, itemIndex);
								const auto legacyPreference = std::find_if(
									ToolbarPrefs.Items.begin(),
									ToolbarPrefs.Items.end(),
									[&](const ToolbarItemPreference &item) { return item.Key == legacyKey; }
								);
								if (legacyPreference != ToolbarPrefs.Items.end()) {
									inherited = &*legacyPreference;
								}
							}
						}
						const auto effective = [&]() -> const ToolbarItemPreference * {
							return found == ToolbarPrefs.Items.end() ? inherited : &*found;
						};
						const auto ensure = [&]() -> ToolbarItemPreference & {
							if (found == ToolbarPrefs.Items.end()) {
								ToolbarItemPreference created = inherited == nullptr
									? ToolbarItemPreference{
										  key,
										  defaultTab,
										  button.Visible,
										  ClampPluginToolWidth(button.Width),
										  {},
										  {},
										  itemIndex,
									  }
									: *inherited;
								created.Key = key;
								created.Row =
									button.Row.empty() ? std::string{} : defaultTab + "/" + button.Row;
								created.Column =
									button.Column.empty() ? std::string{} : defaultTab + "/" + button.Column;
								created.Order = itemIndex;
								ToolbarPrefs.Items.push_back(std::move(created));
								found = std::prev(ToolbarPrefs.Items.end());
							}
							return *found;
						};

						ImGui::TableNextRow();
						ImGui::PushID(key.c_str());
						ImGui::TableNextColumn();
						bool visible = effective() == nullptr ? button.Visible : effective()->Visible;
						if (ImGui::Checkbox("##visible", &visible)) {
							ensure().Visible = visible;
							save();
						}

						ImGui::TableNextColumn();
						ImGui::TextUnformatted(button.Name.c_str());
						ImGui::TextDisabled("%s", plugin.Manifest.Name.c_str());

						ImGui::TableNextColumn();
						const std::string current = effective() == nullptr || effective()->Tab.empty()
														? defaultTab
														: effective()->Tab;
						const auto currentChoice =
							std::find_if(choices.begin(), choices.end(), [&](const TabChoice &choice) {
								return choice.Id == current;
							});
						const char *preview =
							currentChoice == choices.end() ? current.c_str() : currentChoice->Name.c_str();
						if (ImGui::BeginCombo("##tab", preview)) {
							for (const TabChoice &choice : choices) {
								if (ImGui::Selectable(choice.Name.c_str(), choice.Id == current)) {
									ensure().Tab = choice.Id;
									save();
								}
							}
							ImGui::EndCombo();
						}

						ImGui::TableNextColumn();
						float width =
							effective() == nullptr ? ClampPluginToolWidth(button.Width) : effective()->Width;
						ImGui::BeginDisabled(button.Kind == PluginControlKind::Builtin);
						ImGui::SetNextItemWidth(-1.0f);
						if (ImGui::DragFloat(
								"##width",
								&width,
								1.0f,
								PLUGIN_TOOL_MINIMUM_WIDTH,
								PLUGIN_TOOL_MAXIMUM_WIDTH,
								"%.0f px",
								ImGuiSliderFlags_AlwaysClamp
							)) {
							ensure().Width = ClampPluginToolWidth(width);
							save();
						}
						ImGui::EndDisabled();
						ImGui::PopID();
					}
				}
			}
			ImGui::EndTable();
		}

		ImGui::End();
	}

	void Editor::DrawDockWidgetEditor() {
		if (!ShowDockWidgetEditor) {
			return;
		}
		if (!ImGui::Begin("Dock Widgets", &ShowDockWidgetEditor)) {
			ImGui::End();
			return;
		}

		ImGui::TextWrapped(
			"Plugin widgets are ordinary ImGui dock windows. The requested dock and size limits apply on "
			"first use; "
			"after that, the saved layout belongs to the person using Studio."
		);
		ImGui::Separator();

		bool any = false;
		for (PluginPresentation *pluginPointer : Plugins) {
			PluginPresentation &plugin = *pluginPointer;
			for (PluginWidget &widget : plugin.Widgets) {
				any = true;
				const std::string id =
					PluginIdentity(plugin) + "/widget/" + (widget.Id.empty() ? widget.Title : widget.Id);
				ImGui::PushID(id.c_str());
				ImGui::Checkbox("##open", &widget.Open);
				ImGui::SameLine();
				ImGui::Text("%s  (%s)", widget.Title.c_str(), plugin.Manifest.Name.c_str());

				ImGui::SetNextItemWidth(engine::ui::Scaled(140.0f));
				if (ImGui::BeginCombo("Dock", Describe(widget.Dock))) {
					for (size_t ordinal = 0; ordinal <= static_cast<size_t>(PluginDock::Bottom); ordinal++) {
						const auto dock = static_cast<PluginDock>(ordinal);
						if (ImGui::Selectable(Describe(dock), dock == widget.Dock)) {
							widget.Dock = dock;
						}
					}
					ImGui::EndCombo();
				}
				ImGui::SameLine();
				ImGui::SetNextItemWidth(engine::ui::Scaled(110.0f));
				ImGui::DragFloat("Min W", &widget.MinimumWidth, 1.0f, 1.0f, 4096.0f, "%.0f px");
				ImGui::SameLine();
				ImGui::SetNextItemWidth(engine::ui::Scaled(110.0f));
				ImGui::DragFloat("Min H", &widget.MinimumHeight, 1.0f, 1.0f, 4096.0f, "%.0f px");
				ImGui::SameLine();
				ImGui::SetNextItemWidth(engine::ui::Scaled(110.0f));
				ImGui::DragFloat("Max W", &widget.MaximumWidth, 1.0f, 0.0f, 8192.0f, "%.0f px");
				ImGui::SameLine();
				ImGui::SetNextItemWidth(engine::ui::Scaled(110.0f));
				ImGui::DragFloat("Max H", &widget.MaximumHeight, 1.0f, 0.0f, 8192.0f, "%.0f px");
				widget.MinimumWidth = std::max(1.0f, widget.MinimumWidth);
				widget.MinimumHeight = std::max(1.0f, widget.MinimumHeight);
				widget.MaximumWidth = std::max(0.0f, widget.MaximumWidth);
				widget.MaximumHeight = std::max(0.0f, widget.MaximumHeight);
				ImGui::Separator();
				ImGui::PopID();
			}
		}
		if (!any) {
			ImGui::TextDisabled("No running plugin created a dock widget.");
		}
		ImGui::End();
	}

	void Editor::DrawPlugins() {
		if (!ShowPlugins) {
			return;
		}

		if (!ImGui::Begin("Plugins", &ShowPlugins)) {
			ImGui::End();
			return;
		}

		ImGui::TextDisabled("%s", PluginRoot().string().c_str());
		ImGui::Separator();

		if (ImGui::Button("Reload", ImVec2(96.0f, 0.0f))) {
			LoadPlugins();
		}
		ImGui::SameLine();
		ImGui::TextDisabled("A reload restarts Studio and active playtest plugin instances.");

		ImGui::Spacing();

		// **The toolbars are not here any more; they are the ribbon's Plugins
		// tab.** A toolbar button is pressed while working and this panel is
		// opened when something is wrong, so the two belong in different places
		// - and drawing them in both would be two sets of the same buttons that
		// could disagree about which is active. `DrawPluginTools` is the one.

		if (Plugins.empty()) {
			ImGui::TextDisabled("nothing installed");
			ImGui::TextWrapped("No native definitions or script plugin folders are available.");
			ImGui::End();
			return;
		}

		bool reloadAfterTable = false;
		bool saveState = false;
		if (ImGui::BeginTable("plugins", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY)) {
			ImGui::TableSetupColumn("On", ImGuiTableColumnFlags_WidthFixed, 36.0f);
			ImGui::TableSetupColumn("Plugin", ImGuiTableColumnFlags_WidthFixed, 150.0f);
			ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthFixed, 90.0f);
			ImGui::TableSetupColumn("What it is doing", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupScrollFreeze(0, 1);
			ImGui::TableHeadersRow();

			for (PluginPresentation *pluginPointer : Plugins) {
				PluginPresentation &plugin = *pluginPointer;
				ImGui::TableNextRow();

				ImGui::TableNextColumn();
				const std::string identity = PluginIdentity(plugin);
				bool enabled = plugin.Manifest.Enabled;
				const std::string enabledId = "##enabled." + identity;
				if (ImGui::Checkbox(enabledId.c_str(), &enabled)) {
					PluginEnabled[identity] = enabled;
					saveState = true;
					reloadAfterTable = true;
				}

				ImGui::TableNextColumn();
				ImGui::TextUnformatted(plugin.Manifest.Name.c_str());
				if (ImGui::IsItemHovered()) {
					ImGui::SetTooltip("%s", plugin.Root.string().c_str());
				}
				if (!plugin.Manifest.Version.empty() || !plugin.Manifest.Author.empty()) {
					ImGui::TextDisabled(
						"%s%s%s",
						plugin.Manifest.Version.empty() ? "" : plugin.Manifest.Version.c_str(),
						!plugin.Manifest.Version.empty() && !plugin.Manifest.Author.empty() ? " by " : "",
						plugin.Manifest.Author.empty() ? "" : plugin.Manifest.Author.c_str()
					);
				}
				std::string targets;
				for (const PluginRunTarget target : {
						 PluginRunTarget::Studio,
						 PluginRunTarget::PlaytestServer,
						 PluginRunTarget::PlaytestClient,
					 }) {
					if (!RunsIn(plugin.Manifest.Runs, target)) {
						continue;
					}
					if (!targets.empty()) {
						targets += ", ";
					}
					targets += Describe(target);
				}
				ImGui::TextDisabled("%s | %s", plugin.Native ? "C++" : "script", targets.c_str());

				ImGui::TableNextColumn();
				if (plugin.Running) {
					ImGui::TextColored(ImVec4(0.4f, 0.8f, 0.4f, 1.0f), "running");
				} else if ((plugin.Native ? static_cast<LoadedCppPlugin &>(plugin).Faults
										  : static_cast<LoadedPlugin &>(plugin).Faults) > 0) {
					ImGui::TextColored(ImVec4(0.9f, 0.4f, 0.4f, 1.0f), "faulted");
				} else {
					ImGui::TextDisabled("stopped");
				}

				ImGui::TableNextColumn();

				// A panel somebody closed is reopened from here, which is the
				// one thing they cannot do from the plugin's own surface.
				for (PluginWidget &widget : plugin.Widgets) {
					const std::string id =
						widget.Title + "###toggle." + plugin.Manifest.Name + "." + widget.Title;
					ImGui::Checkbox(id.c_str(), &widget.Open);
					ImGui::SameLine();
				}

				// **The error where the description would be**, because a
				// plugin that is not running is one somebody is trying to fix
				// and the description is not what they need.
				if (!plugin.Error.empty()) {
					ImGui::TextWrapped("%s", plugin.Error.c_str());
				} else if (!plugin.Manifest.Description.empty()) {
					ImGui::TextWrapped("%s", plugin.Manifest.Description.c_str());
				} else {
					ImGui::TextDisabled("-");
				}
			}
			ImGui::EndTable();
		}

		if (saveState) {
			std::string error;
			if (!SavePluginState(ConfigPath("plugins.json"), PluginEnabled, error)) {
				Say(error, engine::core::LogLevel::Warning);
				reloadAfterTable = false;
			}
		}
		if (reloadAfterTable) {
			LoadPlugins();
		}

		ImGui::End();
	}

	// --- ChangeHistoryService's events ----------------------------------------
	//
	// **One watcher on the log, fanned out here.** `CommandLog` has a single
	// seam because a log that knew what a plugin was would be a log that knows
	// what an editor is. Deciding who hears about a waypoint is this class's
	// job: the plugins, and - when one is open - the team-create edit stream.
	//
	// **Every running plugin hears every event, including its own.** Roblox does
	// the same, and the alternative needs the log to record which plugin made a
	// change, which it deliberately does not: an edit is an edit whoever asked
	// for it, and a plugin that reacts to its own undo is a plugin that asked to.

	void Editor::InstallHistoryWatcher() {
		if (Commands == nullptr) {
			return;
		}

		CommandLog::Watcher watcher;

		watcher.Undone = [this](std::string_view waypoint) {
			const engine::script::HostValue name = engine::script::HostValue::Of(waypoint);
			for (LoadedPlugin &plugin : ScriptPlugins) {
				InvokePlugin(plugin, plugin.OnUndo, false, engine::script::HostArguments(&name, 1));
			}
		};

		watcher.Redone = [this](std::string_view waypoint) {
			const engine::script::HostValue name = engine::script::HostValue::Of(waypoint);
			for (LoadedPlugin &plugin : ScriptPlugins) {
				InvokePlugin(plugin, plugin.OnRedo, false, engine::script::HostArguments(&name, 1));
			}
		};

		watcher.RecordingStarted = [this](const Recording &recording) {
			// Roblox passes name and displayName, in that order.
			const engine::script::HostValue arguments[] = {
				engine::script::HostValue::Of(std::string_view(recording.Name)),
				engine::script::HostValue::Of(std::string_view(recording.DisplayName)),
			};
			for (LoadedPlugin &plugin : ScriptPlugins) {
				InvokePlugin(
					plugin, plugin.OnRecordingStarted, false, engine::script::HostArguments(arguments, 2)
				);
			}
		};

		watcher.RecordingFinished = [this](const Recording &recording, FinishOperation operation) {
			// Name, displayName, identifier, operation - Roblox's order. The
			// operation crosses as its member's name, which is how an
			// `EnumItem` crosses in the other direction too.
			const engine::script::HostValue arguments[] = {
				engine::script::HostValue::Of(std::string_view(recording.Name)),
				engine::script::HostValue::Of(std::string_view(recording.DisplayName)),
				engine::script::HostValue::Of(std::string_view(recording.Identifier)),
				engine::script::HostValue::Of(std::string_view(Describe(operation))),
			};
			for (LoadedPlugin &plugin : ScriptPlugins) {
				InvokePlugin(
					plugin, plugin.OnRecordingFinished, false, engine::script::HostArguments(arguments, 4)
				);
			}
		};

		watcher.Committed = [this](uint64_t waypoint, std::span<const Command> group) {
			// **One waypoint, whole, in the order it was made.** Team create's
			// end of it: a peer that applied half of a group would show a state
			// the author never saw. Does nothing when no session is open, which
			// is what keeps an editor that never opens the panel from paying
			// for any of this.
			if (Team != nullptr) {
				Team->PublishEdits(waypoint, group, engine::core::Clock::Seconds());
			}
		};

		Commands->Watch(std::move(watcher));
	}
}
