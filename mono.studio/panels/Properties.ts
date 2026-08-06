// A property grid, authored in TypeScript, drawn by the engine's own UI.
//
// **This is the point of the whole 2D branch and it is deliberately the last
// step.** `mono.studio` keeps Dear ImGui until the engine's own tree can draw a
// property grid — because an editor half on each is two widget sets, and the
// rule against two ways to do one job applies hardest to the thing you look at
// all day. So this is not a replacement for the imgui panels; it is the proof
// that the tree can carry one, which is what has to be true before any of them
// move.
//
// **What it proves, specifically.** A panel is rows of label-and-value inside a
// scrolling container, aligned by a layout rather than by arithmetic, and
// responding to a click. Every one of those is a piece that had no caller until
// now:
//
//   - `UIListLayout` stacking rows, and `UIPadding` insetting them — the layout
//     pass, which until this had only the example scene exercising it;
//   - `ScrollingFrame`, so a grid longer than its panel is reachable;
//   - `.Activated` on a row, which is `gui::Router`'s events reaching a script
//     through `script::Signals` — the join this version added;
//   - `StarterGui` containment, because a `ScreenGui` parented anywhere else
//     draws nothing.
//
// **React is not here and does not have to be.** The roadmap leaves the
// reconciler open — `react-lua` or a hand-written one are both reasonable — and
// this file is what that decision would be made *against*: a reconciler's job
// is to produce this tree from a description, so the tree has to be known to
// work first. Building it imperatively is how you find out.

const StarterGui = game.GetService("StarterGui");

// The palette, in one place. Same rule as `ui::Theme`'s: a colour used twice is
// a colour that will be changed once.
const PAPER = Color3.fromRGB(232, 234, 238);
const INK = Color3.fromRGB(27, 42, 53);
const MUTED = Color3.fromRGB(120, 130, 145);
const PANEL = Color3.fromRGB(250, 250, 252);
const RULE = Color3.fromRGB(224, 228, 234);
const ACCENT = Color3.fromRGB(0, 122, 204);

const ROW_HEIGHT = 22;
const LABEL_WIDTH = 0.45;

// One property, as a panel shows it.
type Row = { name: string; value: string };

// What a real grid would read off `ecs::Classes`. Written out here because this
// file is proving the *drawing*, and a grid that also had to resolve a selection
// would fail for two reasons and name neither.
const ROWS: Row[] = [
	{ name: "Name", value: "Baseplate" },
	{ name: "ClassName", value: "Part" },
	{ name: "Anchored", value: "true" },
	{ name: "CanCollide", value: "true" },
	{ name: "Position", value: "0, -0.25, 0" },
	{ name: "Size", value: "60, 0.5, 60" },
	{ name: "Color", value: "120, 120, 125" },
	{ name: "Material", value: "Plastic" },
	{ name: "Transparency", value: "0" },
	{ name: "CastShadow", value: "true" },
];

const screen = Instance.new("ScreenGui");
screen.Name = "PropertiesPanel";

// **Parented into `StarterGui`, which is what makes it draw at all.** A
// `ScreenGui` is only laid out under `StarterGui` or a player's `PlayerGui` —
// Roblox's containment rule, which `gui::Layout` enforces since v0.8.
screen.Parent = StarterGui;

const panel = Instance.new("Frame");
panel.Name = "Panel";
panel.AnchorPoint = Vector2.new(1, 0);
panel.Position = UDim2.new(1, -16, 0, 16);
panel.Size = UDim2.new(0, 320, 0, 400);
panel.BackgroundColor3 = PANEL;
panel.BorderSizePixel = 0;
panel.Parent = screen;

const panelCorner = Instance.new("UICorner");
panelCorner.CornerRadius = UDim.new(0, 6);
panelCorner.Parent = panel;

const panelStroke = Instance.new("UIStroke");
panelStroke.Color = RULE;
panelStroke.Thickness = 1;
panelStroke.Parent = panel;

const title = Instance.new("TextLabel");
title.Name = "Title";
title.Size = UDim2.new(1, 0, 0, 32);
title.BackgroundTransparency = 1;
title.Text = "Properties";
title.TextColor3 = INK;
title.TextSize = 15;
title.TextXAlignment = Enum.TextXAlignment.Left;
title.Parent = panel;

const titlePadding = Instance.new("UIPadding");
titlePadding.PaddingLeft = UDim.new(0, 12);
titlePadding.Parent = title;

// **A `ScrollingFrame`, because a grid is longer than its panel.** This is the
// class whose canvas the layout resolves against rather than against the frame,
// which is the one thing about it that is not obvious and is why it is here
// rather than a plain `Frame`.
const list = Instance.new("ScrollingFrame");
list.Name = "Rows";
list.Position = UDim2.new(0, 0, 0, 32);
list.Size = UDim2.new(1, 0, 1, -32);
list.BackgroundTransparency = 1;
list.BorderSizePixel = 0;
list.CanvasSize = UDim2.new(0, 0, 0, ROWS.length * ROW_HEIGHT);
list.Parent = panel;

// **Stacked by a layout rather than by arithmetic.** A grid that positioned its
// own rows would be a second answer to what `UIListLayout` already decides, and
// the two would disagree the first time a row's height changed.
const stack = Instance.new("UIListLayout");
stack.FillDirection = Enum.FillDirection.Vertical;
stack.SortOrder = Enum.SortOrder.LayoutOrder;
stack.Parent = list;

let selected: TextButton | null = null;

function makeRow(row: Row, order: number): TextButton {
	// **A `TextButton` rather than a `Frame`, because a row is clickable.** A
	// plain frame is decoration and the click goes through it — which is
	// `gui::Router`'s rule and exactly right for a background, and exactly wrong
	// for a row somebody is trying to select.
	const item = Instance.new("TextButton");
	item.Name = row.name;
	item.LayoutOrder = order;
	item.Size = UDim2.new(1, 0, 0, ROW_HEIGHT);
	item.BackgroundColor3 = PAPER;
	item.BackgroundTransparency = 1;
	item.BorderSizePixel = 0;

	// The button's own text is empty: the two labels below carry it, because a
	// property grid is two columns and a single centred string is not one.
	item.Text = "";
	item.Parent = list;

	const padding = Instance.new("UIPadding");
	padding.PaddingLeft = UDim.new(0, 12);
	padding.PaddingRight = UDim.new(0, 12);
	padding.Parent = item;

	const name = Instance.new("TextLabel");
	name.Name = "Key";
	name.Size = UDim2.new(LABEL_WIDTH, 0, 1, 0);
	name.BackgroundTransparency = 1;
	name.Text = row.name;
	name.TextColor3 = MUTED;
	name.TextSize = 13;
	name.TextXAlignment = Enum.TextXAlignment.Left;
	name.Parent = item;

	const value = Instance.new("TextLabel");
	value.Name = "Value";
	value.Position = UDim2.new(LABEL_WIDTH, 0, 0, 0);
	value.Size = UDim2.new(1 - LABEL_WIDTH, 0, 1, 0);
	value.BackgroundTransparency = 1;
	value.Text = row.value;
	value.TextColor3 = INK;
	value.TextSize = 13;
	value.TextXAlignment = Enum.TextXAlignment.Left;
	value.Parent = item;

	// **The join this version added, seen from a script.** `gui::Router`
	// produces the event, `script::Signals` delivers it at the barrier, and this
	// runs. Until that existed a row could be drawn and could not answer.
	item.Activated.Connect(() => {
		if (selected !== null) {
			selected.BackgroundTransparency = 1;
		}
		selected = item;
		item.BackgroundColor3 = ACCENT;
		item.BackgroundTransparency = 0.85;
	});

	return item;
}

for (let index = 0; index < ROWS.length; index++) {
	makeRow(ROWS[index], index);
}

print(`properties panel: ${ROWS.length} rows, authored in TypeScript`);
