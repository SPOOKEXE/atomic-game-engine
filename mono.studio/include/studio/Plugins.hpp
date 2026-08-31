#pragma once

// Scripts the editor runs, written by somebody who is not the editor.
//
// **A plugin is a script against the world, and that is the whole design.** The
// engine already has a scripting surface with a sandbox, a step budget, a memory
// ceiling and two languages - `script/Runtime.hpp` - and a plugin is that surface
// pointed at the world an author is editing rather than at one a game is running.
// Inventing a second scripting model for tools would be two sandboxes to keep
// safe and two vocabularies to learn.
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
// | panels | `plugin.CreateWidget`, `.SetWidgetRender`, `.SetWidgetOpen`,
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
// **Ids rather than objects, because the seam carries values.** A `HostValue`
// has no userdata to hang a toolbar on, so `CreateToolbar` answers a number and
// `CreateButton` takes it back - the one place this reads differently from
// Roblox's, and it is stated rather than smoothed over.
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
// ## One runtime each, and one failure each
//
// **Every plugin gets its own `script::Runtime`.** Two plugins sharing one would
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
#include <engine/script/Host.hpp>
#include <engine/script/Runtime.hpp>
#include <engine/ui/Theme.hpp>
#include <engine/world/Universe.hpp>

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace studio {

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
	// **Immediate mode, which is how the rest of this editor works.** The
	// contents are not a retained tree the plugin builds and the editor walks -
	// the editor calls `Render` while its window is open and the plugin issues
	// widget calls from inside it, exactly as `DrawExplorer` and every other panel
	// here does. A retained tree would be a second widget model beside ImGui,
	// and the engine already has one of those in `gui` for the *game's* UI.
	//
	// @since v0.12
	struct PluginWidget {
		// The window title, which is also how ImGui identifies it.
		std::string Title;

		// Whether the window is open. A plugin may set it and a person may close
		// it, and both write here.
		bool Open = false;

		// What to call while it is open, in the plugin's own VM.
		engine::script::HostCallback Render;

		// Native dispatch for widgets contributed by Default Studio. Installed
		// plugins leave this as `None` and render through their VM callback.
		BuiltinStudioPanel BuiltinPanel = BuiltinStudioPanel::None;

		// The last open state shared with the native panel flag. This resolves
		// changes from either side, including toolbar actions and window closes,
		// without making two independent owners of the setting.
		bool SynchronizedOpen = false;

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

	// One plugin, as the editor holds it.
	//
	// @since v0.12
	struct LoadedPlugin {
		// Its folder, which is also its identity: two plugins may call
		// themselves the same thing and they are still two plugins.
		std::filesystem::path Root;

		// What its manifest said.
		PluginManifest Manifest;

		// Its own VM, or null when it could not be started.
		std::shared_ptr<engine::script::Runtime> Vm;

		// Whether it is running. False for one that was switched off, and for
		// one that failed.
		bool Running = false;

		// Why it is not, in the words somebody can act on. Empty when it is.
		std::string Error;

		// Whether discovery accepted the manifest and identity. Source reloads may
		// retry runtime failures, but only a manifest rescan may clear this gate.
		bool DefinitionValid = true;

		// How many times its heartbeat has raised. A plugin that throws every
		// frame is switched off rather than logged sixty times a second.
		size_t Faults = 0;

		// What it asked the editor for.
		//@{
		std::vector<PluginToolbar> Toolbars;
		std::vector<PluginWidget> Widgets;
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

		// Built-in plugins have native controls and no script runtime.
		bool Builtin = false;
	};

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
		//@}
	};

	// The cached toolbar projection drawn by the editor.
	// @since v0.20
	struct ToolbarLayoutView {
		// Permanent rows and selectable tabs in the composed layout.
		//@{
		std::vector<ToolbarRowView> PinnedRows;
		std::vector<ToolbarTabView> Tabs;
		//@}
	};

	// Pure toolbar model helpers used by the editor and its suite.
	// @since v0.20
	//@{
	float ClampPluginToolWidth(float width);
	std::string PluginIdentity(const LoadedPlugin &plugin);
	std::string PluginToolbarKey(const LoadedPlugin &plugin, const PluginToolbar &toolbar, size_t index);
	std::string PluginToolKey(
		const LoadedPlugin &plugin,
		const PluginToolbar &toolbar,
		size_t toolbarIndex,
		const PluginButton &button,
		size_t itemIndex
	);
	ToolbarLayoutView
	ComposeToolbar(const std::vector<LoadedPlugin> &plugins, const ToolbarPreferences &preferences);
	bool
	LoadToolbarPreferences(const std::filesystem::path &path, ToolbarPreferences &out, std::string &error);
	bool SaveToolbarPreferences(
		const std::filesystem::path &path, const ToolbarPreferences &preferences, std::string &error
	);
	//@}

	// Creates the engine-owned plugin that contributes Studio's standard ribbon.
	// @since v0.20
	LoadedPlugin MakeDefaultStudioPlugin();

	// How many times a plugin may raise before it is switched off.
	//
	// **Small, because the failure is per frame.** A plugin whose heartbeat
	// throws does it again next frame and every frame after; three is enough to
	// tell a transient from a broken one and few enough that the log stays
	// readable.
	inline constexpr size_t PLUGIN_FAULT_LIMIT = 3;

	class Editor;

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
	std::unique_ptr<engine::script::HostSurface> MakePluginSurface(Editor &editor, LoadedPlugin &plugin);

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
		const std::function<std::unique_ptr<engine::script::HostSurface>(LoadedPlugin &)> &surface = {}
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
}
