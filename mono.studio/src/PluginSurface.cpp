// What the editor offers a plugin, over `script::HostSurface`.
//
// **The names are Roblox's where Roblox has one**, because a person who has
// written a Studio plugin should be able to write one of these without learning
// a second vocabulary for the same idea. Where the shape differs it is because
// the seam carries values rather than objects - `plugin.CreateButton(toolbar,
// ...)` takes the toolbar's id where Roblox's takes the toolbar - and that is
// stated rather than smoothed over.
//
//     local toolbar = plugin.CreateToolbar("My Tools")
//     local Selection = game:GetService("Selection")
//
//     plugin.CreateButton(toolbar, "Align", "Align the selection", function()
//         for _, part in Selection:Get() do
//             part.CFrame = CFrame.new(0, part.Position.Y, 0)
//         end
//     end)
//
//     local panel = plugin.CreateWidget("Align", true)
//     local root = plugin.GetWidgetGui(panel)
//     local button = Instance.new("TextButton")
//     button.Parent = root
//     plugin.SetWidgetRender(panel, function()
//         plugin.Label("Selected: " .. #Selection:Get())
//         if plugin.Button("Clear") then Selection:Set({}) end
//     end)
//
// ## Three groups, and the third is the one with a rule
//
// **World objects use the engine's ordinary instance surface.** The one join is
// `GetWidgetGui`: the editor owns the dock window, so it hands the script the
// `DockWidgetPluginGui` entity whose children fill that window. Everything
// below it is created and edited through `Instance` like any other GUI tree.
//
// **The widget calls are only legal while a widget is rendering.** They are
// immediate-mode ImGui underneath, so calling `plugin.Label` from a heartbeat
// would draw into whatever window the editor happened to be building. `Drawing`
// is the gate and the refusal names the reason, because the alternative is a
// plugin that corrupts a panel it has never heard of.
//
// ## What a plugin may read of another script, and why that is not a hole
//
// `GetScriptSource` hands back the text of a `LuaSourceContainer` in the world
// being edited. That is the same text the script editor shows and the same text
// a save file carries - a plugin is a tool running in an editor on a project
// somebody opened, so it is reading what its user is already looking at.
//
// It is *not* a way out of the sandbox: it reads through `script::ReadSource`
// against this world's own `SourceCache`, so there is no path from a name to a
// file on disk. A plugin cannot read `/etc/passwd` by calling it, which is the
// property that matters.

#include "PluginSurfaceInternal.hpp"
#include "SourceEditor.hpp"

#include <engine/core/Log.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/gui/Components.hpp>
#include <engine/gui/Registration.hpp>
#include <engine/script/Instances.hpp>
#include <engine/script/Runtime.hpp>
#include <engine/script/SourceCache.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <imgui.h>
#include <iterator>
#include <limits>
#include <nlohmann/json.hpp>
#include <string>
#include <studio/Editor.hpp>
#include <studio/Plugins.hpp>
#include <vector>

namespace studio {

	using engine::ecs::Entity;
	using engine::ecs::Store;
	using engine::script::HostArguments;
	using engine::script::HostCallback;
	using engine::script::HostTag;
	using engine::script::HostValue;

	namespace {
		// One argument, or nil when the script passed fewer.
		const HostValue &At(HostArguments arguments, size_t index) {
			static const HostValue nothing;
			return index < arguments.size() ? arguments[index] : nothing;
		}

		// An index into a vector, from a script's number.
		//
		// **One-based on the script side**, because that is Luau's own
		// convention and a plugin author counting from zero in Luau is a plugin
		// author who will get it wrong once. Zero and out of range both answer
		// `false`, which the caller turns into a named refusal.
		bool SurfaceIndexOf(const HostValue &value, size_t count, size_t &out) {
			const double number = value.AsNumber(0.0);
			if (!std::isfinite(number) || number != std::floor(number) || number < 1.0 ||
				number > static_cast<double>(count)) {
				return false;
			}
			out = static_cast<size_t>(number) - 1;
			return true;
		}

		std::string ColourText(const engine::core::Color3 &colour) {
			const auto channel = [](float value) {
				return static_cast<unsigned int>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
			};
			return engine::ui::ColourText(
				IM_COL32(channel(colour.R), channel(colour.G), channel(colour.B), 255u)
			);
		}

		bool ParseColour(const HostValue &value, engine::core::Color3 &out) {
			const std::optional<unsigned int> packed = engine::ui::ParseColourText(value.AsText());
			if (!packed) {
				return false;
			}
			out = engine::core::Color3{
				static_cast<float>((*packed >> IM_COL32_R_SHIFT) & 0xFFu) / 255.0f,
				static_cast<float>((*packed >> IM_COL32_G_SHIFT) & 0xFFu) / 255.0f,
				static_cast<float>((*packed >> IM_COL32_B_SHIFT) & 0xFFu) / 255.0f,
			};
			return true;
		}
	}

	// The editor's half of the seam, one per plugin.
	//
	// **Per plugin rather than one shared**, because every call has to know
	// which plugin made it: a toolbar belongs to the plugin that created it, and
	// a surface shared between them would need the caller's identity passed in
	// on every call - which the seam has no way to supply and a plugin could
	// forge.
	class PluginSurface final : public engine::script::HostSurface {
	  public:
		PluginSurface(Editor &editor, LoadedPlugin &plugin, Store &store)
			: Owner(editor), Plugin(plugin), World(store) {}

		~PluginSurface() override {
			for (PluginWidget &widget : Plugin.Widgets) {
				if (World.Alive(widget.Gui)) {
					World.Destroy(widget.Gui);
				}
			}
		}

		std::string_view GlobalName() const override {
			return "plugin";
		}

		// `plugin` is the `Plugin` instance in Studio, so `plugin.Name` and
		// `plugin:CreateToolbar` work beside the flat `plugin.*` calls.
		Entity GlobalInstance() const override {
			return Plugin.Target == PluginRunTarget::Studio ? Plugin.Tree.Instance() : Entity{};
		}

		std::vector<std::string> Names() const override {
			std::vector<std::string> names;
			if (Plugin.Target == PluginRunTarget::Studio) {
				names.reserve(std::size(STUDIO_PLUGIN_HOST_NAMES) + std::size(STUDIO_PLUGIN_MEMBER_NAMES));
				for (const std::string_view name : STUDIO_PLUGIN_HOST_NAMES) {
					names.emplace_back(name);
				}
				for (const std::string_view name : STUDIO_PLUGIN_MEMBER_NAMES) {
					names.emplace_back(name);
				}
			}
			if (Plugin.Bindings != nullptr) {
				for (std::string &dynamic : Plugin.Bindings->Names(Plugin.Language)) {
					if (std::find(names.begin(), names.end(), dynamic) == names.end()) {
						names.push_back(std::move(dynamic));
					}
				}
			}
			return names;
		}

		bool Call(
			std::string_view name, HostArguments arguments, HostValue &result, std::string &failure
		) override {
			if (Plugin.Target != PluginRunTarget::Studio) {
				if (Plugin.Bindings != nullptr) {
					return Plugin.Bindings->Call(Plugin.Language, name, arguments, result, failure);
				}
				failure = "no such playtest plugin binding";
				return false;
			}
			// A plain chain rather than a table of member pointers. Each call is a
			// handful of lines, and a dispatch table would duplicate this list.
			if (name.starts_with("@") || name == "DockWidgetPluginGuiInfo.new") {
				return Member(name, arguments, result, failure);
			}
			if (name == "Notify") {
				Owner.Say("[" + Plugin.Manifest.Name + "] " + std::string(At(arguments, 0).AsText()));
				return true;
			}
			if (name == "GetActiveWorld") {
				result = HostValue::Of(std::string_view(Owner.ActiveWorldName()));
				return true;
			}
			if (name == "Selection.Get") {
				std::vector<HostValue> selected;
				for (const Entity instance : Owner.Selection) {
					selected.push_back(HostValue::Of(instance));
				}
				result = HostValue::List(std::move(selected));
				return true;
			}
			if (name == "Selection.Set" || name == "Selection.Add" || name == "Selection.Remove") {
				return Selection(name, At(arguments, 0), failure);
			}
			if (name == "Selection.SelectionChanged") {
				// One slot, filled by the adapter's dispatcher; see `IsHostSignal`.
				const HostValue &handler = At(arguments, 0);
				if (handler.Tag != HostTag::Callback) {
					failure = "SelectionChanged expects a function";
					return false;
				}
				if (Plugin.OnSelectionChanged.Valid() && Plugin.Vm != nullptr)
					Plugin.Vm->Release(Plugin.OnSelectionChanged);
				Plugin.OnSelectionChanged = handler.Callback;
				return true;
			}
			if (name.rfind("ChangeHistoryService.", 0) == 0) {
				return History(
					name.substr(std::string_view("ChangeHistoryService.").size()), arguments, result, failure
				);
			}
			if (name == "GetScriptSource") {
				return GetScriptSource(At(arguments, 0), result, failure);
			}
			if (name == "SetScriptSource") {
				return SetScriptSource(At(arguments, 0), At(arguments, 1), failure);
			}
			if (name == "GetScripts") {
				return GetScripts(result, failure);
			}
			if (name == "CreateToolbar" || name == "CreateToolbarTab") {
				PluginToolbar toolbar;
				toolbar.Name = std::string(At(arguments, 0).AsText());
				if (toolbar.Name.empty()) {
					failure = "a toolbar needs a name";
					return false;
				}
				toolbar.Id = StableId(At(arguments, 1), "toolbar", Plugin.Toolbars.size());
				if (HasToolbarId(toolbar.Id)) {
					failure = "a toolbar with id '" + toolbar.Id + "' already exists";
					return false;
				}
				if (name == "CreateToolbarTab" && At(arguments, 2).Tag != HostTag::Nil) {
					const auto placement = ParsePluginToolbarPlacement(At(arguments, 2).AsText());
					if (!placement.has_value()) {
						failure = "toolbar placement has to be 'Tabbed' or 'Pinned'";
						return false;
					}
					toolbar.Placement = *placement;
				}
				if (name == "CreateToolbarTab" && At(arguments, 3).Tag != HostTag::Nil) {
					toolbar.Visible = At(arguments, 3).AsBoolean();
				}
				Plugin.Toolbars.push_back(std::move(toolbar));
				result = HostValue::Of(static_cast<double>(Plugin.Toolbars.size()));
				Owner.InvalidateToolbarLayout();
				return true;
			}
			if (name == "CreateToolbarRow" || name == "CreateToolbarColumn") {
				return CreateToolbarTrack(name, arguments, result, failure);
			}
			if (name == "CreateButton" || name == "CreateToggle" || name == "CreateDropdown" ||
				name == "CreateLabel") {
				return CreateControl(name, arguments, result, failure);
			}
			if (name == "SetToolCell") {
				return SetToolCell(arguments, failure);
			}
			if (name == "SetButtonActive") {
				return SetButtonActive(arguments, failure);
			}
			if (name == "SetToolVisible" || name == "SetToolWidth" || name == "SetToolbarVisible" ||
				name == "SetToolbarPlacement") {
				return ConfigureToolbar(name, arguments, failure);
			}
			if (name == "CreateWidget") {
				PluginWidget widget;
				widget.Title = std::string(At(arguments, 0).AsText());
				if (widget.Title.empty()) {
					failure = "a widget needs a title";
					return false;
				}
				widget.Open = At(arguments, 1).AsBoolean();
				widget.Id = StableId(At(arguments, 2), "widget", Plugin.Widgets.size());
				if (HasWidgetId(widget.Id)) {
					failure = "a widget with id '" + widget.Id + "' already exists";
					return false;
				}
				if (At(arguments, 3).Tag != HostTag::Nil) {
					const std::optional<PluginDock> dock = ParsePluginDock(At(arguments, 3).AsText());
					if (!dock) {
						failure = "a widget dock is Floating, Centre, Left, Right, or Bottom";
						return false;
					}
					widget.Dock = *dock;
				}
				engine::gui::RegisterGuiClasses();
				widget.Gui = World.CreateInstance(engine::gui::GuiClass("DockWidgetPluginGui"), widget.Title);
				if (widget.Gui == engine::ecs::NULL_ENTITY) {
					failure = "the dock widget GUI could not be created";
					return false;
				}
				if (engine::gui::Layer *layer = World.GetMutable<engine::gui::Layer>(widget.Gui)) {
					layer->Enabled = widget.Open;
				}
				Plugin.Widgets.push_back(std::move(widget));
				result = HostValue::Of(static_cast<double>(Plugin.Widgets.size()));
				return true;
			}
			if (name == "GetWidgetGui" || name == "SetWidgetRender" || name == "SetWidgetOpen" ||
				name == "IsWidgetOpen" || name == "SetWidgetColour" || name == "SetWidgetDock" ||
				name == "SetWidgetSizeConstraints") {
				return Widget(name, arguments, result, failure);
			}
			if (name == "GetViewportOption" || name == "SetViewportOption" || name == "AddViewport") {
				return Viewport(name, arguments, result, failure);
			}
			if (name == "OpenScript") {
				return OpenScript(At(arguments, 0), At(arguments, 1), failure);
			}

			// Everything below draws, and drawing is only legal from inside a
			// widget's own render call.
			if (!Drawing) {
				failure = std::string(name) + " may only be called while a widget is drawing";
				return false;
			}
			return Draw(name, arguments, result, failure);
		}

		// Whether a widget's render callback is on the stack.
		//
		// **Set by the editor around the invoke**, so the gate is a fact about
		// where the call came from rather than a promise the plugin makes.
		bool Drawing = false;

	  private:
		// --- Roblox's plugin object model ----------------------------------
		//
		// Built over the flat calls below, so a toolbar made with
		// `plugin:CreateToolbar` and one made with `plugin.CreateToolbar` are
		// the same record drawn the same way. The instances are the script's
		// handles; the records stay the editor's.

		// A new instance of a plugin class, parented under `parent`.
		Entity MakeInstance(std::string_view type, std::string_view name, Entity parent) {
			const Entity made =
				World.CreateInstance(engine::ecs::Classes::Find(engine::core::Name(type)), name);
			if (made != engine::ecs::NULL_ENTITY && parent != engine::ecs::NULL_ENTITY) {
				World.SetParent(made, parent);
			}
			return made;
		}

		// A toolbar argument to a flat call: its 1-based index, or the
		// `PluginToolbar` `plugin:CreateToolbar` returned.
		bool ToolbarIndexOf(const HostValue &value, size_t &out) const {
			if (value.Tag == HostTag::Instance) {
				const Entity instance = value.AsInstance();
				for (size_t index = 0; index < Plugin.Toolbars.size(); index++) {
					if (Plugin.Toolbars[index].Instance != instance) continue;
					out = index;
					return true;
				}
				return false;
			}
			return SurfaceIndexOf(value, Plugin.Toolbars.size(), out);
		}

		PluginToolbar *ToolbarOf(Entity instance) {
			for (PluginToolbar &toolbar : Plugin.Toolbars) {
				if (toolbar.Instance == instance) return &toolbar;
			}
			return nullptr;
		}

		PluginButton *ButtonOf(Entity instance) {
			for (PluginToolbar &toolbar : Plugin.Toolbars) {
				for (PluginButton &button : toolbar.Buttons) {
					if (button.Instance == instance) return &button;
				}
			}
			return nullptr;
		}

		PluginWidget *WidgetOf(Entity instance) {
			for (PluginWidget &widget : Plugin.Widgets) {
				if (widget.Gui == instance) return &widget;
			}
			return nullptr;
		}

		PluginActionRecord *ActionOf(Entity instance) {
			for (PluginActionRecord &action : Plugin.Actions) {
				if (action.Instance == instance) return &action;
			}
			return nullptr;
		}

		static nlohmann::json ToJson(const HostValue &value, bool &ok) {
			switch (value.Tag) {
			case HostTag::Nil:
				return nullptr;
			case HostTag::Boolean:
				return value.Boolean;
			case HostTag::Number:
				return value.Number;
			case HostTag::String:
				return value.Text;
			case HostTag::Array: {
				nlohmann::json out = nlohmann::json::array();
				for (const HostValue &item : value.Items)
					out.push_back(ToJson(item, ok));
				return out;
			}
			case HostTag::Map: {
				nlohmann::json out = nlohmann::json::object();
				for (const auto &[key, item] : value.Entries)
					out[key] = ToJson(item, ok);
				return out;
			}
			default:
				ok = false;
				return nullptr;
			}
		}

		static HostValue FromJson(const nlohmann::json &value) {
			if (value.is_boolean()) return HostValue::Of(value.get<bool>());
			if (value.is_number()) return HostValue::Of(value.get<double>());
			if (value.is_string())
				return HostValue::Of(std::string_view(value.get_ref<const std::string &>()));
			if (value.is_array()) {
				std::vector<HostValue> items;
				for (const nlohmann::json &item : value)
					items.push_back(FromJson(item));
				return HostValue::List(std::move(items));
			}
			if (value.is_object()) {
				HostValue map(HostTag::Map);
				for (const auto &[key, item] : value.items())
					map.Entries.emplace_back(key, FromJson(item));
				return map;
			}
			return {};
		}

		nlohmann::json ReadSettings() const {
			std::ifstream file(PluginSettingsPath(Plugin));
			if (!file) return nlohmann::json::object();
			nlohmann::json document = nlohmann::json::parse(file, nullptr, false);
			return document.is_object() ? document : nlohmann::json::object();
		}

		// The editor's tool as Roblox names it.
		const char *RibbonToolName() const {
			switch (Owner.CurrentTool) {
			case Editor::ToolMode::None:
				return "None";
			case Editor::ToolMode::Select:
				return "Select";
			case Editor::ToolMode::Move:
				return "Move";
			case Editor::ToolMode::Rotate:
				return "Rotate";
			case Editor::ToolMode::Scale:
				return "Scale";
			}
			return "None";
		}

		bool Member(std::string_view name, HostArguments arguments, HostValue &result, std::string &failure) {
			const Entity self = At(arguments, 0).AsInstance();
			const HostValue &first = At(arguments, 1);

			if (name == "DockWidgetPluginGuiInfo.new") {
				// Roblox's positional constructor, answered as a map.
				static constexpr const char *FIELDS[] = {
					"InitialDockState",
					"InitialEnabled",
					"InitialEnabledShouldOverrideRestore",
					"FloatingXSize",
					"FloatingYSize",
					"MinWidth",
					"MinHeight",
				};
				result = HostValue(HostTag::Map);
				for (size_t index = 0; index < std::size(FIELDS); index++) {
					if (At(arguments, index).Tag != HostTag::Nil) {
						result.Entries.emplace_back(FIELDS[index], At(arguments, index));
					}
				}
				return true;
			}

			// --- Plugin ------------------------------------------------------
			if (name == "@Plugin:CreateToolbar") {
				const HostValue legacy[] = {first};
				HostValue index;
				if (!Call("CreateToolbar", legacy, index, failure)) return false;
				PluginToolbar &toolbar = Plugin.Toolbars.back();
				toolbar.Instance = MakeInstance("PluginToolbar", toolbar.Name, self);
				result = HostValue::Of(toolbar.Instance);
				return true;
			}
			if (name == "@Plugin:CreatePluginAction") {
				PluginActionRecord action;
				action.Id = std::string(first.AsText());
				if (action.Id.empty()) {
					failure = "an action needs an id";
					return false;
				}
				if (std::any_of(
						Plugin.Actions.begin(), Plugin.Actions.end(), [&](const PluginActionRecord &held) {
							return held.Id == action.Id;
						}
					)) {
					failure = "an action with id '" + action.Id + "' already exists";
					return false;
				}
				action.Text = std::string(At(arguments, 2).AsText());
				action.StatusTip = std::string(At(arguments, 3).AsText());
				action.Icon = std::string(At(arguments, 4).AsText());
				action.AllowBinding = At(arguments, 5).Tag == HostTag::Nil || At(arguments, 5).AsBoolean();
				action.Instance = MakeInstance("PluginAction", action.Id, self);
				Plugin.Actions.push_back(action);
				result = HostValue::Of(action.Instance);
				return true;
			}
			if (name == "@Plugin:CreateDockWidgetPluginGui" ||
				name == "@Plugin:CreateDockWidgetPluginGuiAsync") {
				return CreateDockWidget(arguments, result, failure);
			}
			if (name == "@Plugin:GetMouse") {
				if (!World.Alive(Plugin.Mouse))
					Plugin.Mouse = MakeInstance("PluginMouse", "PluginMouse", self);
				result = HostValue::Of(Plugin.Mouse);
				return true;
			}
			if (name == "@Plugin:GetSetting") {
				const nlohmann::json settings = ReadSettings();
				const auto found = settings.find(std::string(first.AsText()));
				if (found != settings.end()) result = FromJson(*found);
				return true;
			}
			if (name == "@Plugin:SetSetting") {
				const std::string key(first.AsText());
				if (key.empty()) {
					failure = "a setting needs a key";
					return false;
				}
				bool ok = true;
				nlohmann::json settings = ReadSettings();
				const nlohmann::json value = ToJson(At(arguments, 2), ok);
				if (!ok) {
					failure = "a setting holds booleans, numbers, strings and tables of them";
					return false;
				}
				if (value.is_null())
					settings.erase(key);
				else
					settings[key] = value;
				const std::filesystem::path path = PluginSettingsPath(Plugin);
				std::error_code made;
				std::filesystem::create_directories(path.parent_path(), made);
				std::ofstream file(path, std::ios::trunc);
				if (!file) {
					failure = "could not write " + path.string();
					return false;
				}
				file << settings.dump(1, '\t');
				return true;
			}
			if (name == "@Plugin:Activate") {
				Owner.ActivatePlugin(&Plugin, first.AsBoolean());
				return true;
			}
			if (name == "@Plugin:Deactivate") {
				if (Plugin.Activated) Owner.ActivatePlugin(nullptr, false);
				return true;
			}
			if (name == "@Plugin:IsActivated") {
				result = HostValue::Of(Plugin.Activated);
				return true;
			}
			if (name == "@Plugin:IsActivatedWithExclusiveMouse") {
				result = HostValue::Of(Plugin.Activated && Plugin.ExclusiveMouse);
				return true;
			}
			if (name == "@Plugin:GetSelectedRibbonTool") {
				result = HostValue::Of(std::string_view(RibbonToolName()));
				return true;
			}
			if (name == "@Plugin:SelectRibbonTool") {
				const std::string_view tool = first.AsText();
				if (tool == "Select")
					Owner.CurrentTool = Editor::ToolMode::Select;
				else if (tool == "Move")
					Owner.CurrentTool = Editor::ToolMode::Move;
				else if (tool == "Rotate")
					Owner.CurrentTool = Editor::ToolMode::Rotate;
				else if (tool == "Scale")
					Owner.CurrentTool = Editor::ToolMode::Scale;
				else if (tool == "None")
					Owner.CurrentTool = Editor::ToolMode::None;
				else {
					failure = "Studio offers the Select, Move, Rotate, Scale and None tools";
					return false;
				}
				return true;
			}
			if (name == "@Plugin:OpenScript") {
				return OpenScript(first, At(arguments, 2), failure);
			}
			if (name == "@Plugin:CreatePluginMenu") {
				PluginMenuRecord menu;
				menu.Id = std::string(first.AsText());
				if (menu.Id.empty()) {
					failure = "a menu needs an id";
					return false;
				}
				menu.Title = std::string(At(arguments, 2).AsText());
				menu.Icon = std::string(At(arguments, 3).AsText());
				menu.Instance = MakeInstance("PluginMenu", menu.Id, self);
				Plugin.Menus.push_back(std::move(menu));
				result = HostValue::Of(Plugin.Menus.back().Instance);
				return true;
			}
			if (name == "@Plugin.GridSize") {
				result = HostValue::Of(static_cast<double>(Owner.GridSettings.Step));
				return true;
			}
			if (name == "@Plugin.CollisionEnabled" || name == "@Plugin.IsDebuggable") {
				if (arguments.size() > 1) {
					failure = std::string(name.substr(8)) + " is read-only";
					return false;
				}
				result = HostValue::Of(false);
				return true;
			}

			// --- toolbars ----------------------------------------------------
			if (name == "@PluginToolbar:CreateButton") {
				PluginToolbar *toolbar = ToolbarOf(self);
				if (toolbar == nullptr) {
					failure = "this toolbar has been removed";
					return false;
				}
				const size_t toolbarIndex = static_cast<size_t>(toolbar - Plugin.Toolbars.data());
				// Roblox's (id, tooltip, icon, text): the text is what shows,
				// and the id is the stable key a toolbar layout remembers.
				const std::string id(first.AsText());
				const std::string text(At(arguments, 4).AsText());
				const HostValue legacy[] = {
					HostValue::Of(static_cast<double>(toolbarIndex + 1)),
					HostValue::Of(std::string_view(text.empty() ? id : text)),
					At(arguments, 2),
					HostValue{},
					HostValue::Of(std::string_view(id)),
				};
				HostValue index;
				if (!Call("CreateButton", legacy, index, failure)) return false;
				PluginButton &button = Plugin.Toolbars[toolbarIndex].Buttons.back();
				button.Icon = std::string(At(arguments, 3).AsText());
				button.Instance = MakeInstance("PluginToolbarButton", id, self);
				result = HostValue::Of(button.Instance);
				return true;
			}
			if (name.starts_with("@PluginToolbarButton")) {
				PluginButton *button = ButtonOf(self);
				if (button == nullptr) {
					failure = "this button has been removed";
					return false;
				}
				const bool write = arguments.size() > 1;
				if (name == "@PluginToolbarButton:SetActive") {
					button->Active = first.AsBoolean();
				} else if (name == "@PluginToolbarButton.Enabled") {
					if (write)
						button->Enabled = first.AsBoolean();
					else
						result = HostValue::Of(button->Enabled);
				} else if (name == "@PluginToolbarButton.Icon") {
					if (write)
						button->Icon = std::string(first.AsText());
					else
						result = HostValue::Of(std::string_view(button->Icon));
				} else if (name == "@PluginToolbarButton.ClickableWhenViewportHidden") {
					if (write)
						button->ClickableWhenViewportHidden = first.AsBoolean();
					else
						result = HostValue::Of(button->ClickableWhenViewportHidden);
				}
				return true;
			}

			// --- actions -----------------------------------------------------
			if (name.starts_with("@PluginAction.")) {
				const PluginActionRecord *action = ActionOf(self);
				if (action == nullptr) {
					failure = "this action has been removed";
					return false;
				}
				if (arguments.size() > 1) {
					failure = std::string(name.substr(14)) + " is read-only";
					return false;
				}
				if (name == "@PluginAction.ActionId")
					result = HostValue::Of(std::string_view(action->Id));
				else if (name == "@PluginAction.Text")
					result = HostValue::Of(std::string_view(action->Text));
				else if (name == "@PluginAction.StatusTip")
					result = HostValue::Of(std::string_view(action->StatusTip));
				else if (name == "@PluginAction.AllowBinding")
					result = HostValue::Of(action->AllowBinding);
				return true;
			}

			// --- menus -------------------------------------------------------
			if (name.starts_with("@PluginMenu")) {
				return Menu(name, self, arguments, result, failure);
			}

			// --- the mouse ---------------------------------------------------
			if (name.starts_with("@PluginMouse.")) {
				if (arguments.size() > 1) {
					failure = std::string(name.substr(13)) + " is read-only";
					return false;
				}
				const Editor::PluginMouseState &mouse = Owner.PluginMouse;
				if (name == "@PluginMouse.X")
					result = HostValue::Of(static_cast<double>(mouse.X));
				else if (name == "@PluginMouse.Y")
					result = HostValue::Of(static_cast<double>(mouse.Y));
				else if (name == "@PluginMouse.ViewSizeX")
					result = HostValue::Of(static_cast<double>(mouse.ViewSizeX));
				else if (name == "@PluginMouse.ViewSizeY")
					result = HostValue::Of(static_cast<double>(mouse.ViewSizeY));
				else if (name == "@PluginMouse.Target") {
					if (mouse.Valid && World.Alive(mouse.Target)) result = HostValue::Of(mouse.Target);
				} else if (name == "@PluginMouse.Hit") {
					result = HostValue(HostTag::CFrame);
					result.Frame = mouse.Hit;
				} else if (name == "@PluginMouse.Origin") {
					result = HostValue(HostTag::CFrame);
					result.Frame = engine::core::CFrame(mouse.Origin);
				}
				return true;
			}

			// --- dock widgets ------------------------------------------------
			if (name.starts_with("@DockWidgetPluginGui")) {
				PluginWidget *widget = WidgetOf(self);
				if (widget == nullptr) {
					failure = "this widget does not belong to this plugin";
					return false;
				}
				if (name == "@DockWidgetPluginGui.Title") {
					if (arguments.size() > 1) {
						const std::string title(first.AsText());
						if (title.empty()) {
							failure = "a widget title cannot be empty";
							return false;
						}
						widget->Title = title;
					} else {
						result = HostValue::Of(std::string_view(widget->Title));
					}
					return true;
				}
				if (name == "@DockWidgetPluginGui:BindToClose") {
					if (widget->OnClose.Valid() && Plugin.Vm != nullptr) Plugin.Vm->Release(widget->OnClose);
					widget->OnClose = first.Tag == HostTag::Callback ? first.Callback : HostCallback{};
					return true;
				}
			}

			failure = "no such plugin member";
			return false;
		}

		PluginMenuRecord *MenuOf(Entity instance) {
			for (PluginMenuRecord &menu : Plugin.Menus) {
				if (menu.Instance == instance) return &menu;
			}
			return nullptr;
		}

		// `PluginMenu` members. Rows hold instances, so an action or submenu
		// added to two menus is one record shown twice, as in Roblox.
		bool Menu(
			std::string_view name,
			Entity self,
			HostArguments arguments,
			HostValue &result,
			std::string &failure
		) {
			PluginMenuRecord *menu = MenuOf(self);
			if (menu == nullptr) {
				failure = "this menu has been removed";
				return false;
			}
			const HostValue &first = At(arguments, 1);
			const bool write = arguments.size() > 1;
			if (name == "@PluginMenu.Title") {
				if (write)
					menu->Title = std::string(first.AsText());
				else
					result = HostValue::Of(std::string_view(menu->Title));
				return true;
			}
			if (name == "@PluginMenu.Icon") {
				if (write)
					menu->Icon = std::string(first.AsText());
				else
					result = HostValue::Of(std::string_view(menu->Icon));
				return true;
			}
			if (name == "@PluginMenu:AddAction") {
				const Entity action = first.AsInstance();
				if (ActionOf(action) == nullptr) {
					failure = "AddAction takes a PluginAction this plugin made";
					return false;
				}
				menu->Rows.push_back({action, false});
				return true;
			}
			if (name == "@PluginMenu:AddNewAction") {
				// Roblox's (actionId, text, icon): an action only this menu shows.
				PluginActionRecord action;
				action.Id = std::string(first.AsText());
				if (action.Id.empty()) {
					failure = "an action needs an id";
					return false;
				}
				action.Text = std::string(At(arguments, 2).AsText());
				action.Icon = std::string(At(arguments, 3).AsText());
				action.Listed = false;
				action.Instance = MakeInstance("PluginAction", action.Id, self);
				menu->Rows.push_back({action.Instance, false});
				Plugin.Actions.push_back(std::move(action));
				result = HostValue::Of(Plugin.Actions.back().Instance);
				return true;
			}
			if (name == "@PluginMenu:AddMenu") {
				const Entity inner = first.AsInstance();
				if (MenuOf(inner) == nullptr) {
					failure = "AddMenu takes a PluginMenu this plugin made";
					return false;
				}
				menu->Rows.push_back({inner, true});
				return true;
			}
			if (name == "@PluginMenu:AddSeparator") {
				menu->Rows.push_back({});
				return true;
			}
			if (name == "@PluginMenu:Clear") {
				// `true`, matching the `Clear(): boolean` every instance declares.
				menu->Rows.clear();
				result = HostValue::Of(true);
				return true;
			}
			if (name == "@PluginMenu:ShowAsync") {
				// Waits like Roblox's: the caller resumes with the chosen
				// action, or nil when the menu is dismissed. See
				// `Editor::ResolvePluginMenus`.
				if (Plugin.Showing.Token != 0) {
					failure = "another menu of this plugin is already showing";
					return false;
				}
				const uint64_t token = ++Owner.PluginHistoryTokens;
				if (!SuspendCaller(token)) {
					failure =
						"ShowAsync waits for a choice, so it has to be called where the script can yield";
					return false;
				}
				Plugin.Showing = LoadedPlugin::ShownMenu{token, self, false, false, {}};
				return true;
			}
			failure = "no such PluginMenu member";
			return false;
		}

		// `plugin:CreateDockWidgetPluginGui(id, info)` over `CreateWidget`.
		bool CreateDockWidget(HostArguments arguments, HostValue &result, std::string &failure) {
			const std::string id(At(arguments, 1).AsText());
			if (id.empty()) {
				failure = "a dock widget needs an id";
				return false;
			}
			const HostValue &info = At(arguments, 2);
			const auto field = [&](std::string_view key) -> const HostValue & {
				static const HostValue nothing;
				for (const auto &[name, value] : info.Entries) {
					if (name == key) return value;
				}
				return nothing;
			};

			std::string dock = "Floating";
			const std::string_view state = field("InitialDockState").AsText();
			if (state == "Left" || state == "Right" || state == "Bottom")
				dock = std::string(state);
			else if (state == "Top")
				dock = "Centre";

			const HostValue legacy[] = {
				HostValue::Of(std::string_view(id)),
				HostValue::Of(field("InitialEnabled").AsBoolean()),
				HostValue::Of(std::string_view(id)),
				HostValue::Of(std::string_view(dock)),
			};
			HostValue index;
			if (!Call("CreateWidget", legacy, index, failure)) return false;

			PluginWidget &widget = Plugin.Widgets.back();
			widget.MinimumWidth = static_cast<float>(field("MinWidth").AsNumber(widget.MinimumWidth));
			widget.MinimumHeight = static_cast<float>(field("MinHeight").AsNumber(widget.MinimumHeight));
			// Roblox's dock widgets live in `PluginGuiService`, beside the
			// plugins rather than inside one.
			World.SetParent(widget.Gui, EnsurePluginGuiService(World));
			result = HostValue::Of(widget.Gui);
			return true;
		}

		friend bool RunPluginHistoryCall(
			LoadedPlugin &plugin,
			std::string_view method,
			engine::script::HostArguments arguments,
			engine::script::HostValue &result,
			std::string &failure
		);

		// `Selection:Set`, `:Add` and `:Remove`, which differ only in what they
		// do to what is already selected.
		//
		// **The argument has to be an array and the items do not have to be
		// good.** Those are two different mistakes and they deserve different
		// answers: `Selection:Set(part)` without the braces is the call being
		// wrong, and it is refused; an item that has been destroyed since the
		// plugin picked it up is the *world* having moved on, which is not the
		// plugin's fault and is not worth failing a whole call over.
		//
		// So a bad item is skipped and warned about, once per call, naming what
		// it was. A plugin that selects the results of a query it ran three
		// frames ago should end up selecting the ones that are still there
		// rather than getting an error it can do nothing about.
		//
		// **An empty array is a whole answer**, not a degenerate one:
		// `Selection:Set({})` is how a plugin deselects everything, and it is
		// the reason the binding reads an empty Luau table as an array - see
		// `HostValue::Items`.
		// --- ChangeHistoryService ---------------------------------------------
		//
		// `Undo`, `Redo`, `FinishRecording`, `ResetWaypoints` and `SetEnabled`
		// called from inside an editor callback - a heartbeat, a button, a
		// widget - wait for the editor's next safe point and return there. The
		// caller resumes on its next heartbeat; a Luau handler whose answer the
		// editor reads cannot wait and is answered at once.
		//
		// `GetCanUndo` and `GetCanRedo` answer a tuple, so Luau reads
		// `local can, name = ChangeHistoryService:GetCanUndo()` as on Roblox
		// (JavaScript receives `[can, name]`).
		//
		// The four `On*` members each hold one handler per plugin here. The
		// script adapters wrap them as signals (`OnUndo:Connect(f)`, see
		// `IsHostSignal`), so a plugin can connect many handlers and disconnect
		// them; the adapter registers one dispatcher in the slot below.
		//
		// **The service is the editor's single history, so one recording at a
		// time is a rule about the editor rather than about a plugin.** Roblox
		// allows one per plugin; two plugins recording into one undo stack
		// would produce a step neither of them described.
		bool
		History(std::string_view method, HostArguments arguments, HostValue &result, std::string &failure) {
			CommandLog *log = Owner.Commands.get();
			if (log == nullptr) {
				failure = "ChangeHistoryService is not available - this editor has no command log";
				return false;
			}

			// **A call that replays or rewrites history waits for a safe point
			// when its caller can wait.** Run from inside a plugin callback it
			// would undo commands under the frame still running that plugin -
			// and the script tabs it reconnects cannot be tracked from inside a
			// world entry. The call suspends and `RunPendingPluginHistory` runs
			// it, which is the same code below with nothing on the stack. A
			// caller that cannot wait still gets the immediate answer.
			const bool rewrites = method == "Undo" || method == "Redo" || method == "FinishRecording" ||
								  method == "ResetWaypoints" || method == "SetEnabled";
			if (rewrites && Owner.WorldLifecycleBlocked() && CanSuspend()) {
				const uint64_t token = ++Owner.PluginHistoryTokens;
				if (SuspendCaller(token)) {
					Plugin.PendingHistory.push_back(
						LoadedPlugin::PendingHistoryCall{
							token,
							std::string(method),
							std::vector<HostValue>(arguments.begin(), arguments.end())
						}
					);
					return true;
				}
			}

			if (method == "TryBeginRecording") {
				const std::string_view name = At(arguments, 0).AsText();
				if (name.empty()) {
					failure = "TryBeginRecording expects a name";
					return false;
				}
				// Nil for a refusal, which is Roblox's answer and the one a
				// plugin already checks for. It is a refusal to *record*, not a
				// licence to edit anyway.
				if (const auto identifier =
						log->TryBeginRecording(std::string(name), std::string(At(arguments, 1).AsText()))) {
					result = HostValue::Of(std::string_view(*identifier));
				}
				return true;
			}

			if (method == "FinishRecording") {
				FinishOperation operation = FinishOperation::Commit;
				if (!ReadOperation(At(arguments, 1), operation, failure)) {
					return false;
				}
				if (!Owner.CanFinishRecording()) {
					result = HostValue::Of(false);
					return true;
				}
				// The third argument is Roblox's `finalOptions`, forwarded to
				// the finish handler. Nothing in this editor reads it yet, and
				// accepting it costs nothing against refusing a call a plugin
				// already writes.
				const bool canRebind = Owner.Universe != nullptr && !Owner.Universe->IsEntering();
				if (canRebind && operation == FinishOperation::Cancel) Owner.TrackScriptTabs();
				result = HostValue::Of(log->FinishRecording(
					At(arguments, 0).AsText(),
					operation,
					operation == FinishOperation::Cancel
						? std::function<void()>([this] { Owner.RebindScriptTabs(); })
						: std::function<void()>{}
				));
				return true;
			}

			if (method == "IsRecordingInProgress") {
				result = HostValue::Of(log->IsRecordingInProgress(At(arguments, 0).AsText()));
				return true;
			}

			if (method == "GetCanUndo" || method == "GetCanRedo") {
				const bool undo = method == "GetCanUndo";
				const bool can = undo ? log->CanUndo() : log->CanRedo();

				std::vector<HostValue> answer;
				answer.push_back(HostValue::Of(can));
				if (can) {
					answer.push_back(HostValue::Of(undo ? log->NextUndo() : log->NextRedo()));
				}
				result = HostValue::Tuple(std::move(answer));
				return true;
			}

			if (method == "Undo" || method == "Redo") {
				// **Raises rather than answering false when there is nothing**,
				// which is Roblox's behaviour: a plugin that walks the history
				// without asking `GetCanUndo` first has a bug, and a silent
				// no-op is how that bug survives to the next release.
				const bool canRebind = Owner.Universe != nullptr && !Owner.Universe->IsEntering();
				if (canRebind) Owner.TrackScriptTabs();
				const std::function<void()> afterReplay = [this] { Owner.RebindScriptTabs(); };
				const bool moved = method == "Undo" ? log->Undo(afterReplay) : log->Redo(afterReplay);
				if (!moved) {
					failure = std::string("there is nothing to ") + (method == "Undo" ? "undo" : "redo");
					return false;
				}
				return true;
			}

			if (method == "SetWaypoint") {
				log->SetWaypoint(std::string(At(arguments, 0).AsText()));
				return true;
			}

			if (method == "ResetWaypoints") {
				if (Owner.HistoryActionInProgress()) {
					failure = "ResetWaypoints cannot run during an editor action";
					return false;
				}
				log->ResetWaypoints();
				return true;
			}

			if (method == "SetEnabled") {
				if (Owner.HistoryActionInProgress()) {
					failure = "SetEnabled cannot run during an editor action";
					return false;
				}
				log->SetEnabled(At(arguments, 0).AsBoolean());
				return true;
			}

			// The four events. A handler replaces whatever this plugin had
			// registered rather than adding to it: one per plugin per event is
			// what a `HostCallback` slot holds, and a list would need a
			// disconnect to go with it.
			const HostValue &handler = At(arguments, 0);
			if (handler.Tag != engine::script::HostTag::Callback) {
				if (method == "OnUndo" || method == "OnRedo" || method == "OnRecordingStarted" ||
					method == "OnRecordingFinished") {
					failure = std::string(method) + " expects a function, and was given " +
							  engine::script::Describe(handler.Tag);
					return false;
				}
				failure = "ChangeHistoryService has no member '" + std::string(method) + "'";
				return false;
			}

			if (method == "OnUndo") {
				Plugin.OnUndo = handler.Callback;
				return true;
			}
			if (method == "OnRedo") {
				Plugin.OnRedo = handler.Callback;
				return true;
			}
			if (method == "OnRecordingStarted") {
				Plugin.OnRecordingStarted = handler.Callback;
				return true;
			}
			if (method == "OnRecordingFinished") {
				Plugin.OnRecordingFinished = handler.Callback;
				return true;
			}

			failure = "ChangeHistoryService has no member '" + std::string(method) + "'";
			return false;
		}

		// Reads a finish operation from an `EnumItem`, a string or a number.
		//
		// An `EnumItem` crosses the seam as its member's name, so the first two
		// are one case. The number is accepted because the ordinals are on a
		// wire anyway and a plugin generated from a table may have one.
		bool ReadOperation(const HostValue &value, FinishOperation &out, std::string &failure) {
			const std::string_view text = value.AsText();
			if (text == "Commit") {
				out = FinishOperation::Commit;
				return true;
			}
			if (text == "Cancel") {
				out = FinishOperation::Cancel;
				return true;
			}
			if (text == "Append") {
				out = FinishOperation::Append;
				return true;
			}
			if (value.Tag == engine::script::HostTag::Number) {
				const double ordinal = value.AsNumber(-1.0);
				if (ordinal >= 0.0 && ordinal <= 2.0) {
					out = static_cast<FinishOperation>(static_cast<uint8_t>(ordinal));
					return true;
				}
			}

			failure = "FinishRecording expects Enum.FinishRecordingOperation.Commit, .Cancel or .Append";
			return false;
		}

		bool Selection(std::string_view name, const HostValue &value, std::string &failure) {
			const std::string method(name.substr(name.find('.') + 1));

			if (value.Tag != HostTag::Array) {
				failure = "Selection:" + method + " expects an array of Instances, and was given " +
						  engine::script::Describe(value.Tag);

				// **The near-misses are named, because they are what somebody
				// actually types.** A bare instance is the common one and a nil
				// is the second; telling them what to write costs a sentence and
				// saves them the guess.
				if (value.Tag == HostTag::Instance) {
					failure += " - write Selection:" + method + "({ instance })";
				} else if (value.Tag == HostTag::Nil) {
					failure += " - write Selection:" + method + "({}) to select nothing";
				}
				return false;
			}

			std::vector<Entity> instances;
			instances.reserve(value.Items.size());

			// What was skipped, in the words the warning uses. Collected rather
			// than logged per item: a plugin passing a hundred stale handles is
			// one mistake and should be one line.
			std::vector<std::string> ignored;

			Owner.WithSelectionWorld([&](Store &store) {
				for (size_t at = 0; at < value.Items.size(); at++) {
					const HostValue &item = value.Items[at];
					const std::string where = "item " + std::to_string(at + 1) + " is ";

					if (item.Tag != HostTag::Instance) {
						// One-based, because that is how the script counted.
						ignored.push_back(where + engine::script::Describe(item.Tag));
						continue;
					}

					// **A destroyed instance is a different skip from a wrong
					// type**, and saying which is most of the value of the
					// warning: one means the plugin's code is wrong and the
					// other means the world moved under it.
					if (!store.Alive(item.Instance)) {
						ignored.push_back(where + "an Instance that no longer exists");
						continue;
					}

					instances.push_back(item.Instance);
				}
			});

			if (!ignored.empty()) {
				// **Capped, because a plugin passing a thousand bad items would
				// otherwise be a line nobody can read.** The count is still
				// exact, which is the part somebody acts on.
				constexpr size_t SHOWN = 3;

				std::string message =
					"Selection:" + method + " ignored " + std::to_string(ignored.size()) + " item(s): ";

				for (size_t at = 0; at < std::min(ignored.size(), SHOWN); at++) {
					message += (at > 0 ? ", " : "") + ignored[at];
				}
				if (ignored.size() > SHOWN) {
					message += ", and " + std::to_string(ignored.size() - SHOWN) + " more";
				}

				// Through the editor's own output, prefixed with the plugin's
				// name, because the person reading it has several installed and
				// needs to know whose mistake it is.
				Owner.Say("[" + Plugin.Manifest.Name + "] " + message, engine::core::LogLevel::Warning);
			}

			// **The list is edited directly rather than through
			// `Editor::Select`, and that is not a shortcut.** That function
			// *toggles* - it is what a ctrl-click calls, so adding something
			// already selected removes it. `Selection:Add` must not, and a
			// plugin adding a part twice must not end up with it deselected.
			std::vector<Entity> selected(static_cast<const std::vector<Entity> &>(Owner.Selection));

			if (method == "Remove") {
				for (const Entity instance : instances) {
					selected.erase(std::remove(selected.begin(), selected.end(), instance), selected.end());
				}
				Owner.ReplaceSelection(Owner.SelectionWorld, std::move(selected));
				return true;
			}

			// **`Set` replaces and `Add` does not**, which is Roblox's split and
			// the reason both exist: a tool that grows a selection would
			// otherwise have to read it, append and write it back.
			//
			// `Set` with an empty array therefore deselects everything, which is
			// the whole of what it means and needs no case of its own. **An
			// array whose every item was skipped clears it too**, which is the
			// honest reading: the plugin asked for a selection of things that
			// are not there.
			if (method == "Set") {
				Owner.ClearRootSelection();
				Owner.SelectionAnchor = engine::ecs::NULL_ENTITY;
				selected.clear();
			} else if (!instances.empty()) {
				Owner.ClearRootSelection();
			}

			// The instances were validated in SelectionWorld, which can be an
			// extra viewport's world rather than Active.
			for (const Entity instance : instances) {
				if (std::find(selected.begin(), selected.end(), instance) == selected.end()) {
					selected.push_back(instance);
				}
			}
			if (method == "Set" && !selected.empty()) {
				Owner.SelectionAnchor = selected.front();
			}
			Owner.ReplaceSelection(Owner.SelectionWorld, std::move(selected));
			return true;
		}

		// The world a script instance's source lives in.
		//
		// **Through `script::ReadSource` rather than off the filesystem**, which
		// is the property that keeps this from being a hole: a source name is a
		// key into this world's own `SourceCache`, so there is no path from one
		// to a file on disk.
		bool GetScriptSource(const HostValue &value, HostValue &result, std::string &failure) {
			const Entity instance = value.AsInstance();
			if (instance == engine::ecs::NULL_ENTITY) {
				failure = "GetScriptSource takes a script instance";
				return false;
			}

			bool found = false;
			std::string text;

			Owner.WithSelectionWorld([&](Store &store) {
				const engine::ecs::ClassId container =
					engine::ecs::Classes::Find(engine::core::Name("LuaSourceContainer"));
				if (!container.IsValid() || !store.IsA(instance, container)) {
					return;
				}

				engine::core::Name path;
				if (!store.GetProperty(instance, engine::core::Name("Source"), &path, sizeof(path))) {
					return;
				}

				std::string error;
				found = engine::script::ReadSource(store, path, text, error);
			});

			if (!found) {
				failure = "that instance carries no readable source";
				return false;
			}

			result = HostValue::Of(std::string_view(text));
			return true;
		}

		bool SetScriptSource(const HostValue &target, const HostValue &text, std::string &failure) {
			const Entity instance = target.AsInstance();
			if (instance == engine::ecs::NULL_ENTITY || text.Tag != HostTag::String) {
				failure = "SetScriptSource takes a script instance and a string";
				return false;
			}

			bool written = false;
			Owner.WithSelectionWorld([&](Store &store) {
				engine::core::Name path;
				if (!store.GetProperty(instance, engine::core::Name("Source"), &path, sizeof(path))) {
					return;
				}

				auto *cache = store.ResourceMutable<engine::script::SourceCache>();
				if (cache == nullptr) {
					store.SetResource(engine::script::SourceCache{});
					cache = store.ResourceMutable<engine::script::SourceCache>();
				}
				if (cache == nullptr) {
					return;
				}

				cache->Set(path, text.Text);
				written = true;
			});

			if (!written) {
				failure = "that instance carries no source to write";
				return false;
			}

			// The file on disk is not this editor's to write from here - a
			// plugin edits the world, and saving the world is what writes it.
			Owner.MarkModified();
			return true;
		}

		bool GetScripts(HostValue &result, std::string &failure) {
			std::vector<HostValue> scripts;

			Owner.WithSelectionWorld([&](Store &store) {
				const engine::ecs::ClassId container =
					engine::ecs::Classes::Find(engine::core::Name("LuaSourceContainer"));
				if (!container.IsValid()) {
					return;
				}

				// Every instance in the world, filtered by class. Deliberately a
				// walk rather than a query: `IsA` is set inclusion over a class
				// tree and `Store` has no term for it.
				store.EachEntity([&](Entity entity) {
					if (store.IsA(entity, container)) {
						scripts.push_back(HostValue::Of(entity));
					}
				});
			});

			if (scripts.empty() && !Owner.HasActiveWorld()) {
				failure = "there is no scene open";
				return false;
			}

			result = HostValue::List(std::move(scripts));
			return true;
		}

		static std::string StableId(const HostValue &value, std::string_view prefix, size_t index) {
			if (value.Tag == HostTag::String && !value.Text.empty()) {
				return value.Text;
			}
			return std::string(prefix) + "-" + std::to_string(index + 1);
		}

		bool HasToolbarId(std::string_view id) const {
			return std::any_of(
				Plugin.Toolbars.begin(), Plugin.Toolbars.end(), [&](const PluginToolbar &toolbar) {
					return toolbar.Id == id;
				}
			);
		}

		bool HasWidgetId(std::string_view id) const {
			return std::any_of(Plugin.Widgets.begin(), Plugin.Widgets.end(), [&](const PluginWidget &widget) {
				return widget.Id == id;
			});
		}

		bool CreateToolbarTrack(
			std::string_view name, HostArguments arguments, HostValue &result, std::string &failure
		) {
			size_t toolbar = 0;
			if (!ToolbarIndexOf(At(arguments, 0), toolbar)) {
				failure = "no such toolbar";
				return false;
			}

			std::vector<PluginToolbarTrack> &tracks =
				name == "CreateToolbarRow" ? Plugin.Toolbars[toolbar].Rows : Plugin.Toolbars[toolbar].Columns;
			const std::string prefix = name == "CreateToolbarRow" ? "row" : "column";
			PluginToolbarTrack track;
			track.Id = StableId(At(arguments, 1), prefix, tracks.size());
			if (name == "CreateToolbarColumn" && At(arguments, 2).Tag != HostTag::Nil) {
				if (At(arguments, 2).Tag != HostTag::Number || !std::isfinite(At(arguments, 2).Number)) {
					failure = "a toolbar column width has to be a finite number";
					return false;
				}
				track.Width = ClampPluginToolWidth(static_cast<float>(At(arguments, 2).Number));
			}
			if (std::any_of(tracks.begin(), tracks.end(), [&](const PluginToolbarTrack &existing) {
					return existing.Id == track.Id;
				})) {
				failure = "a toolbar " + prefix + " with id '" + track.Id + "' already exists";
				return false;
			}

			tracks.push_back(std::move(track));
			result = HostValue::Of(static_cast<double>(tracks.size()));
			Owner.InvalidateToolbarLayout();
			return true;
		}

		bool SetToolCell(HostArguments arguments, std::string &failure) {
			size_t toolbar = 0;
			if (!ToolbarIndexOf(At(arguments, 0), toolbar)) {
				failure = "no such toolbar";
				return false;
			}

			PluginToolbar &target = Plugin.Toolbars[toolbar];
			size_t button = 0;
			size_t row = 0;
			size_t column = 0;
			if (!SurfaceIndexOf(At(arguments, 1), target.Buttons.size(), button)) {
				failure = "no such toolbar control";
				return false;
			}
			if (!SurfaceIndexOf(At(arguments, 2), target.Rows.size(), row)) {
				failure = "no such toolbar row";
				return false;
			}
			if (!SurfaceIndexOf(At(arguments, 3), target.Columns.size(), column)) {
				failure = "no such toolbar column";
				return false;
			}

			target.Buttons[button].Row = target.Rows[row].Id;
			target.Buttons[button].Column = target.Columns[column].Id;
			Owner.InvalidateToolbarLayout();
			return true;
		}

		bool CreateControl(
			std::string_view kind, HostArguments arguments, HostValue &result, std::string &failure
		) {
			size_t toolbar = 0;
			if (!ToolbarIndexOf(At(arguments, 0), toolbar)) {
				failure = "no such toolbar - CreateToolbar answers the id to pass here";
				return false;
			}

			PluginButton button;
			button.Name = std::string(At(arguments, 1).AsText());
			if (button.Name.empty()) {
				failure = "a toolbar control needs a name";
				return false;
			}

			button.Tooltip = std::string(At(arguments, 2).AsText());

			size_t handlerIndex = 3;
			size_t idIndex = 4;
			size_t widthIndex = 5;
			size_t visibleIndex = 6;
			if (kind == "CreateLabel") {
				button.Kind = PluginControlKind::Label;
				handlerIndex = std::numeric_limits<size_t>::max();
				idIndex = 3;
				widthIndex = 4;
				visibleIndex = 5;
			} else if (kind == "CreateToggle") {
				button.Kind = PluginControlKind::Toggle;
				button.Active = At(arguments, 3).AsBoolean();
				handlerIndex = 4;
				idIndex = 5;
				widthIndex = 6;
				visibleIndex = 7;
			} else if (kind == "CreateDropdown") {
				button.Kind = PluginControlKind::Dropdown;
				if (At(arguments, 3).Tag != HostTag::Array) {
					failure = "a dropdown needs an array of option strings";
					return false;
				}
				for (const HostValue &option : At(arguments, 3).Items) {
					if (option.Tag != HostTag::String) {
						failure = "every dropdown option has to be a string";
						return false;
					}
					button.Options.push_back(option.Text);
				}
				if (button.Options.empty()) {
					failure = "a dropdown needs at least one option";
					return false;
				}
				size_t selected = 0;
				if (!SurfaceIndexOf(At(arguments, 4), button.Options.size(), selected)) {
					failure = "a dropdown selection has to name one of its options";
					return false;
				}
				button.Selected = selected;
				handlerIndex = 5;
				idIndex = 6;
				widthIndex = 7;
				visibleIndex = 8;
			}

			if (button.Kind != PluginControlKind::Label) {
				const HostValue &handler = At(arguments, handlerIndex);
				if (handler.Tag == HostTag::Callback) {
					if (button.Kind == PluginControlKind::Button) {
						button.OnClick = handler.Callback;
					} else {
						button.OnChanged = handler.Callback;
					}
				} else if (handler.Tag != HostTag::Nil) {
					failure = "a toolbar control's handler has to be a function";
					return false;
				}
			}

			button.Id = StableId(At(arguments, idIndex), "tool", Plugin.Toolbars[toolbar].Buttons.size());
			if (std::any_of(
					Plugin.Toolbars[toolbar].Buttons.begin(),
					Plugin.Toolbars[toolbar].Buttons.end(),
					[&](const PluginButton &existing) { return existing.Id == button.Id; }
				)) {
				failure = "a toolbar control with id '" + button.Id + "' already exists";
				return false;
			}
			if (At(arguments, widthIndex).Tag == HostTag::Number) {
				if (!std::isfinite(At(arguments, widthIndex).Number)) {
					failure = "a toolbar control width has to be a finite number";
					return false;
				}
				button.Width = ClampPluginToolWidth(static_cast<float>(At(arguments, widthIndex).Number));
			}
			if (At(arguments, visibleIndex).Tag != HostTag::Nil) {
				button.Visible = At(arguments, visibleIndex).AsBoolean();
			}

			Plugin.Toolbars[toolbar].Buttons.push_back(std::move(button));
			result = HostValue::Of(static_cast<double>(Plugin.Toolbars[toolbar].Buttons.size()));
			Owner.InvalidateToolbarLayout();
			return true;
		}

		bool ConfigureToolbar(std::string_view name, HostArguments arguments, std::string &failure) {
			size_t toolbar = 0;
			if (!ToolbarIndexOf(At(arguments, 0), toolbar)) {
				failure = "no such toolbar";
				return false;
			}
			if (name == "SetToolbarVisible") {
				Plugin.Toolbars[toolbar].Visible = At(arguments, 1).AsBoolean();
				Owner.InvalidateToolbarLayout();
				return true;
			}
			if (name == "SetToolbarPlacement") {
				const auto placement = ParsePluginToolbarPlacement(At(arguments, 1).AsText());
				if (!placement.has_value()) {
					failure = "toolbar placement has to be 'Tabbed' or 'Pinned'";
					return false;
				}
				Plugin.Toolbars[toolbar].Placement = *placement;
				Owner.InvalidateToolbarLayout();
				return true;
			}

			size_t button = 0;
			if (!SurfaceIndexOf(At(arguments, 1), Plugin.Toolbars[toolbar].Buttons.size(), button)) {
				failure = "no such toolbar control";
				return false;
			}
			PluginButton &control = Plugin.Toolbars[toolbar].Buttons[button];
			if (name == "SetToolVisible") {
				control.Visible = At(arguments, 2).AsBoolean();
				Owner.InvalidateToolbarLayout();
				return true;
			}
			if (At(arguments, 2).Tag != HostTag::Number || !std::isfinite(At(arguments, 2).Number)) {
				failure = "a toolbar control width has to be a finite number";
				return false;
			}
			control.Width = ClampPluginToolWidth(static_cast<float>(At(arguments, 2).Number));
			Owner.InvalidateToolbarLayout();
			return true;
		}

		bool SetButtonActive(HostArguments arguments, std::string &failure) {
			size_t toolbar = 0;
			size_t button = 0;
			if (!ToolbarIndexOf(At(arguments, 0), toolbar) ||
				!SurfaceIndexOf(At(arguments, 1), Plugin.Toolbars[toolbar].Buttons.size(), button)) {
				failure = "no such button";
				return false;
			}

			Plugin.Toolbars[toolbar].Buttons[button].Active = At(arguments, 2).AsBoolean();
			return true;
		}

		bool Widget(std::string_view name, HostArguments arguments, HostValue &result, std::string &failure) {
			size_t widget = 0;
			if (!SurfaceIndexOf(At(arguments, 0), Plugin.Widgets.size(), widget)) {
				failure = "no such widget - CreateWidget answers the id to pass here";
				return false;
			}

			if (name == "IsWidgetOpen") {
				result = HostValue::Of(Plugin.Widgets[widget].Open);
				return true;
			}
			if (name == "GetWidgetGui") {
				result = HostValue::Of(Plugin.Widgets[widget].Gui);
				return true;
			}
			if (name == "SetWidgetOpen") {
				Plugin.Widgets[widget].Open = At(arguments, 1).AsBoolean();
				if (engine::gui::Layer *layer =
						World.GetMutable<engine::gui::Layer>(Plugin.Widgets[widget].Gui)) {
					layer->Enabled = Plugin.Widgets[widget].Open;
				}
				return true;
			}
			if (name == "SetWidgetColour") {
				return SetWidgetColour(widget, At(arguments, 1), At(arguments, 2), failure);
			}
			if (name == "SetWidgetDock") {
				const std::optional<PluginDock> dock = ParsePluginDock(At(arguments, 1).AsText());
				if (!dock) {
					failure = "a widget dock is Floating, Centre, Left, Right, or Bottom";
					return false;
				}
				Plugin.Widgets[widget].Dock = *dock;
				return true;
			}
			if (name == "SetWidgetSizeConstraints") {
				const double minimumWidth = At(arguments, 1).AsNumber(-1.0);
				const double minimumHeight = At(arguments, 2).AsNumber(-1.0);
				const double maximumWidth = At(arguments, 3).AsNumber(-1.0);
				const double maximumHeight = At(arguments, 4).AsNumber(-1.0);
				if (!std::isfinite(minimumWidth) || !std::isfinite(minimumHeight) ||
					!std::isfinite(maximumWidth) || !std::isfinite(maximumHeight) || minimumWidth < 1.0 ||
					minimumHeight < 1.0 || maximumWidth < 0.0 || maximumHeight < 0.0) {
					failure = "widget constraints need finite positive minimums and non-negative maximums";
					return false;
				}
				PluginWidget &target = Plugin.Widgets[widget];
				target.MinimumWidth = static_cast<float>(minimumWidth);
				target.MinimumHeight = static_cast<float>(minimumHeight);
				target.MaximumWidth =
					maximumWidth > 0.0 ? static_cast<float>(std::max(maximumWidth, minimumWidth)) : 0.0f;
				target.MaximumHeight =
					maximumHeight > 0.0 ? static_cast<float>(std::max(maximumHeight, minimumHeight)) : 0.0f;
				return true;
			}

			const HostValue &handler = At(arguments, 1);
			if (handler.Tag != HostTag::Callback) {
				failure = "SetWidgetRender takes a function";
				return false;
			}

			// **The previous one is released.** A plugin that reassigns a render
			// callback every heartbeat would otherwise hold one reference per
			// frame for the life of the session.
			if (Plugin.Widgets[widget].Render.Valid() && Plugin.Vm != nullptr) {
				Plugin.Vm->Release(Plugin.Widgets[widget].Render);
			}

			Plugin.Widgets[widget].Render = handler.Callback;
			return true;
		}

		bool
		Viewport(std::string_view name, HostArguments arguments, HostValue &result, std::string &failure) {
			if (name == "AddViewport") {
				result = HostValue::Of(static_cast<double>(Owner.AddViewport() + 1));
				return true;
			}

			const std::string option(At(arguments, 0).AsText());
			engine::core::Color3 *colour = nullptr;
			if (option == "Grid Colour" || option == "Grid Color") {
				colour = &Owner.GridSettings.Colour;
			} else if (option == "Grid Axis X Colour" || option == "Grid Axis X Color") {
				colour = &Owner.GridSettings.AxisX;
			} else if (option == "Grid Axis Z Colour" || option == "Grid Axis Z Color") {
				colour = &Owner.GridSettings.AxisZ;
			}
			if (colour != nullptr) {
				if (name == "GetViewportOption") {
					result = HostValue::Of(ColourText(*colour));
					return true;
				}
				if (!ParseColour(At(arguments, 1), *colour)) {
					failure = option + " has to be RRGGBB or RRGGBBAA text";
					return false;
				}
				return true;
			}

			float *number = nullptr;
			double minimum = 0.0;
			double maximum = 0.0;
			bool integer = false;
			if (option == "Grid Step" || option == "Grid Scale") {
				number = &Owner.GridSettings.Step;
				minimum = 0.01;
				maximum = 1000.0;
			} else if (option == "Grid Major") {
				number = &Owner.GridSettings.Major;
				minimum = 1.0;
				maximum = 100.0;
				integer = true;
			} else if (option == "Grid Reach" || option == "Grid Size") {
				number = &Owner.GridSettings.Reach;
				minimum = 1.0;
				maximum = 100000.0;
			} else if (option == "Grid Strength") {
				number = &Owner.GridSettings.Strength;
				maximum = 1.0;
			} else if (option == "Grid Alpha") {
				number = &Owner.GridSettings.Alpha;
				maximum = 1.0;
			} else if (option == "Grid Axis Alpha") {
				number = &Owner.GridSettings.AxisAlpha;
				maximum = 1.0;
			} else if (option == "Grid Offset X") {
				number = &Owner.GridSettings.Offset.X;
				minimum = -1000000.0;
				maximum = 1000000.0;
			} else if (option == "Grid Offset Z") {
				number = &Owner.GridSettings.Offset.Z;
				minimum = -1000000.0;
				maximum = 1000000.0;
			}
			if (number != nullptr) {
				if (name == "GetViewportOption") {
					result = HostValue::Of(static_cast<double>(*number));
					return true;
				}
				const double value = At(arguments, 1).AsNumber(std::numeric_limits<double>::quiet_NaN());
				if (!std::isfinite(value) || value < minimum || value > maximum ||
					(integer && value != std::floor(value))) {
					failure = option + " is outside its supported range";
					return false;
				}
				*number = static_cast<float>(value);
				return true;
			}

			bool *toggle = nullptr;
			if (option == "Grid") {
				toggle = &Owner.ShowGrid;
			} else if (option == "Direction Gizmo") {
				toggle = &Owner.ShowDirectionGizmo;
			} else if (option == "3D Cursor") {
				toggle = &Owner.ShowCursor;
			} else if (option == "Orbit") {
				toggle = &Owner.OrbitCamera;
			} else if (option == "Lock Direction") {
				toggle = &Owner.DirectionLocked;
			} else if (option == "Particles") {
				toggle = &Owner.ShowParticleEmitters;
			} else if (option == "Collider Outlines") {
				toggle = &Owner.ShowColliders;
			}

			if (option == "Camera Speed") {
				if (name == "GetViewportOption") {
					result = HostValue::Of(static_cast<double>(Owner.CameraSpeed));
					return true;
				}
				const double speed = At(arguments, 1).AsNumber(-1.0);
				if (!std::isfinite(speed) || speed < 1.0 || speed > 200.0) {
					failure = "Camera Speed has to be between 1 and 200";
					return false;
				}
				Owner.CameraSpeed = static_cast<float>(speed);
				return true;
			}
			if (toggle == nullptr) {
				failure = "no such viewport option";
				return false;
			}
			if (name == "GetViewportOption") {
				result = HostValue::Of(*toggle);
			} else {
				*toggle = At(arguments, 1).AsBoolean();
			}
			return true;
		}

		// `plugin:OpenScript(script, lineNumber)`. A line puts the caret there.
		bool OpenScript(const HostValue &value, const HostValue &line, std::string &failure) {
			const Entity instance = value.AsInstance();
			if (instance == engine::ecs::NULL_ENTITY || !Owner.Active.IsValid()) {
				failure = "OpenScript takes a script instance in the active world";
				return false;
			}
			bool script = false;
			Owner.Universe->Enter(Owner.Active, [&](Store &store) {
				script = SourceDocumentKindOf(store, instance).has_value();
			});
			if (!script) {
				failure = "OpenScript takes a script instance in the active world";
				return false;
			}
			const double number = line.AsNumber(0.0);
			const int lineNumber =
				std::isfinite(number) && number >= 1.0 ? static_cast<int>(std::min(number, 1.0e9)) : 0;
			Owner.OpenScriptTab(Owner.Active, instance, lineNumber);
			Owner.ShowScripts = true;
			return true;
		}

		// `plugin.SetWidgetColour(id, "Surface", "#2E3440")`, and clearing one.
		//
		// **A name and text rather than an index and a number**, which is the
		// same choice the preferences file makes and for the same reason: a
		// plugin author writing `0xFF403020` has to know imgui's byte order to
		// get it right, and one writing `"#203040"` does not.
		bool
		SetWidgetColour(size_t widget, const HostValue &which, const HostValue &value, std::string &failure) {
			const std::string name(which.AsText());
			const std::optional<engine::ui::ThemeColour> colour = engine::ui::ParseThemeColour(name);
			if (!colour) {
				// The whole list in the message, because there are seven of them
				// and the one somebody meant is certainly in it.
				std::string names;
				for (size_t index = 0; index < engine::ui::THEME_COLOUR_COUNT; index++) {
					names += index > 0 ? ", " : "";
					names += engine::ui::Describe(static_cast<engine::ui::ThemeColour>(index));
				}
				failure = "no colour called '" + name + "' - one of: " + names;
				return false;
			}

			// **Nil clears it**, so a plugin has one call rather than two and
			// the widget goes back to the editor's theme rather than to a colour
			// the plugin had to know.
			if (value.Tag == HostTag::Nil) {
				Plugin.Widgets[widget].Colours[*colour].reset();
				return true;
			}

			const std::optional<unsigned int> packed = engine::ui::ParseColourText(value.AsText());
			if (!packed) {
				failure = "a colour is RRGGBB or RRGGBBAA text, and '" + std::string(value.AsText()) +
						  "' is neither";
				return false;
			}

			Plugin.Widgets[widget].Colours[*colour] = *packed;
			return true;
		}

		bool Draw(std::string_view name, HostArguments arguments, HostValue &result, std::string &failure) {
			const std::string text(At(arguments, 0).AsText());

			if (name == "Label") {
				ImGui::TextWrapped("%s", text.c_str());
				return true;
			}
			if (name == "Separator") {
				ImGui::Separator();
				return true;
			}
			if (name == "Button") {
				result = HostValue::Of(ImGui::Button(text.c_str()));
				return true;
			}
			if (name == "Checkbox") {
				bool value = At(arguments, 1).AsBoolean();
				ImGui::Checkbox(text.c_str(), &value);
				result = HostValue::Of(value);
				return true;
			}
			if (name == "Combo") {
				if (At(arguments, 1).Tag != HostTag::Array) {
					failure = "Combo takes an array of option strings";
					return false;
				}
				std::vector<std::string> options;
				for (const HostValue &option : At(arguments, 1).Items) {
					if (option.Tag != HostTag::String) {
						failure = "every Combo option has to be a string";
						return false;
					}
					options.push_back(option.Text);
				}
				if (options.empty()) {
					failure = "Combo needs at least one option";
					return false;
				}
				size_t selected = 0;
				if (!SurfaceIndexOf(At(arguments, 2), options.size(), selected)) {
					failure = "Combo selection has to name one of its options";
					return false;
				}
				if (ImGui::BeginCombo(text.c_str(), options[selected].c_str())) {
					for (size_t index = 0; index < options.size(); index++) {
						if (ImGui::Selectable(options[index].c_str(), index == selected)) {
							selected = index;
						}
					}
					ImGui::EndCombo();
				}
				result = HostValue::Of(static_cast<double>(selected + 1));
				return true;
			}
			if (name == "InputText") {
				// **A fixed buffer, and the length is stated rather than
				// implied.** A plugin's text field is not where somebody edits a
				// script - that is the script editor - so a growing buffer per
				// field per frame would be an allocation nobody needed.
				char buffer[512] = {};
				const std::string initial(At(arguments, 1).AsText());
				const size_t copied = std::min(initial.size(), sizeof(buffer) - 1);
				std::copy_n(initial.begin(), copied, buffer);

				ImGui::InputText(text.c_str(), buffer, sizeof(buffer));
				result = HostValue::Of(std::string_view(buffer));
				return true;
			}

			if (Plugin.Bindings != nullptr) {
				return Plugin.Bindings->Call(Plugin.Language, name, arguments, result, failure);
			}
			failure = "no such widget call";
			return false;
		}

		Editor &Owner;
		LoadedPlugin &Plugin;
		Store &World;
	};

	std::unique_ptr<engine::script::HostSurface>
	MakePluginSurface(Editor &editor, LoadedPlugin &plugin, Store &store) {
		return std::make_unique<PluginSurface>(editor, plugin, store);
	}

	bool RunPluginHistoryCall(
		LoadedPlugin &plugin,
		std::string_view method,
		engine::script::HostArguments arguments,
		engine::script::HostValue &result,
		std::string &failure
	) {
		if (plugin.Surface == nullptr) {
			failure = "the plugin stopped before its history call ran";
			return false;
		}
		return static_cast<PluginSurface &>(*plugin.Surface).History(method, arguments, result, failure);
	}

	void SetPluginDrawing(engine::script::HostSurface &surface, bool drawing) {
		// A `static_cast` rather than a virtual, because `Drawing` is this
		// editor's own gate and not part of what a host surface *is* - a second
		// host would have no use for it.
		static_cast<PluginSurface &>(surface).Drawing = drawing;
	}
}
