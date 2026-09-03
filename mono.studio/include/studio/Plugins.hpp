#pragma once

// Native C++ plugins and isolated script plugins hosted by Studio.
//
// Native definitions own editor adapters and may publish value-shaped bindings.
// Script plugins keep the engine's existing sandbox, step budget, memory ceiling,
// and one runtime per instance. Both contribute the same presentation records,
// so toolbar and widget composition does not care which language owns a tool.
// Binding scopes are move-only cleanup handles: closing one removes every Luau
// or JavaScript function the native plugin installed.
//
//     ~/Documents/atomic-game-engine/studio/plugins/
//       align-tool/
//         plugin.json     { "name": "Align", "main": "main.luau" }
//         main.luau
//
// ## What a plugin can reach, and why it needs no new surface for it
//
// Everything a game script can: `Instance`, `workspace`, `game`, the datatypes,
// and - since v0.12 - `World`, the ECS underneath. That last one is what makes a
// tool possible without an editor API: a plugin declares a component, queries
// for entities carrying one, and writes values back, all through the same
// storage the editor is looking at.
//
// **The selection is a component, not a function call.** `studio.Selected` is a
// described component the editor puts on whatever is selected and takes off
// whatever is not, so a plugin reads it with `World:Query("studio.Selected")`
// and changes it with `entity:SetComponent("studio.Selected", {})`. The editor
// reads it back the same frame.
//
// That is deliberate rather than a shortcut around a missing API. A selection
// *is* per-entity state about the world, which is what a component is for - and
// putting it in the store means a plugin, a C++ system and the properties panel
// are three readers of one fact rather than three copies of it. `ecs/AGENTS.md`
// rule 2 is the argument, and it applies to the editor's own state as much as to
// a game's.
//
// ## What a plugin reaches of the editor
//
// Everything above is the *world*. The editor itself arrives through
// `script::HostSurface` - one seam, a value tree, no `lua_State` crossing a
// module boundary - as a `plugin` global:
//
//     local Selection = game:GetService("Selection")
//     local bar = plugin.CreateToolbar("My Tools")
//
//     plugin.CreateButton(bar, "Align", "Align the selection", function()
//         for _, part in Selection:Get() do
//             part.CFrame = CFrame.new(0, part.Position.Y, 0)
//         end
//     end)
//
//     local panel = plugin.CreateWidget("Align", true)
//     local root = plugin.GetWidgetGui(panel)
//     local button = Instance.new("TextButton")
//     button.Size = UDim2.fromScale(1, 1)
//     button.Text = "Clear selection"
//     button.Parent = root
//     plugin.SetWidgetRender(panel, function()
//         plugin.Label("Selected: " .. #Selection:Get())
//         if plugin.Button("Clear") then Selection:Set({}) end
//     end)
//
// | Group | Calls |
// |---|---|
// | the editor | `plugin.Notify`, `plugin.GetActiveWorld` |
// | the selection | `Selection:Get`, `:Set`, `:Add`, `:Remove` |
// | scripts in the scene | `plugin.GetScripts`, `.GetScriptSource`, `.SetScriptSource` |
// | toolbars | `plugin.CreateToolbar`, `.CreateToolbarTab`, `.CreateToolbarRow`,
// `.CreateToolbarColumn`, `.CreateButton`, `.CreateToggle`, `.CreateDropdown`,
// `.CreateLabel`, `.SetToolCell`, `.SetToolVisible`, `.SetToolWidth`,
// `.SetToolbarVisible`, `.SetToolbarPlacement` |
// | panels | `plugin.CreateWidget`, `.GetWidgetGui`, `.SetWidgetRender`, `.SetWidgetOpen`,
// `.SetWidgetDock`, `.SetWidgetSizeConstraints` |
// | viewport | `plugin.GetViewportOption`, `.SetViewportOption`, `.AddViewport` |
// | script editor | `plugin.OpenScript`, `.GetScriptSource`, `.SetScriptSource` |
// | inside a panel | `plugin.Label`, `.Button`, `.Checkbox`, `.Combo`, `.Separator`,
// `.InputText` |
//
// Viewport options use stable text names. `Grid`, `Particles`, `Direction
// Gizmo`, `3D Cursor`, `Orbit`, `Lock Direction`, and `Collider Outlines` are
// booleans. `Camera Speed`, `Grid Step` (`Grid Scale`), `Grid Major`, `Grid
// Reach` (`Grid Size`), `Grid Strength`, `Grid Alpha`, `Grid Axis Alpha`,
// `Grid Offset X`, and `Grid Offset Z` are numbers. `Grid Colour`, `Grid Axis X
// Colour`, and `Grid Axis Z Colour` take `RRGGBB` or `RRGGBBAA` text; `Color`
// spellings are accepted too.
//
// **`Selection` is a service and the rest is a table**, which is Roblox's own
// split rather than an inconsistency: a selection is a thing the editor *has*,
// so it is reached with `game:GetService` like every other service, and the
// plugin table is what this plugin is doing. A dotted host name is what builds
// it - `script/Host.hpp` - so `Selection:Get()`, `Selection.Get()` and
// `game:GetService("Selection")` are one object reached three ways.
//
// **`:Set`, `:Add` and `:Remove` take an array of `Instance`**, and the two ways
// that can go wrong get two different answers.
//
// **The argument being the wrong shape is refused.** Not a bare instance, not
// nil, not a table of named keys - each is a near-miss somebody types before
// reading anything, and accepting one would make their mistake read as "nothing
// happened". The refusal names what was given and, for the two common ones, what
// to write instead.
//
// **An item that is not selectable is skipped, with one warning per call.** A
// value of the wrong type is one case and an `Instance` that has been destroyed
// is the other, and the warning says which - the first means the plugin's code
// is wrong and the second means the world moved under it. Neither fails the
// call: a plugin selecting the results of a query it ran three frames ago should
// end up with the ones that are still there rather than an error it can do
// nothing about.
//
// The warning is capped at three named items with an exact count beside it,
// because a plugin passing a hundred stale handles is one mistake and should be
// one line.
//
// **`Selection:Set({})` deselects everything**, which is the whole of what an
// empty array means and needs no case of its own - as does a `Set` whose every
// item was skipped, which is the honest reading: the plugin asked for a
// selection of things that are not there. It is also why the binding
// reads an empty Luau table as an array rather than as a map - `{}` is one
// value and the reader has to pick, and this is the call that would otherwise
// have been refused.
//
// **`SelectionChanged` is not there.** A signal needs a connection list in the
// plugin's VM and a fan-out from the editor's frame, which the seam has no
// shape for yet; a plugin that has to react polls `Selection:Get()` on its
// heartbeat. Stated rather than left to be discovered.
//
// **Ids for editor objects, entities for world objects.** A `HostValue` has no
// userdata to hang a toolbar or dock window on, so their configuration calls
// take numeric handles. `GetWidgetGui` returns an entity because the retained
// collector is an ordinary world instance and the script already knows how to
// parent GUI instances to one.
//
// **The panel calls are only legal while a panel is drawing.** They are
// immediate-mode ImGui underneath, which is how every other panel in this editor
// works, so a `plugin.Label` from a heartbeat would draw into whatever window
// the editor happened to be building. The gate is set by the editor around the
// invoke rather than promised by the plugin.
//
// **Reading another script's source is not a hole in the sandbox.**
// `GetScriptSource` goes through `script::ReadSource` against this world's own
// `SourceCache`, so a name resolves to text the editor already holds and never
// to a file on disk. A plugin is a tool running on a project somebody opened; it
// reads what its user is already looking at.
//
// ## One runtime per script instance, and one failure each
//
// **Every script plugin gets its own `script::Runtime`.** Two plugins sharing one would
// share a global table, a step budget and a memory ceiling, so a plugin that
// looped would stop the others and a plugin that set a global would be read by
// them. The cost is a VM per plugin, which is the same trade the engine already
// makes per world.
//
// **A plugin that fails is switched off and named.** It does not stop the load,
// it does not stop the beat, and the reason is kept where somebody can read it -
// the same rule the universe sync follows one file over, for the same reason: an
// author with five plugins and one mistake has to be told which.
//
// @tier client

#include <engine/ecs/Store.hpp>
#include <engine/gui/Compile.hpp>
#include <engine/gui/Input.hpp>
#include <engine/script/Host.hpp>
#include <engine/script/Runtime.hpp>
#include <engine/ui/Theme.hpp>
#include <engine/world/Universe.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace studio {
	class Editor;

	// One place a plugin may run while it is hosted by Studio.
	//
	// @since v0.21
	enum class PluginRunTarget : uint8_t {
		Studio = 1u << 0,
		PlaytestServer = 1u << 1,
		PlaytestClient = 1u << 2,
	};

	using PluginRunTargets = uint8_t;

	// Target mask helpers and stable manifest text.
	// @since v0.21
	//@{
	constexpr PluginRunTargets PluginTarget(PluginRunTarget target) {
		return static_cast<PluginRunTargets>(target);
	}

	constexpr bool RunsIn(PluginRunTargets targets, PluginRunTarget target) {
		return (targets & PluginTarget(target)) != 0;
	}

	const char *Describe(PluginRunTarget target);
	std::optional<PluginRunTarget> ParsePluginRunTarget(std::string_view text);
	//@}

	// The component the editor's selection is published as.
	//
	// **A tag - no fields - because the fact is binary.** Whether an instance is
	// selected is the whole of it; anything else about the selection is a
	// property of the *editor*, not of the instance, and belongs where the rest
	// of the editor's state does.
	//
	// @since v0.12
	inline constexpr std::string_view SELECTED_COMPONENT = "studio.Selected";

	// Registers `studio.Selected`, so a plugin can name it before anything is
	// selected.
	//
	// **Idempotent, and called when the editor starts rather than when the first
	// thing is selected.** A component registered mid-session takes an id
	// decided by how long somebody browsed before clicking, and `ecs::Components`
	// exists to stop exactly that.
	//
	// @return `true` when the component is registered and usable.
	// @since v0.12
	bool RegisterSelectionComponent();

	// The immediate-mode control represented by one toolbar item.
	//
	// @since v0.20
	enum class PluginControlKind : uint8_t {
		Button,
		Toggle,
		Dropdown,
		Label,
		Builtin,
	};

	// Where a toolbar is composed in Studio's top strip.
	// @since v0.20
	enum class PluginToolbarPlacement : uint8_t {
		Tabbed,
		Pinned,
	};

	// Stable text for script/config input.
	// @since v0.20
	//@{
	const char *Describe(PluginToolbarPlacement placement);
	std::optional<PluginToolbarPlacement> ParsePluginToolbarPlacement(std::string_view text);
	//@}

	// One native control group contributed by the Default Studio plugin.
	//
	// Keeping these as data is what lets the toolbar editor move and hide the
	// built-in controls through the same path as an installed plugin's controls.
	//
	// @since v0.20
	enum class BuiltinStudioTool : uint8_t {
		None,
		Play,
		PlayHere,
		Run,
		Pause,
		Stop,
		SpawnPlayer,
		RemovePlayer,
		PlayerCount,
		ViewportName,
		SceneSelector,
		WorldState,
		InsertObject,
		SelectMode,
		MoveMode,
		RotateMode,
		ScaleMode,
		SnapToggle,
		SnapDistance,
		SnapDegrees,
		ScaleFaces,
		Anchor,
		Lock,
		Align,
		Facing,
		EditPivot,
		ResetPivot,
		PivotNotice,
		Duplicate,
		Delete,
		Deselect,
		Undo,
		Redo,
		SelectionCount,
		CreateScript,
		CreateLocalScript,
		CreateModuleScript,
		ScriptDestination,
		ScriptEditorPanel,
		DebuggerPanel,
		CommandBarPanel,
		Grid,
		Particles,
		ViewportIndicator,
		Cursor3D,
		OrbitAroundCursor,
		DirectionLock,
		ExplorerPanel,
		PropertiesPanel,
		OutputPanel,
		AssetsPanel,
		StatisticsPanel,
		FrameGraphPanel,
		HeapPanel,
		DatasetEditorPanel,
		CameraSpeed,
		PluginReload,
		PluginManage,
		ToolbarEditor,
		DockWidgetEditor,
		PluginStatus,
		DemoNodes,
		DemoDescription,
	};

	// One native panel declared by the Default Studio plugin.
	//
	// The declaration belongs to the plugin layer, while the body remains the
	// native adapter that already owns its ECS and editor interactions.
	//
	// @since v0.21
	enum class BuiltinStudioPanel : uint8_t {
		None,
		Explorer,
		Properties,
		ComponentInspector,
		ScriptEditor,
		DatasetEditor,
		RobloxImport,
	};

	// Where a plugin asks its dock widget to appear on first use.
	//
	// The person's saved ImGui layout wins after first use.
	//
	// @since v0.20
	enum class PluginDock : uint8_t {
		Floating,
		Centre,
		Left,
		Right,
		Bottom,
	};

	// Stable text for a dock target and its parser for script/config input.
	// @since v0.20
	//@{
	const char *Describe(PluginDock dock);
	std::optional<PluginDock> ParsePluginDock(std::string_view text);
	//@}

	// What a `plugin.json` says.
	//
	// @since v0.12
	struct PluginManifest {
		// What it calls itself, for a list and a log line.
		std::string Name;

		// One sentence, shown beside the name.
		std::string Description;

		// The entry script, relative to the plugin's own folder.
		//
		// **Relative and refused if it escapes**, which is the one piece of
		// path handling here that is a decision rather than plumbing: a `main`
		// of `../../../../etc/passwd` is a plugin reading a file outside its
		// folder, and a manifest is a file somebody downloaded.
		std::string Main = "main.luau";

		// Whether the editor should run it. A plugin somebody switched off stays
		// on disk and stays listed.
		bool Enabled = true;

		// Stable identity used by toolbar and manager preferences. Empty in a
		// manifest means the plugin folder name is used during discovery.
		std::string Id;

		// Optional manager metadata.
		//@{
		std::string Version;
		std::string Author;
		//@}

		// The Studio-owned contexts that receive an instance. Old manifests run
		// only while authoring, which preserves their existing behaviour.
		PluginRunTargets Runs = PluginTarget(PluginRunTarget::Studio);
	};

	// A toolbar a plugin asked for, and the buttons on it.
	//
	// **Flat ids rather than objects, because the seam carries values.** A
	// `HostValue` has no userdata tag to hang a toolbar on, so a plugin holds
	// the number it was given and passes it back - which is the same shape every
	// immediate-mode API in this editor already has.
	//
	// @since v0.12
	struct PluginButton {
		// What the plugin called it, and what the button says.
		std::string Name;

		// The line under the cursor when somebody hovers it.
		std::string Tooltip;

		// What to call when it is pressed, in the plugin's own VM.
		engine::script::HostCallback OnClick;

		// Native C++ plugins use value arguments too, keeping toolbar dispatch
		// independent of either scripting VM.
		std::function<void(engine::script::HostArguments)> NativeOnClick;

		// Whether it draws as held. A plugin sets this to show a mode.
		bool Active = false;

		// Stable within its toolbar. Generated from creation order when the
		// plugin does not provide one.
		std::string Id;

		// What this item draws. `Builtin` is reserved for Default Studio.
		PluginControlKind Kind = PluginControlKind::Button;

		// Dropdown choices and the selected zero-based row.
		//@{
		std::vector<std::string> Options;
		size_t Selected = 0;
		//@}

		// Toggle and dropdown changes use this callback. A button uses `OnClick`.
		engine::script::HostCallback OnChanged;
		std::function<void(engine::script::HostArguments)> NativeOnChanged;

		// The plugin's defaults. A person's toolbar layout may override both.
		//@{
		bool Visible = true;
		float Width = 92.0f;
		//@}

		// Native control group when `Kind` is `Builtin`.
		BuiltinStudioTool Builtin = BuiltinStudioTool::None;

		// Stable grid cell names. Empty means automatic placement in declaration
		// order, which preserves every pre-grid plugin.
		//@{
		std::string Row;
		std::string Column;
		//@}
	};

	// One named row or column in a plugin toolbar grid.
	// @since v0.20
	struct PluginToolbarTrack {
		// Stable row or column identity.
		std::string Id;

		// Optional control width for a declared column. Zero keeps each control's
		// own width. Rows do not use this field.
		float Width = 0.0f;
	};

	// @since v0.12
	struct PluginToolbar {
		// The section's title.
		std::string Name;

		// Its buttons, in the order they were created.
		std::vector<PluginButton> Buttons;

		// Stable within the plugin. Generated from creation order when omitted.
		std::string Id;

		// Whether this tab is offered by default.
		bool Visible = true;

		// Pinned toolbars share the permanent first row. Tabbed toolbars appear in
		// the selectable ribbon below it.
		PluginToolbarPlacement Placement = PluginToolbarPlacement::Tabbed;

		// Stable grid declarations. Empty vectors produce an implicit row and one
		// automatic column per control.
		//@{
		std::vector<PluginToolbarTrack> Rows;
		std::vector<PluginToolbarTrack> Columns;
		//@}
	};

	// A docked panel a plugin asked for.
	//
	// The dock is ImGui-owned, while its content may use either plugin immediate
	// calls or the engine's retained GUI tree. `Gui` is the collector at that
	// seam; the compiled list and router stay here because they are host state,
	// not replicated world facts.
	//
	// @since v0.12
	struct PluginWidget {
		// The visible window title. Its ImGui identity also includes the plugin
		// and widget ids, so two plugins may use the same title.
		std::string Title;

		// Whether the window is open. A plugin may set it and a person may close
		// it, and both write here.
		bool Open = false;

		// What to call while it is open, in the plugin's own VM.
		engine::script::HostCallback Render;

		// Immediate-mode render callback for a native C++ plugin widget.
		std::function<void()> NativeRender;

		// Native dispatch for widgets contributed by Default Studio. Installed
		// plugins leave this as `None` and render through their VM callback.
		BuiltinStudioPanel BuiltinPanel = BuiltinStudioPanel::None;

		// The last open state shared with the native panel flag. This resolves
		// changes from either side, including toolbar actions and window closes,
		// without making two independent owners of the setting.
		bool SynchronizedOpen = false;

		// The retained content root created with this dock widget, plus the host
		// caches that turn it into pixels and pointer events.
		//@{
		engine::ecs::Entity Gui;
		engine::gui::Compiled GuiList;
		engine::gui::Router GuiRouter;
		//@}

		// What the plugin coloured it, if anything. See `SetWidgetColour`.
		//
		// **The plugin's, not the person's.** Every other panel in the editor
		// takes its colours from the settings page, because they are the
		// editor's panels and the person owns them; this one was created by a
		// script and the script owns it. A settings page that listed a plugin's
		// widgets would be a page whose rows appear and vanish as plugins load,
		// and whose entries outlive the plugin that made them.
		//
		// Resolved over the editor's theme rather than instead of it, so a
		// widget that sets one colour keeps every other colour in step with the
		// palette around it - see `engine::ui::ScopedColours`.
		//
		// @since v0.13
		// Defaulted rather than left to the aggregate, so a `PluginWidget`
		// built by naming the members before it does not warn under the `ci`
		// preset. An empty override set is "take the editor's theme", which is
		// what those sites mean.
		engine::ui::ThemeColours Colours = {};

		// Stable within the plugin and the first-use docking request.
		//@{
		std::string Id;
		PluginDock Dock = PluginDock::Floating;
		//@}

		// Window constraints in scaled pixels. A zero maximum means unbounded.
		//@{
		float MinimumWidth = 160.0f;
		float MinimumHeight = 100.0f;
		float MaximumWidth = 0.0f;
		float MaximumHeight = 0.0f;
		//@}
	};

	// Presentation shared by native and script plugins. Runtime ownership stays
	// in the two distinct loaded records below.
	//
	// @since v0.21
	struct PluginPresentation {
		// Its folder, which is also its identity: two plugins may call
		// themselves the same thing and they are still two plugins.
		std::filesystem::path Root;

		// What its manifest said.
		PluginManifest Manifest;

		// Whether it is running. False for one that was switched off, and for
		// one that failed.
		bool Running = false;

		// Why it is not, in the words somebody can act on. Empty when it is.
		std::string Error;

		// Whether discovery accepted the manifest and identity. Source reloads may
		// retry runtime failures, but only a manifest rescan may clear this gate.
		bool DefinitionValid = true;

		// What it asked the editor for.
		//@{
		std::vector<PluginToolbar> Toolbars;
		std::vector<PluginWidget> Widgets;
		//@}

		// Default Studio uses compatibility keys for toolbar preferences written
		// before each built-in control gained its own stable id.
		bool Builtin = false;

		// Native presentations are owned by `LoadedCppPlugin`; the rest by a
		// `LoadedPlugin` script instance.
		bool Native = false;
	};

	class PluginBindingRegistry;

	// One script plugin instance. A definition may have one instance per
	// matching Studio, playtest-server, or playtest-client world.
	//
	// @since v0.21
	struct LoadedPlugin : PluginPresentation {
		// Its own VM, or null when it could not be started.
		std::shared_ptr<engine::script::Runtime> Vm;

		// How many times its heartbeat has raised. A plugin that throws every
		// frame is switched off rather than logged sixty times a second.
		size_t Faults = 0;

		// The context this instance belongs to.
		//@{
		PluginRunTarget Target = PluginRunTarget::Studio;
		engine::world::WorldId World;
		PluginBindingRegistry *Bindings = nullptr;
		engine::script::Language Language = engine::script::Language::Luau;
		//@}

		// What it asked to hear about from `ChangeHistoryService`.
		//
		// **One slot per event rather than a list**, because a list would need
		// a disconnect to go with it and the seam has no handle to disconnect
		// by. Registering twice replaces, which is the behaviour a plugin that
		// re-registers on reload actually wants.
		//
		// @since v0.13
		//@{
		engine::script::HostCallback OnUndo;
		engine::script::HostCallback OnRedo;
		engine::script::HostCallback OnRecordingStarted;
		engine::script::HostCallback OnRecordingFinished;
		//@}

		// Its half of the seam, kept alive as long as its runtime is.
		//
		// **A `unique_ptr` because `HostSurface` is not copyable and the plugin
		// list is**, and because the runtime holds a raw pointer to it - so it
		// has to sit still while the vector it lives beside grows.
		std::unique_ptr<engine::script::HostSurface> Surface;
	};

	// Script languages a dynamic native binding is visible to.
	// @since v0.21
	enum class PluginBindingLanguage : uint8_t {
		Luau = 1u << 0,
		JavaScript = 1u << 1,
		Both = (1u << 0) | (1u << 1),
	};

	using PluginBindingLanguages = uint8_t;
	using PluginBindingFunction =
		std::function<bool(engine::script::HostArguments, engine::script::HostValue &, std::string &)>;

	// A move-only owner for dynamic script bindings. Destroying or closing it
	// removes every binding installed through it in one operation.
	// @since v0.21
	class PluginBindingScope {
	  public:
		PluginBindingScope() = default;
		~PluginBindingScope();

		PluginBindingScope(const PluginBindingScope &) = delete;
		PluginBindingScope &operator=(const PluginBindingScope &) = delete;
		PluginBindingScope(PluginBindingScope &&other) noexcept;
		PluginBindingScope &operator=(PluginBindingScope &&other) noexcept;

		[[nodiscard]] bool
		Add(std::string name,
			PluginBindingFunction function,
			std::string &error,
			PluginBindingLanguages languages =
				static_cast<PluginBindingLanguages>(PluginBindingLanguage::Luau));
		void Close();
		bool IsOpen() const;

	  private:
		friend class PluginBindingRegistry;
		struct State;
		PluginBindingScope(std::shared_ptr<State> state, uint64_t owner);

		std::shared_ptr<State> Shared;
		uint64_t Owner = 0;
	};

	// The dynamic host-call table for one world and one run target.
	// @since v0.21
	class PluginBindingRegistry {
	  public:
		explicit PluginBindingRegistry(PluginRunTarget target = PluginRunTarget::Studio);

		PluginBindingRegistry(const PluginBindingRegistry &) = delete;
		PluginBindingRegistry &operator=(const PluginBindingRegistry &) = delete;
		PluginBindingRegistry(PluginBindingRegistry &&) = delete;
		PluginBindingRegistry &operator=(PluginBindingRegistry &&) = delete;

		PluginBindingScope OpenScope();
		std::vector<std::string> Names(engine::script::Language language) const;
		bool Call(
			engine::script::Language language,
			std::string_view name,
			engine::script::HostArguments arguments,
			engine::script::HostValue &result,
			std::string &failure
		) const;
		void OnChanged(std::function<void()> changed);
		uint64_t Revision() const;

	  private:
		std::shared_ptr<PluginBindingScope::State> Shared;
	};

	// The data and callbacks that make one native C++ plugin.
	//
	// A custom library registers this record. Studio copies it into every
	// execution context selected by `Manifest.Runs`.
	// @since v0.21
	struct CppPluginContext;
	using CppPluginOpen = std::function<bool(CppPluginContext &, std::string &)>;
	using CppPluginClose = std::function<void(CppPluginContext &)>;
	using CppPluginHeartbeat = std::function<bool(CppPluginContext &, float, std::string &)>;

	struct CppPluginDefinition {
		PluginManifest Manifest;
		CppPluginOpen Open;
		CppPluginClose Close;
		CppPluginHeartbeat Heartbeat;
	};

	struct CppPluginContext {
		Editor *Owner = nullptr;
		engine::ecs::Store *WorldStore = nullptr;
		engine::world::WorldId World;
		PluginRunTarget Target = PluginRunTarget::Studio;
		PluginPresentation *Presentation = nullptr;
		PluginBindingScope *Bindings = nullptr;
	};

	struct LoadedCppPlugin : PluginPresentation {
		CppPluginDefinition Definition;
		PluginBindingScope Bindings;
		CppPluginContext Context;
		size_t Faults = 0;
	};

	// All native and script plugin instances attached to one Studio-owned world
	// role. Kept by pointer because binding scopes retain its registry address.
	// @since v0.21
	struct PluginRuntimeSet {
		PluginRuntimeSet(PluginRunTarget target, engine::world::WorldId world);
		~PluginRuntimeSet();

		engine::world::WorldId World;
		PluginRunTarget Target = PluginRunTarget::Studio;
		PluginBindingRegistry Bindings;
		std::vector<LoadedCppPlugin> Cpp;
		std::vector<LoadedPlugin> Scripts;
	};

	// A native definition registration. Keeping this handle keeps the
	// definition available; destroying it removes the definition.
	// @since v0.21
	class CppPluginRegistration {
	  public:
		CppPluginRegistration() = default;
		~CppPluginRegistration();

		CppPluginRegistration(const CppPluginRegistration &) = delete;
		CppPluginRegistration &operator=(const CppPluginRegistration &) = delete;
		CppPluginRegistration(CppPluginRegistration &&other) noexcept;
		CppPluginRegistration &operator=(CppPluginRegistration &&other) noexcept;

		void Close();
		bool IsOpen() const;

	  private:
		friend CppPluginRegistration RegisterCppPlugin(CppPluginDefinition, std::string &);
		explicit CppPluginRegistration(uint64_t id) : Id(id) {}
		uint64_t Id = 0;
	};

	[[nodiscard]] CppPluginRegistration RegisterCppPlugin(CppPluginDefinition definition, std::string &error);
	std::vector<CppPluginDefinition> RegisteredCppPlugins();
	uint64_t CppPluginRegistryRevision();

	// The allowed width range of one script-created toolbar control.
	// @since v0.20
	//@{
	inline constexpr float PLUGIN_TOOL_MINIMUM_WIDTH = 40.0f;
	inline constexpr float PLUGIN_TOOL_MAXIMUM_WIDTH = 320.0f;
	//@}

	// A custom toolbar tab saved by the toolbar editor.
	// @since v0.20
	struct ToolbarTabPreference {
		// Stable identity, presentation, placement, and ordering for the tab.
		//@{
		std::string Id;
		std::string Name;
		bool Visible = true;
		bool UserCreated = true;
		PluginToolbarPlacement Placement = PluginToolbarPlacement::Tabbed;
		size_t Order = 0;
		//@}
	};

	// A person's override for one plugin-owned toolbar item.
	// @since v0.20
	struct ToolbarItemPreference {
		// Stable item identity and the tab receiving it.
		//@{
		std::string Key;
		std::string Tab;
		//@}

		// Visibility, size, grid placement, and ordering overrides.
		//@{
		bool Visible = true;
		float Width = 92.0f;
		std::string Row;
		std::string Column;
		size_t Order = 0;
		//@}
	};

	// Persistent toolbar customization. Plugin declarations remain the defaults
	// and this sparse list rides over them.
	// @since v0.20
	struct ToolbarPreferences {
		// Sparse tab and item overrides owned by the user.
		//@{
		std::vector<ToolbarTabPreference> Tabs;
		std::vector<ToolbarItemPreference> Items;
		//@}
	};

	// One item in the toolbar composed for this frame.
	// @since v0.20
	struct ToolbarItemLocation {
		// Source indices, stable key, width, and composed order for one item.
		//@{
		size_t Plugin = 0;
		size_t Toolbar = 0;
		size_t Item = 0;
		std::string Key;
		float Width = 92.0f;
		size_t Order = 0;
		std::string ControlLabel;
		//@}
	};

	// One grid cell. Multiple items are retained in declaration order if a
	// plugin deliberately assigns them to the same cell.
	// @since v0.20
	struct ToolbarCellView {
		// The named column and the controls assigned to it.
		//@{
		std::string Column;
		std::vector<ToolbarItemLocation> Items;
		//@}
	};

	// One named row in a composed toolbar grid.
	// @since v0.20
	struct ToolbarRowView {
		// The named row and its composed cells.
		//@{
		std::string Id;
		std::vector<ToolbarCellView> Cells;
		//@}
	};

	// One visible tab and its visible controls.
	// @since v0.20
	struct ToolbarTabView {
		// Identity, presentation, contents, and ownership of the composed tab.
		//@{
		std::string Id;
		std::string Name;
		std::vector<ToolbarRowView> Rows;
		bool UserCreated = false;
		std::string Label;
		std::string Context;
		//@}
	};

	// The cached toolbar projection drawn by the editor.
	// @since v0.20
	struct ToolbarLayoutView {
		// Permanent rows and selectable tabs in the composed layout.
		//@{
		std::vector<ToolbarRowView> PinnedRows;
		std::vector<ToolbarTabView> Tabs;
		size_t VisualRows = 1;
		//@}
	};

	// Pure toolbar model helpers used by the editor and its suite.
	// @since v0.20
	//@{
	float ClampPluginToolWidth(float width);
	std::string PluginIdentity(const PluginPresentation &plugin);

	// Builds the visible widget title and its stable ImGui identity.
	//
	// @return A label safe to use both with `Begin` and `DockBuilderDockWindow`.
	std::string PluginWidgetLabel(const PluginPresentation &plugin, const PluginWidget &widget);

	std::string
	PluginToolbarKey(const PluginPresentation &plugin, const PluginToolbar &toolbar, size_t index);
	std::string PluginToolKey(
		const PluginPresentation &plugin,
		const PluginToolbar &toolbar,
		size_t toolbarIndex,
		const PluginButton &button,
		size_t itemIndex
	);
	ToolbarLayoutView
	ComposeToolbar(const std::vector<LoadedPlugin> &plugins, const ToolbarPreferences &preferences);
	ToolbarLayoutView
	ComposeToolbar(const std::vector<PluginPresentation *> &plugins, const ToolbarPreferences &preferences);
	bool
	LoadToolbarPreferences(const std::filesystem::path &path, ToolbarPreferences &out, std::string &error);
	bool SaveToolbarPreferences(
		const std::filesystem::path &path, const ToolbarPreferences &preferences, std::string &error
	);
	//@}

	// Creates the engine-owned native plugin that contributes Studio's standard
	// ribbon and the converted native panel adapters.
	// @since v0.21
	CppPluginDefinition MakeDefaultStudioPlugin();

	// How many times a plugin may raise before it is switched off.
	//
	// **Small, because the failure is per frame.** A plugin whose heartbeat
	// throws does it again next frame and every frame after; three is enough to
	// tell a transient from a broken one and few enough that the log stays
	// readable.
	inline constexpr size_t PLUGIN_FAULT_LIMIT = 3;

	// The editor's half of the seam, for one plugin.
	//
	// **One per plugin rather than one shared**, because every call has to know
	// which plugin made it: a toolbar belongs to whoever created it, and a
	// shared surface would need the caller's identity on every call - which the
	// seam cannot supply and a plugin could forge.
	//
	// @param editor The editor answering.
	// @param plugin The plugin asking. Must outlive the surface.
	// @return The surface, for `Runtime::SetHost`.
	// @since v0.12
	std::unique_ptr<engine::script::HostSurface>
	MakePluginSurface(Editor &editor, LoadedPlugin &plugin, engine::ecs::Store &store);

	// Opens or closes the gate on the widget calls.
	//
	// **Set by the editor around a render invoke**, so "am I drawing" is a fact
	// about where the call came from rather than a promise the plugin makes. A
	// `plugin.Label` from a heartbeat would otherwise draw into whatever window
	// the editor happened to be building.
	//
	// @param surface The plugin's surface.
	// @param drawing Whether a render callback is on the stack.
	// @since v0.12
	void SetPluginDrawing(engine::script::HostSurface &surface, bool drawing);

	// Where plugins are looked for.
	//
	// `ConfigRoot() / "plugins"`, so it moves with the rest of the studio's
	// configuration and a test can point it somewhere else in one call.
	//
	// @return The directory, which may not exist.
	// @since v0.12
	std::filesystem::path PluginRoot();

	// Parses a `plugin.json`.
	//
	// @param json  The file's contents.
	// @param out   Filled on success.
	// @param error Filled on failure.
	// @return `false` when the document is not a plugin manifest.
	// @since v0.12
	bool ParsePluginManifest(std::string_view json, PluginManifest &out, std::string &error);

	// Every plugin folder under `PluginRoot`, in name order.
	//
	// **Sorted, because a directory walk is not ordered.** Plugins run in this
	// order and one may build on what another left in the world, so an order
	// that changed between sessions would be a scene that came up differently
	// depending on the filesystem.
	//
	// A folder with no manifest is skipped in silence - it is somebody's notes,
	// not a broken plugin. One with a manifest that does not parse is returned
	// with its `Error` set, because that one *is* broken and saying so is the
	// point.
	//
	// @param root Where to look.
	// @return One entry per plugin folder, ordered by folder name.
	// @since v0.12
	std::vector<LoadedPlugin> DiscoverPlugins(const std::filesystem::path &root);

	// Starts every discovered plugin against a world.
	//
	// **One `script::Runtime` each**, so a plugin cannot see another's globals
	// or spend another's step budget. A plugin whose entry script fails to run is
	// left with `Running` clear and its error kept; the rest still start.
	//
	// **The surface is installed before the entry script runs**, because a
	// plugin's top level is where it creates its toolbar - a host set afterwards
	// would be a global the chunk had already failed to find.
	//
	// @param plugins What `DiscoverPlugins` found, updated in place.
	// @param store   The world they run against.
	// @param surface Builds each plugin's host surface, or empty for a plugin
	//                that gets the world and no editor.
	// @since v0.12
	void StartPlugins(
		std::vector<LoadedPlugin> &plugins,
		engine::ecs::Store &store,
		const std::function<std::unique_ptr<engine::script::HostSurface>(LoadedPlugin &)> &surface = {},
		PluginRunTarget target = PluginRunTarget::Studio,
		engine::world::WorldId world = {},
		PluginBindingRegistry *bindings = nullptr
	);

	// Beats every running plugin once.
	//
	// **One frame's delta, and a plugin that raises is counted rather than
	// stopped.** Past `PLUGIN_FAULT_LIMIT` it is switched off with its last
	// error kept - a plugin throwing sixty times a second is a log nobody can
	// read and a frame nobody can profile.
	//
	// @param plugins What `StartPlugins` started.
	// @param delta   Seconds since the last beat.
	// @return How many plugins were beaten.
	// @since v0.12
	size_t BeatPlugins(std::vector<LoadedPlugin> &plugins, float delta);

	// Starts, beats, and closes native definitions in one execution context.
	// Bindings are closed after `Close` returns, so plugin cleanup may still use
	// functions it registered during `Open`.
	// @since v0.21
	//@{
	void StartCppPlugins(
		std::vector<LoadedCppPlugin> &plugins,
		const std::vector<CppPluginDefinition> &definitions,
		engine::ecs::Store &store,
		PluginBindingRegistry &bindings,
		PluginRunTarget target = PluginRunTarget::Studio,
		engine::world::WorldId world = {},
		Editor *owner = nullptr
	);
	size_t BeatCppPlugins(std::vector<LoadedCppPlugin> &plugins, float delta);
	void StopCppPlugins(std::vector<LoadedCppPlugin> &plugins);
	//@}
}
