# launcher - module invariants

L13, `client` tier. The front door: a window that picks a mode, builds a
command line for it, and starts the staged program that mode names.

Configured only under `MONO_BUILD_CLIENT`, for `mono.studio`'s reason - it links
a renderer and Dear ImGui, and the `server` and `cdn` presets have no graphics
stack. It is absent there rather than stubbed.

## This program links none of the programs it starts

It starts the client, the server, the origin and the editor, and its link line
holds `core`, `parallel`, `render` and `ui`. That is the whole design, and the
first change anybody will want to make to it is the one that breaks it.

Linking them would be the largest tier escape in the repository, it would make
this unbuildable under two presets, and a fault in any of the four would take
the launcher down with it. It would also make the displayed command line a
fiction, because there would be no command line - and a launcher that cannot
show you exactly what it ran is a launcher whose bug reports say "it did not
work".

**So: no `Mono::client`, no `Mono::server`, no `Mono::cdn`, no `Mono::studio`,
and no `ALLOW_TIER_ESCAPE` on this target. Ever.** If a mode needs something out
of one of those, it needs a flag on that program, not an edge here.

## No flag tables live here

`Catalogue.cpp` names five modes, four programs and about thirty option names to
pin. It names no option's type, no option's description, and no setting at all -
those come from `core::Arguments::Describe`, read out of the program itself at
startup.

Between them the four programs declare around 150 options and 300 settings.
Transcribed here they would be 450 facts with two homes, and rule 2's failure
mode applies with a twist: a launcher missing a flag still starts the program,
so the stale copy is invisible until somebody needs the flag.

**A new option must appear in this launcher with no change to this module.** If
you find yourself adding a table of option names and descriptions, the change
belongs in the program that owns them.

The pinned list is the one exception, and it is a *reading order* rather than a
filter - every declared option is reachable from the form whether it is pinned
or not. A pinned name that the program no longer declares is silently nothing,
which is deliberate: this launcher must work beside an older staged tree.

## Three tabs, and only the first one is curated

**Common** is the mode's pinned block - what the run *is*, in five or six rows.
**All options** is every declared option, including the pinned ones, grouped by
the prefix their names already carry. **Engine settings** is `core::Flags`'
declared table, written as `--flag NAME=VALUE` because that is the source which
outranks a config file and the environment.

Both option tabs edit the same `FieldState`, so a row shown on each is still one
row and cannot disagree with itself. A tab called "all" that quietly omitted the
seven pinned ones would be the lie, which is why `GroupOptions` stopped taking a
`Mode` at v0.18.

## The decisions are in `Plan.cpp`, and the drawing is not

Which rows exist, how they group, what the search matches, what `+`, `-` and a
multi-folder pick do to the row list, and exactly what argv reaches the child are
pure functions of a `Mode` and a `Description`, and they have suites.
`Interface.cpp` is imgui calls over those answers and has none.

**A row loop re-reads `Options.size()` every step.** `+`, `-` and a confirmed
multi-folder pick all change the row count from inside the draw, so a loop
holding a cached size walks off the end on the frame a row is removed. That is
what `DrawRowsOf` is for.

That split is not tidiness. A form generated from another program's output is a
form nobody can read the source of and predict, so the part that decides has to
be the part that can be asserted about. **Do not move a decision into
`Interface.cpp`** - the moment "which options show" depends on a widget's state
rather than on a value, it stops being testable and starts being a screenshot.

**The line is the argument list.** An answer that is a pure function of a `Mode`,
a `Description` or a `Form` belongs in `Plan.hpp` with a case in
`tests/Plan.cpp`. What stays in `Interface.cpp` is what needs a live
`ImGuiStyle` or a live id stack to mean anything: `NameColumnWidth` and
`ActionsColumnWidth` measure text in the current font, `ForceHeaderState` and
`TabLabel` are about imgui's own state, and `DrawBrowseDialogs` exists for where
on the id stack a popup is opened. The pair either side of the line is
`IsBooleanSetting`, which says a declared kind is a two-state value and is in
`Plan.hpp`, and the `ImGui::Checkbox` that follows from it, which is not.

**Nothing checks this, so it is a convention** - rule 6's second option. The
build cannot tell a decision from a draw call, and the way it goes wrong is
drift rather than a bad commit: six answers had crossed the line by v0.19, none
of them wrong on screen and none of them tested. `WantsFile` and `WantsFolder`
read `PATH` and `DIR` off a declaration, `AnyBrowses` asked the same question a
third time with its own string literals, `CommonHits`, `AllHits` and
`SettingHits` counted what the tabs then filtered for a second time, and a
`Kind == "boolean"` decided a widget. All six are in `Plan.hpp` now, as
`BrowseShapeOf`, `AnyBrowses`, `MatchingOptions`, `MatchingSettings`,
`OptionHits`, `SettingHits` and `IsBooleanSetting`. The count on a tab and the
rows under it are one function since, which is the bug that pair could have had:
a tab reading `(3)` over two rows.

## A Browse button records, it does not open

`ImGui::OpenPopup` names a popup against the id stack **at the moment it is
called**. A row's Browse button is called from inside that row's `PushID`,
inside a table, inside the scrolling child - three levels below the
`BeginPopupModal` that has to match it. Opening from there produces an id
nothing ever begins: the button highlights, the tooltip appears, and no dialog
does.

That shipped once. The fix is `Launcher::BrowseRequested`: a button records
which dialog it wants, and `DrawBrowseDialogs` - the one place the modals live -
does the opening. **Any new dialog goes through the same seam.** An
`ImGui::OpenPopup` anywhere inside `DrawOption` is the bug coming back.

The same trap has a milder cousin: `SetNextItemOpen(value, ImGuiCond_Always)`
with a `value` that is false on most frames is a collapsing header that cannot
be opened, because the click lands and the next frame overrides it. See
`ForceHeaderState`.

## Programs are found by the staged layout, and there is no setting for it

`mono_add_program` puts every program at `<stage>/<name>/<name>`. This one is at
`<stage>/launcher/launcher`, so the client is `../client/client`. Right by
construction, in a build tree and in a shipped copy alike.

A configurable path would go stale the first time a build moved, and a launcher
pointed at last week's client is the one bug this program must not have: it
would start something, it would look like it worked, and the thing that ran
would be the wrong build. That is why every mode row shows its program's
version - two versions in one tree means a half-rebuilt tree, and that is worth
seeing before the run rather than after.

## A missing program is an answer, not a failure

The `server` preset stages no client; a tree built for one target has three
missing. Every mode whose program is absent is greyed out with the reason on
the row. `Descriptions::Failure` holds those strings and they name the path
that was looked at - a launcher reporting "no client" without saying where is
one nobody can debug from a screenshot.

## The child inherits the launcher's streams

`parallel::Process`'s documented behaviour, and the right one here: a server's
log belongs in the terminal the launcher was started from. This module reports
*that* a child ended and *how*; it does not capture or display what it said.

`parallel::Capture` is the other one, and it is only for `--describe` - a
question asked during startup of a program expected to print one object and
exit. Do not reach for it to run a mode.

## Stop is polite, and the launcher outlives the child

A supervised host or origin is asked to stop and given the chance to close its
sockets before anything kills it - that is the difference between a port that is
free on the next Start and one held in `TIME_WAIT`. Closing the launcher stops
the child first, for the same reason.

`HandOver` modes minimise rather than exit, because the launcher has to be alive
to notice the child ended and coming back to the form is the reason somebody
would use a launcher twice.

## What is deliberately not here

- **Saved profiles.** Forms live as long as the launcher window and no longer.
  Persisting them means a file format, a migration and a stale-profile failure
  mode. Every program already reads `--config PATH`, and that option is on the
  form like any other.
- **A log pane.** The child writes to the terminal. Capturing it to show it here
  means owning a pipe and a scrollback for the lifetime of a server.
- **A downloader or an updater.** This launches what is beside it. Nothing more.
- **An icon set.** `ui::Icons` draws a folder and a page with `ImDrawList`
  because no vendored font carries either and a browse button that does not say
  which dialog it opens is a button you have to press to find out. A third icon
  needs an argument of its own, not a precedent.
