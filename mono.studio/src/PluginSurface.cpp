// What the editor offers a plugin, over `script::HostSurface`.
//
// **The names are Roblox's where Roblox has one**, because a person who has
// written a Studio plugin should be able to write one of these without learning
// a second vocabulary for the same idea. Where the shape differs it is because
// the seam carries values rather than objects — `plugin.CreateButton(toolbar,
// ...)` takes the toolbar's id where Roblox's takes the toolbar — and that is
// stated rather than smoothed over.
//
//     local toolbar = plugin.CreateToolbar("My Tools")
//     plugin.CreateButton(toolbar, "Align", "Align the selection", function()
//         for _, part in plugin.GetSelection() do
//             part.CFrame = CFrame.new(0, part.Position.Y, 0)
//         end
//     end)
//
//     local panel = plugin.CreateWidget("Align", true)
//     plugin.SetWidgetRender(panel, function()
//         plugin.Label("Selected: " .. #plugin.GetSelection())
//         if plugin.Button("Clear") then plugin.SetSelection({}) end
//     end)
//
// ## Three groups, and the third is the one with a rule
//
// **The world half needs no host call at all** and is deliberately absent here:
// `Instance`, `workspace` and `World` are the engine's own surface, and a plugin
// gets them because it is a script. What is here is only what an *editor* has.
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
// a save file carries — a plugin is a tool running in an editor on a project
// somebody opened, so it is reading what its user is already looking at.
//
// It is *not* a way out of the sandbox: it reads through `script::ReadSource`
// against this world's own `SourceCache`, so there is no path from a name to a
// file on disk. A plugin cannot read `/etc/passwd` by calling it, which is the
// property that matters.

#include <engine/core/Log.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/script/Instances.hpp>
#include <engine/script/SourceCache.hpp>
#include <imgui.h>
#include <studio/Editor.hpp>
#include <studio/Plugins.hpp>

#include <algorithm>
#include <string>
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
		bool IndexOf(const HostValue &value, size_t count, size_t &out) {
			const double number = value.AsNumber(0.0);
			if (number < 1.0 || number > static_cast<double>(count)) {
				return false;
			}
			out = static_cast<size_t>(number) - 1;
			return true;
		}
	}

	// The editor's half of the seam, one per plugin.
	//
	// **Per plugin rather than one shared**, because every call has to know
	// which plugin made it: a toolbar belongs to the plugin that created it, and
	// a surface shared between them would need the caller's identity passed in
	// on every call — which the seam has no way to supply and a plugin could
	// forge.
	class PluginSurface final : public engine::script::HostSurface {
	  public:
		PluginSurface(Editor &editor, LoadedPlugin &plugin) : Owner(editor), Plugin(plugin) {}

		std::string_view GlobalName() const override {
			return "plugin";
		}

		std::vector<std::string> Names() const override {
			return {
				// The editor.
				"Notify",
				"GetSelection",
				"SetSelection",
				"GetActiveWorld",

				// Scripts in the world being edited.
				"GetScriptSource",
				"SetScriptSource",
				"GetScripts",

				// Toolbars and buttons.
				"CreateToolbar",
				"CreateButton",
				"SetButtonActive",

				// Docked panels, and what may be drawn in one.
				"CreateWidget",
				"SetWidgetRender",
				"SetWidgetOpen",
				"IsWidgetOpen",
				"Label",
				"Button",
				"Checkbox",
				"Separator",
				"InputText",
			};
		}

		bool Call(std::string_view name, HostArguments arguments, HostValue &result, std::string &failure)
			override {
			// A plain chain rather than a table of member pointers: nineteen
			// names, each a handful of lines, and a dispatch table would be a
			// second list to keep in step with `Names`.
			if (name == "Notify") {
				Owner.Say("[" + Plugin.Manifest.Name + "] " + std::string(At(arguments, 0).AsText()));
				return true;
			}
			if (name == "GetActiveWorld") {
				result = HostValue::Of(std::string_view(Owner.ActiveWorldName()));
				return true;
			}
			if (name == "GetSelection") {
				std::vector<HostValue> selected;
				for (const Entity instance : Owner.Selection) {
					selected.push_back(HostValue::Of(instance));
				}
				result = HostValue::List(std::move(selected));
				return true;
			}
			if (name == "SetSelection") {
				return SetSelection(At(arguments, 0), failure);
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
			if (name == "CreateToolbar") {
				PluginToolbar toolbar;
				toolbar.Name = std::string(At(arguments, 0).AsText());
				if (toolbar.Name.empty()) {
					failure = "a toolbar needs a name";
					return false;
				}
				Plugin.Toolbars.push_back(std::move(toolbar));
				result = HostValue::Of(static_cast<double>(Plugin.Toolbars.size()));
				return true;
			}
			if (name == "CreateButton") {
				return CreateButton(arguments, result, failure);
			}
			if (name == "SetButtonActive") {
				return SetButtonActive(arguments, failure);
			}
			if (name == "CreateWidget") {
				PluginWidget widget;
				widget.Title = std::string(At(arguments, 0).AsText());
				if (widget.Title.empty()) {
					failure = "a widget needs a title";
					return false;
				}
				widget.Open = At(arguments, 1).AsBoolean();
				Plugin.Widgets.push_back(std::move(widget));
				result = HostValue::Of(static_cast<double>(Plugin.Widgets.size()));
				return true;
			}
			if (name == "SetWidgetRender" || name == "SetWidgetOpen" || name == "IsWidgetOpen") {
				return Widget(name, arguments, result, failure);
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
		bool SetSelection(const HostValue &value, std::string &failure) {
			if (value.Tag != HostTag::Array && value.Tag != HostTag::Nil) {
				failure = "SetSelection takes a list of instances";
				return false;
			}

			Owner.ClearSelection();
			for (const HostValue &item : value.Items) {
				const Entity instance = item.AsInstance();
				if (instance == engine::ecs::NULL_ENTITY) {
					failure = "SetSelection takes a list of instances";
					return false;
				}
				Owner.Select(Owner.SelectionWorld, instance, true);
			}
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

			// The file on disk is not this editor's to write from here — a
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

		bool CreateButton(HostArguments arguments, HostValue &result, std::string &failure) {
			size_t toolbar = 0;
			if (!IndexOf(At(arguments, 0), Plugin.Toolbars.size(), toolbar)) {
				failure = "no such toolbar — CreateToolbar answers the id to pass here";
				return false;
			}

			PluginButton button;
			button.Name = std::string(At(arguments, 1).AsText());
			if (button.Name.empty()) {
				failure = "a button needs a name";
				return false;
			}

			button.Tooltip = std::string(At(arguments, 2).AsText());

			const HostValue &handler = At(arguments, 3);
			if (handler.Tag == HostTag::Callback) {
				button.OnClick = handler.Callback;
			} else if (handler.Tag != HostTag::Nil) {
				failure = "a button's handler has to be a function";
				return false;
			}

			Plugin.Toolbars[toolbar].Buttons.push_back(std::move(button));
			result = HostValue::Of(static_cast<double>(Plugin.Toolbars[toolbar].Buttons.size()));
			return true;
		}

		bool SetButtonActive(HostArguments arguments, std::string &failure) {
			size_t toolbar = 0;
			size_t button = 0;
			if (!IndexOf(At(arguments, 0), Plugin.Toolbars.size(), toolbar) ||
				!IndexOf(At(arguments, 1), Plugin.Toolbars[toolbar].Buttons.size(), button)) {
				failure = "no such button";
				return false;
			}

			Plugin.Toolbars[toolbar].Buttons[button].Active = At(arguments, 2).AsBoolean();
			return true;
		}

		bool Widget(std::string_view name, HostArguments arguments, HostValue &result, std::string &failure) {
			size_t widget = 0;
			if (!IndexOf(At(arguments, 0), Plugin.Widgets.size(), widget)) {
				failure = "no such widget — CreateWidget answers the id to pass here";
				return false;
			}

			if (name == "IsWidgetOpen") {
				result = HostValue::Of(Plugin.Widgets[widget].Open);
				return true;
			}
			if (name == "SetWidgetOpen") {
				Plugin.Widgets[widget].Open = At(arguments, 1).AsBoolean();
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
			if (name == "InputText") {
				// **A fixed buffer, and the length is stated rather than
				// implied.** A plugin's text field is not where somebody edits a
				// script — that is the script editor — so a growing buffer per
				// field per frame would be an allocation nobody needed.
				char buffer[512] = {};
				const std::string initial(At(arguments, 1).AsText());
				const size_t copied = std::min(initial.size(), sizeof(buffer) - 1);
				std::copy_n(initial.begin(), copied, buffer);

				ImGui::InputText(text.c_str(), buffer, sizeof(buffer));
				result = HostValue::Of(std::string_view(buffer));
				return true;
			}

			failure = "no such widget call";
			return false;
		}

		Editor &Owner;
		LoadedPlugin &Plugin;
	};

	std::unique_ptr<engine::script::HostSurface> MakePluginSurface(Editor &editor, LoadedPlugin &plugin) {
		return std::make_unique<PluginSurface>(editor, plugin);
	}

	void SetPluginDrawing(engine::script::HostSurface &surface, bool drawing) {
		// A `static_cast` rather than a virtual, because `Drawing` is this
		// editor's own gate and not part of what a host surface *is* — a second
		// host would have no use for it.
		static_cast<PluginSurface &>(surface).Drawing = drawing;
	}
}
