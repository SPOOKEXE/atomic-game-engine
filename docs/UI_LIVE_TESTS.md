# UI live tests

`UiLab.luau` is a manual client scene for exercising GUI behavior. Its source is
[`UiLab.luau`](../mono.engine/examples/assets/scripts/UiLab.luau), with headless
coverage in [`UiLab.cpp`](../mono.engine/examples/tests/UiLab.cpp).

Start the scene with `just ui-lab`. Pass client options after the recipe, for
example `just ui-lab --width 1280 --height 720`. The live input check is
`just ui-live-check`; it requires an X11 `DISPLAY`, `xdotool`, `xwininfo`,
`xrandr`, `xprop`, `wmctrl`, and Python Pillow. It refuses to run when a required
dependency is missing and writes its report under
`.cache/build/<preset>/ui-live-check/`.

The runner uses native desktop input and the client's existing control socket.
It reads resolved widget positions, checks script results and actual created
instances, and retains screenshots and frame profiles. A missing prerequisite
or failed check produces a failed report and a nonzero exit code.

## Manual exercises

- Click each button and image control. Check hover and press feedback, then
  press and drag outside the control before releasing to check cancellation.
- Focus the text box, enter UTF-8 text, edit it, and press Return to submit.
- Try scales `0.75`, `1`, and `1.5`. Check that a sample hit target follows
  the rendered control, the layout keeps its aspect ratio, and scrolling clips
  content at the viewport edge.
- Drag the draggable control, create items up to the limit of eight, destroy
  them, toggle visibility, and use reset to restore the initial state.

## Headless checks

Run the focused suite with `just ui-test`. It covers UI lab layout and hit
targets, button and hover routing including cancelled presses, and text
submission through focused keyboard input. This recipe always runs the current
`UiLab` suite, including after a script-only edit that the test cache cannot
detect. The native `just ui-live-check` recipe exercises the running client
with desktop input and produces a report for review.

## Verified on 2026-10-12

The dev Vulkan client passed all 22 live checks at 360x640, 640x360, 960x540,
1280x720 and 1920x1080. Manual desktop checks also exercised image clicks,
portrait scaling, child creation and reset. Visual review caught a caption
overlap at 150%; the corrected spacing has a resolved-bounds assertion.

| Check | Result |
|---|---|
| `just ui-test` | 6 cases, 207 assertions passed |
| `.cache/build/dev/tests/test_input` | 16 cases, 3,308 assertions passed |
| `.cache/build/dev/tests/test_gui` | 326 cases, 5,718 assertions passed |
| `.cache/build/dev/tests/test_examples` | 95 cases, 353,765 assertions passed |
| `just ui-live-check` | 22 native checks passed |
| Strict CI client/input/GUI/examples build | Passed; input, GUI and UI lab suites passed |
| Server build | Passed; GUI and UI lab suites passed headlessly |
| `just docs-check` | Every public entity documented |
| `just test-architecture` | Passed |
| `just source-check` | Passed; existing public-header advisories remain |

The checks exposed and repaired lost input between presentations, duplicate
control taps in one event pump, and an outer scrollbar taking wheel input
intended for its nested list. The aspect test was also run against an
intentionally broken constraint and correctly failed before the scene was
restored.

Current captures and reports are generated under
`.cache/build/<preset>/ui-live-check/`; manual captures from this session are
under `.cache/build/dev/ui-manual/`. Frame profiles use the dev build and include
profiling overhead. The readable snapshot is `frame-profile.txt`; the live span
sample is `profile-live.json`.
