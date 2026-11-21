# UI live tests

`UiLab.luau` is a manual client scene for exercising GUI behavior. Its source is
`mono.engine/examples/assets/scripts/UiLab.luau`, with headless
coverage in `mono.engine/examples/tests/UiLab.cpp`.

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
