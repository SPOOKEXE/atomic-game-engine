# scriptcheck

Type-checks Luau against the engine declarations (`engine.d.luau`), in strict
mode, without running anything. One checker serves every caller: the command
line, `just typecheck`, the `--serve` language server, and Studio's script tab
Check button (through the control surface's `script_check`, which runs the
staged program because Studio does not link `Luau.Analysis`).

Source: `mono.tools/scriptcheck/` (library `Tool::scriptcheck`, program
`<build>/tools/scriptcheck`).

`scriptcheck --describe` adds `"pluginBackend"` to the usual description:
`"native"` when this CPU supports the Luau native backend, so a Studio
plugin's `--!native` modules compile to machine code, else `"interpreter"`.

## Check a folder

```
just scriptcheck ~/Documents/GitHub/my-plugin
.cache/build/dev/tools/scriptcheck ~/Documents/GitHub/my-plugin --format json
```

| Argument | Checks |
|---|---|
| a folder with `plugin.json` somewhere below it | every `.luau`/`.lua` inside each plugin folder. Files outside a plugin (Lune tests beside it) are skipped. |
| a folder with no `plugin.json` | every `.luau`/`.lua` in it, as one project |
| a file | that file and every module it requires |

Hidden folders and `node_modules` are skipped. Exit code: `0` clean, `1`
diagnostics, `2` bad arguments or no declarations.

Each diagnostic names the file it is in, so an error in a required module is
reported at that module's path and line, once even when several checked files
reach it. Text output is `path(line,column): Code: message`. `--format json` prints an
array of `{file, line, column, endLine, endColumn, code, message}`. Codes:

| Code | Meaning |
|---|---|
| `TypeError` | the type checker's own error |
| `SyntaxError` | the file does not parse |
| `UnknownRequire` | a relative require names no file |
| `RequireEscape` | a require leaves the plugin or project folder; it is not read |

## Requires

| Form | Resolves to |
|---|---|
| `require("./x")`, `"../x"`, `"@self/x"` | Luau's require-by-string rule, as game scripts and Studio's plugin host resolve it |
| `require(script.Parent.X)`, `script:FindFirstChild("X")`, `:WaitForChild("X")`, through locals | the folder-to-instance rule the plugin host and `MountModuleTree` use: `init.luau` is its folder, `X.server.luau` is a Script named `X` |
| anything else (`game.ReplicatedStorage.X`, an alias like `@lune/fs`) | `any` |

`script` is typed per file from the plugin or project folder tree, by the same
rule: a folder is a `Folder`, a module a `ModuleScript`, `X.server.luau` a
`Script` and `X.client.luau` a `LocalScript`, each with its children as
properties. So `script.Parent.Sibling` is typed as the sibling, and a name with
no file or folder behind it is still `Key 'X' not found`. Above the plugin or
project folder, and for a loose file's `Parent`, the tree is unknown and the
type is `any`.

`--allow-missing-requires` types a relative require with no file as `any`.
`script_check` passes it for drafts, which sit alone in a scratch file.

## Finding the declarations

In order: `--definitions PATH`, `$ATOMIC_LUAU_DEFINITIONS`, `engine.d.luau`
beside the program (the build stages a copy into `tools/`), then
`mono.engine/script/bindings/engine.d.luau` in the working directory or any
parent of it or of the program. A copied `tools/` folder works with no flag.

## Language server

```
scriptcheck --serve
```

LSP over stdio, full-text sync. The workspace root from `initialize` sets the
require roots the same way a folder argument does. Handled: `didOpen`,
`didChange`, `didSave`, `didClose` (each checks every open document and
publishes per file URI, including required modules that are not open, and sends
an empty list to a file that has become clean), `hover` (the type at the cursor), `completion`, `shutdown`, `exit`.

Any editor with a generic LSP client can start it for `luau` files. Neovim:

```lua
vim.lsp.start({
	name = "scriptcheck",
	cmd = { "/path/to/tools/scriptcheck", "--serve" },
	root_dir = vim.fs.root(0, "plugin.json"),
})
```

Helix (`languages.toml`):

```toml
[language-server.scriptcheck]
command = "/path/to/tools/scriptcheck"
args = ["--serve"]

[[language]]
name = "luau"
language-servers = ["scriptcheck"]
```

## VS Code and luau-lsp

VS Code users run luau-lsp. Give it the engine vocabulary for a folder outside
the checkout:

```
scriptcheck --emit-lsp-config ~/Documents/GitHub/my-plugin
```

This writes `.atomic/engine.d.luau` and `.atomic/luau-lsp.json` into the folder
and changes nothing else. Copy the keys of `.atomic/luau-lsp.json` into
`.vscode/settings.json`, or run `luau-lsp analyze --settings=.atomic/luau-lsp.json`.
Re-run it after `just bindings` changes the declarations. Upstream luau-lsp
does not resolve `Enum.NormalId` in a type position; the build from `just
luau-lsp` carries the patch that does, and `Enum_NormalId` works in both.

## From a plugin repository

A plugin kept in its own repository can gate on the checker in its own test
script or CI:

```
/path/to/atomic-game-engine/.cache/build/dev/tools/scriptcheck plugin --format json
```

or, with a copied `tools/` folder on `PATH`, `scriptcheck plugin`.
