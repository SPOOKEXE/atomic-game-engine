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
//     plugin.SetWidgetRender(panel, function()
//         plugin.Label("Selected: " .. #Selection:Get())
//         if plugin.Button("Clear") then Selection:Set({}) end
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
// a save file carries - a plugin is a tool running in an editor on a project
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

#include <algorithm>
#include <cmath>
#include <imgui.h>
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
	// on every call - which the seam has no way to supply and a plugin could
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
				"GetActiveWorld",

				// **`Selection` is a service, which is Roblox's own shape.**
				// A dotted name becomes a global table of methods - see
				// `OpenHost` - so `game:GetService("Selection")` finds it for
				// free, and `Selection:Get()` is what a Roblox plugin author
				// already types.
				"Selection.Get",
				"Selection.Set",
				"Selection.Add",
				"Selection.Remove",

				// **`ChangeHistoryService` is how a plugin tells the editor what
				// one undo should reverse**, and since v0.13 it is also how a
				// plugin's edits reach the other people in a team-create
				// session - a committed recording is the unit that travels.
				// Roblox's shape, method for method.
				"ChangeHistoryService.TryBeginRecording",
				"ChangeHistoryService.FinishRecording",
				"ChangeHistoryService.IsRecordingInProgress",
				"ChangeHistoryService.GetCanUndo",
				"ChangeHistoryService.GetCanRedo",
				"ChangeHistoryService.Undo",
				"ChangeHistoryService.Redo",
				"ChangeHistoryService.SetWaypoint",
				"ChangeHistoryService.ResetWaypoints",
				"ChangeHistoryService.SetEnabled",
				"ChangeHistoryService.OnUndo",
				"ChangeHistoryService.OnRedo",
				"ChangeHistoryService.OnRecordingStarted",
				"ChangeHistoryService.OnRecordingFinished",

				// Scripts in the world being edited.
				"GetScriptSource",
				"SetScriptSource",
				"GetScripts",

				// Toolbars and buttons.
				"CreateToolbar",
				"CreateButton",
				"CreateToggle",
				"CreateDropdown",
				"SetButtonActive",
				"SetToolVisible",
				"SetToolWidth",
				"SetToolbarVisible",

				// Docked panels, and what may be drawn in one.
				"CreateWidget",
				"SetWidgetRender",
				"SetWidgetOpen",
				"IsWidgetOpen",
				"SetWidgetColour",
				"SetWidgetDock",
				"SetWidgetSizeConstraints",

				// Viewport and script editor integration.
				"GetViewportOption",
				"SetViewportOption",
				"AddViewport",
				"OpenScript",
				"Label",
				"Button",
				"Checkbox",
				"Combo",
				"Separator",
				"InputText",
			};
		}

		bool Call(
			std::string_view name, HostArguments arguments, HostValue &result, std::string &failure
		) override {
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
			if (name == "CreateToolbar") {
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
				Plugin.Toolbars.push_back(std::move(toolbar));
				result = HostValue::Of(static_cast<double>(Plugin.Toolbars.size()));
				return true;
			}
			if (name == "CreateButton" || name == "CreateToggle" || name == "CreateDropdown") {
				return CreateControl(name, arguments, result, failure);
			}
			if (name == "SetButtonActive") {
				return SetButtonActive(arguments, failure);
			}
			if (name == "SetToolVisible" || name == "SetToolWidth" || name == "SetToolbarVisible") {
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
				Plugin.Widgets.push_back(std::move(widget));
				result = HostValue::Of(static_cast<double>(Plugin.Widgets.size()));
				return true;
			}
			if (name == "SetWidgetRender" || name == "SetWidgetOpen" || name == "IsWidgetOpen" ||
				name == "SetWidgetColour" || name == "SetWidgetDock" || name == "SetWidgetSizeConstraints") {
				return Widget(name, arguments, result, failure);
			}
			if (name == "GetViewportOption" || name == "SetViewportOption" || name == "AddViewport") {
				return Viewport(name, arguments, result, failure);
			}
			if (name == "OpenScript") {
				return OpenScript(At(arguments, 0), failure);
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
		// **Two differences from Roblox, both forced by the seam and both worth
		// stating rather than discovering.**
		//
		// `GetCanUndo` and `GetCanRedo` return a *table* rather than a tuple,
		// because a host call answers one value. A Roblox script writes
		// `local can, name = ChangeHistoryService:GetCanUndo()`; here that is
		// `local can, name = table.unpack(ChangeHistoryService:GetCanUndo())`.
		//
		// The events are calls that take a handler rather than
		// `RBXScriptSignal`s with `:Connect`, because the seam has no signal
		// type and inventing one for four events would be a second way for a
		// script to hear about something. `ChangeHistoryService.OnUndo(f)`.
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
				// The third argument is Roblox's `finalOptions`, forwarded to
				// the finish handler. Nothing in this editor reads it yet, and
				// accepting it costs nothing against refusing a call a plugin
				// already writes.
				result = HostValue::Of(log->FinishRecording(At(arguments, 0).AsText(), operation));
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
				result = HostValue::List(std::move(answer));
				return true;
			}

			if (method == "Undo" || method == "Redo") {
				// **Raises rather than answering false when there is nothing**,
				// which is Roblox's behaviour: a plugin that walks the history
				// without asking `GetCanUndo` first has a bug, and a silent
				// no-op is how that bug survives to the next release.
				const bool moved = method == "Undo" ? log->Undo() : log->Redo();
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
				log->ResetWaypoints();
				return true;
			}

			if (method == "SetEnabled") {
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
			std::vector<Entity> &selected = Owner.Selection;

			if (method == "Remove") {
				for (const Entity instance : instances) {
					selected.erase(std::remove(selected.begin(), selected.end(), instance), selected.end());
				}
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
				Owner.ClearSelection();
			}

			// The world has to follow the instances, or the selection is a list
			// of handles into a store nothing is looking at.
			if (!instances.empty()) {
				Owner.SelectionWorld = Owner.Active;
			}

			for (const Entity instance : instances) {
				if (std::find(selected.begin(), selected.end(), instance) == selected.end()) {
					selected.push_back(instance);
				}
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

		bool CreateControl(
			std::string_view kind, HostArguments arguments, HostValue &result, std::string &failure
		) {
			size_t toolbar = 0;
			if (!SurfaceIndexOf(At(arguments, 0), Plugin.Toolbars.size(), toolbar)) {
				failure = "no such toolbar - CreateToolbar answers the id to pass here";
				return false;
			}

			PluginButton button;
			button.Name = std::string(At(arguments, 1).AsText());
			if (button.Name.empty()) {
				failure = "a button needs a name";
				return false;
			}

			button.Tooltip = std::string(At(arguments, 2).AsText());

			size_t handlerIndex = 3;
			size_t idIndex = 4;
			size_t widthIndex = 5;
			size_t visibleIndex = 6;
			if (kind == "CreateToggle") {
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
				button.Width = ClampPluginToolWidth(static_cast<float>(At(arguments, widthIndex).Number));
			}
			if (At(arguments, visibleIndex).Tag != HostTag::Nil) {
				button.Visible = At(arguments, visibleIndex).AsBoolean();
			}

			Plugin.Toolbars[toolbar].Buttons.push_back(std::move(button));
			result = HostValue::Of(static_cast<double>(Plugin.Toolbars[toolbar].Buttons.size()));
			return true;
		}

		bool ConfigureToolbar(std::string_view name, HostArguments arguments, std::string &failure) {
			size_t toolbar = 0;
			if (!SurfaceIndexOf(At(arguments, 0), Plugin.Toolbars.size(), toolbar)) {
				failure = "no such toolbar";
				return false;
			}
			if (name == "SetToolbarVisible") {
				Plugin.Toolbars[toolbar].Visible = At(arguments, 1).AsBoolean();
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
				return true;
			}
			if (At(arguments, 2).Tag != HostTag::Number || !std::isfinite(At(arguments, 2).Number)) {
				failure = "a toolbar control width has to be a finite number";
				return false;
			}
			control.Width = ClampPluginToolWidth(static_cast<float>(At(arguments, 2).Number));
			return true;
		}

		bool SetButtonActive(HostArguments arguments, std::string &failure) {
			size_t toolbar = 0;
			size_t button = 0;
			if (!SurfaceIndexOf(At(arguments, 0), Plugin.Toolbars.size(), toolbar) ||
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
			if (name == "SetWidgetOpen") {
				Plugin.Widgets[widget].Open = At(arguments, 1).AsBoolean();
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

		bool OpenScript(const HostValue &value, std::string &failure) {
			const Entity instance = value.AsInstance();
			if (instance == engine::ecs::NULL_ENTITY || !Owner.Active.IsValid()) {
				failure = "OpenScript takes a script instance in the active world";
				return false;
			}
			bool script = false;
			Owner.Universe->Enter(Owner.Active, [&](Store &store) {
				const engine::ecs::ClassId container =
					engine::ecs::Classes::Find(engine::core::Name("LuaSourceContainer"));
				script = container.IsValid() && store.Alive(instance) && store.IsA(instance, container);
			});
			if (!script) {
				failure = "OpenScript takes a script instance in the active world";
				return false;
			}
			Owner.OpenScriptTab(Owner.Active, instance);
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
		// editor's own gate and not part of what a host surface *is* - a second
		// host would have no use for it.
		static_cast<PluginSurface &>(surface).Drawing = drawing;
	}
}
