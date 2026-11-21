# Studio plugins

A Studio plugin is a folder under `ConfigRoot()/plugins/` with a `plugin.json`.
Examples are in `mono.studio/plugins/`. Copy one into the plugins folder to try it.

```
plugins/
  dock-panel/
    plugin.json          { "name": "Dock Panel", "main": "main.luau" }
    main.luau
  multi-file/
    plugin.json
    main.luau            require("./core/Counter")
    Status.server.luau   a second script, runs after main
    core/Counter.luau    require("../shared/Format")
    shared/Format.luau
```

## Manifest

| Key | Default | Meaning |
|---|---|---|
| `name` | required | The name in lists, and the `Plugin` instance's `Name`. |
| `id` | folder name | Stable identity for toolbar layout, enabled state and settings. |
| `main` | `main.luau` | The entry script, relative to the folder. A path that leaves the folder is refused. |
| `enabled` | `true` | Whether it runs. The Plugins menu overrides this per person. |
| `runs` | `studio` | `studio`, `playtest-server`, `playtest-client`, or `all`. |
| `description`, `version`, `author` | empty | Shown in the manager. |

## The instance tree

When a plugin loads, Studio mirrors its folder as instances:

```
PluginGuiService            not archivable: never saved, never in a Play snapshot
  Dock Panel  (Plugin)      the `plugin` global
    main      (Script)      the manifest's main is always a Script
    core      (Folder)
      Counter (ModuleScript)
  DockPanel   (DockWidgetPluginGui)
```

| On disk | Instance |
|---|---|
| folder | `Folder`, or the script its `init.luau` describes |
| `X.server.luau` | `Script` |
| `X.client.luau` | `LocalScript` |
| `X.luau`, `X.lua` | `ModuleScript` |
| `X.model.json` | the Rojo model it describes, named `X` |
| `X.rbxm`, `X.rbxmx` | the one instance in that Roblox model, named `X` |
| `plugin.json`, dot files, other files | not mirrored |

All three plugin shapes become this tree: one script, a folder of instances
that includes scripts, or many scripts under one parent. `main` runs first,
then every other `Script` and `LocalScript` in tree order. All of them run in
one runtime for the plugin, with one module cache and one `plugin`.

World scripts never run anything under `PluginGuiService`. A game script that
asks for `PluginGuiService` is refused: it needs the `plugin-host` capability,
which only a plugin runtime has. `plugin` is nil in any script that is not a
plugin.

## require

`require` takes a `ModuleScript` or a path. A path follows Luau's
require-by-string rule over the instance tree, which is the same rule as over
the files:

| Path | Starts at |
|---|---|
| `./x`, `../x` | the requiring script's parent folder |
| `@self/x` | the requiring script itself (for `init.luau`, its folder) |

`.luau` and `.lua` on the last part are optional. A path that leaves the
`Plugin` instance is refused with a message naming the plugin. Each module
runs once per plugin runtime; `require("./a")` and `require(script.Parent.a)`
return the same value. A cycle raises with both names:
`require cycle: 'loop/y.luau' requires 'loop/x.luau'`.

Chunk names are the path relative to the plugin folder, so errors read
`core/Counter.luau:12`.

`scriptcheck` resolves the same paths, so `--!strict` types flow between a
plugin's modules:

```
.cache/build/dev/tools/scriptcheck plugins/multi-file/main.luau
```

## Hot reload

Studio polls each plugin folder. A change to any file under it, including a
required module, reloads that plugin. A change to `plugin.json` rescans every
plugin.

## The plugin object

`plugin` is the plugin's `Plugin` instance. These members follow Roblox:

| Member | Notes |
|---|---|
| `plugin:CreateToolbar(name)` | Returns a `PluginToolbar`. |
| `toolbar:CreateButton(id, tooltip, icon, text?)` | Returns a `PluginToolbarButton`. `Click`, `SetActive`, `Enabled`, `Icon`, `ClickableWhenViewportHidden`. |
| `plugin:CreatePluginAction(id, text, statusTip, icon?, allowBinding?)` | Listed in the Plugins menu. Choosing it fires `Triggered`. |
| `plugin:CreateDockWidgetPluginGui(id, info)` | `info` is `DockWidgetPluginGuiInfo.new(state, enabled, override, w, h, minW, minH)`. The widget has `Title`, `Enabled` and `BindToClose`. `Top` docks in the centre. |
| `plugin:GetSetting(key)`, `plugin:SetSetting(key, value)` | Persisted per plugin id in `ConfigRoot()/plugin-settings/<id>.json`. Booleans, numbers, strings and tables of them. |
| `plugin:Activate(exclusiveMouse)`, `:Deactivate()`, `:IsActivated()`, `:IsActivatedWithExclusiveMouse()` | One plugin is active at a time. Activating another fires `Deactivation`. An exclusive mouse stops viewport selection. |
| `plugin:GetMouse()` | A `PluginMouse`: `X`, `Y`, `ViewSizeX`, `ViewSizeY`, `Hit`, `Origin`, `Target`, and `Button1Down`, `Button1Up`, `Move` while the plugin is active. |
| `plugin:GetSelectedRibbonTool()`, `plugin:SelectRibbonTool(tool)` | Studio's Select, Move, Rotate, Scale and None tools. |
| `plugin:OpenScript(script, lineNumber?)` | Opens a script tab, with the caret at the start of `lineNumber` when given. |
| `plugin:CreatePluginMenu(id, title?, icon?)` | Returns a `PluginMenu`: `AddAction(action)`, `AddNewAction(id, text, icon?)`, `AddMenu(menu)`, `AddSeparator()`, `Clear()`, `Title`, `Icon`. |
| `menu:ShowAsync()` | Shows the menu at the mouse and waits. Returns the chosen `PluginAction` and fires its `Triggered`, or returns nil when dismissed. Call it where the script can yield, such as a connection. |
| `plugin.GridSize` | The viewport grid step. |
| `plugin.Deactivation`, `plugin.Unloading` | `Unloading` fires before a reload or a switch-off. |

`game:GetService("Selection")` is the selection service: `Get`, `Set`, `Add`,
`Remove`, and `SelectionChanged`, which fires on the plugin beat after the
selection changes.

The older flat calls still work on the same object with a dot:
`plugin.CreateToolbar(name)` returns a number, `plugin.CreateWidget(...)`,
`plugin.Label(...)` and the rest are unchanged. A colon call gets the Roblox
member and a dot call gets the flat call.

The object model is Luau only. A JavaScript plugin keeps the flat `plugin`
table and a single entry file.

## Switching plugins on and off

The ribbon's Plugins tab has a Manage button, and the main menu has a Plugins
menu. Both list every installed plugin with a toggle. Switching one off
reloads plugins: its toolbars, widgets and actions go, and every script under
its `Plugin` instance stops with its runtime. Switching it on loads it again.
The state is saved in `ConfigRoot()/plugins.json`.
