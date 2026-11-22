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
| `X.luau`, `X.lua`, `X.js` | `ModuleScript` (`.server`/`.client` as above) |
| `X.model.json` | the Rojo model it describes, named `X` |
| `X.rbxm`, `X.rbxmx` | the one instance in that Roblox model, named `X` |
| `plugin.json`, dot files, other files | not mirrored |

All three plugin shapes become this tree: one script, a folder of instances
that includes scripts, or many scripts under one parent. `main` runs first,
then every other `Script` and `LocalScript` in tree order. All of them run in
one runtime for the plugin, with one module cache and one `plugin`.

The tree is local to the process that runs the plugin. In a playtest client,
whose world is a replica of the server's, the tree is built in the predicted
entity range, the only range a replica owns. Game instances in that world stay
under the server's authority.

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
`Plugin` instance is refused with a message naming the plugin.

Game scripts (`Script`, `LocalScript`, `ModuleScript`, Luau and JavaScript,
server and client) take the same paths over the game's tree. There `..` above
a service reaches the game, where a name is a service:
`require("../../ReplicatedStorage/Config")` from a script in a folder in
`Workspace`. A path above the game, or into `PluginGuiService`, is refused with
`leaves the game`. Each module
runs once per plugin runtime; `require("./a")` and `require(script.Parent.a)`
return the same value. A cycle raises with both names:
`require cycle: 'loop/y.luau' requires 'loop/x.luau'`.

Chunk names are the path relative to the plugin folder, so errors read
`core/Counter.luau:12`.

`scriptcheck` resolves the same paths, so `--!strict` types flow between a
plugin's modules, and refuses a require that leaves the plugin folder:

```
just scriptcheck plugins/multi-file
```

[SCRIPTCHECK.md](SCRIPTCHECK.md) covers checking a plugin kept in its own
repository, JSON output and the editor server.

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

A JavaScript plugin (`"main": "main.js"`) gets the same object model with
dots: `plugin.CreateToolbar("x")`, `button.Click.Connect(f)`. JavaScript has
no colon, so where a flat call and a member share a name the member wins:
`plugin.CreateToolbar` returns a `PluginToolbar`, which the flat toolbar
calls also accept in place of an index. `menu.ShowAsync()` returns a promise.
`.js` files mirror like `.luau` ones, and `require` takes a `ModuleScript` or
a relative path; a module's value is what its file `return`s. Only scripts in
the main file's language run.

## Files and the clipboard

A plugin never names a path. A native dialog picks it, and the call waits for
the dialog: Luau yields, so call it where the script can yield, such as a
connection or `task.spawn`. JavaScript gets a promise. Each call is a `Plugin`
member (`plugin:SaveFile`) and a flat call (`plugin.SaveFile`) with the same
arguments. One dialog per plugin is open at a time.

| Call | Notes |
|---|---|
| `plugin:SaveFile(name, data, filters?)` | Suggests `name` and writes `data`, a string or a `buffer` (`Uint8Array` or `ArrayBuffer` in JavaScript). Returns the saved file name, or nil when cancelled. |
| `plugin:OpenFile(filters?, asText?)` | Returns `{ Name, Data }` for the chosen file, or nil when cancelled. `Data` is a `buffer` (`Uint8Array`), or a string when `asText` is true. |
| `plugin:SaveFolder(name, files)` | Asks for a parent folder and writes `files`, a table of relative paths to strings or buffers, into a new folder `name` inside it. Returns `name`, or nil when cancelled. |
| `plugin:SetClipboard(text)`, `plugin:GetClipboard()` | The system clipboard, as UTF-8 text. No dialog. |

`filters` is an array of `{ Name = "Images", Pattern = "png;jpg" }`. A pattern
lists extensions without dots, and `*` matches every file.

Limits:

- A file, or a folder's files in all, is at most 256 MiB.
- A `SaveFolder` path must stay inside the folder: no absolute paths and no
  `..` above it. `name` is one folder name with no separators.
- Clipboard text is at most 16 MiB.

```luau
button.Click:Connect(function()
	local saved = plugin:SaveFile("character.json", json, { { Name = "JSON", Pattern = "json" } })
	local opened = plugin:OpenFile({ { Pattern = "json" } }, true)
	if opened then print(opened.Name, #opened.Data) end
end)
```

## Text fields

`plugin.InputText(label, value, maxBytes?)` and
`plugin.InputTextMultiline(label, value, maxBytes?)` draw a text field inside a
widget's render function and return its text. A field holds at most
`maxBytes`, which defaults to and is capped at 64 KiB. A longer `value` is cut
on a UTF-8 character boundary.

## Switching plugins on and off

The ribbon's Plugins tab has a Manage button, and the main menu has a Plugins
menu. Both list every installed plugin with a toggle. Switching one off
reloads plugins: its toolbars, widgets and actions go, and every script under
its `Plugin` instance stops with its runtime. Switching it on loads it again.
The state is saved in `ConfigRoot()/plugins.json`.
