--!strict
-- A `ScreenGui` built entirely from a script, which is the point of it.
--
-- `SkyGrid.luau` proves the world path and `Mirrors-1-world.luau` proves the
-- rendering path. This proves the **third** one v0.8 added: `Instance.new` over
-- the `gui` class tree, `UDim2` and `Vector2` as real property types, the
-- layout pass, and the compiled draw list a backend paints.
--
-- ## What it is arranged to catch
--
-- Every element here exists because one specific thing looks correct without it
-- and obviously wrong with it.
--
-- - The **anchored, centred panel** catches an anchor point applied in the
--   wrong direction. Half its own size out of place is the classic UI bug and
--   it reads as a margin nobody set.
-- - The **list of rows** catches a sort that ignores `LayoutOrder`, and the
--   padding between them catches a gap applied once instead of between.
-- - The **grid of swatches** catches a wrap that counts cells without counting
--   the padding, which puts the last column half off the panel.
-- - The **clipped strip** has content four times its own width. A clip that is
--   replaced rather than intersected lets it draw over the panel border, which
--   is the failure that only shows up once something is nested.
-- - The **corner radii and strokes** catch a radius resolved against the wrong
--   axis: a scale of 0.5 on a wide element becomes an ellipse rather than a
--   pill.
-- - The **right-aligned and left-aligned labels** catch the two text alignment
--   enums being read as each other, which is easy because Roblox numbers them
--   differently and this engine keeps its numbering.
--
-- ## Why it does not animate
--
-- The compiled list is kept until its signature moves — see `gui/Compile.hpp` —
-- and a scene that changed every frame would rebuild every frame and prove
-- nothing about the cache. The one moving part is deliberate and is the *only*
-- thing that should cause a rebuild.

local RunService = game:GetService("RunService")

-- The palette, in one place. Same rule as `SkyGrid.luau`'s and `ui::Theme`'s: a
-- literal colour at a point of use is one that cannot be changed without
-- finding every copy.
local INK = Color3.fromRGB(24, 28, 38)
local PANEL = Color3.fromRGB(38, 44, 58)
local RAISED = Color3.fromRGB(52, 60, 78)
local ACCENT = Color3.fromRGB(96, 176, 232)
local PAPER = Color3.fromRGB(232, 236, 244)
local MUTED = Color3.fromRGB(150, 160, 178)

local SWATCHES = {
	Color3.fromRGB(196, 84, 72),
	Color3.fromRGB(214, 158, 74),
	Color3.fromRGB(126, 178, 96),
	Color3.fromRGB(96, 152, 196),
	Color3.fromRGB(150, 118, 190),
	Color3.fromRGB(216, 132, 168),
}

local screen = Instance.new("ScreenGui")
screen.Name = "InterfaceExample"

-- **Parented into `StarterGui`, which is what makes it draw at all.**
--
-- A `ScreenGui` is only laid out when it sits under `StarterGui` or under a
-- player's `PlayerGui` — Roblox's containment rule, and `gui::Layout` enforces
-- it. This file left it unparented for a version and drew anyway, which was the
-- engine being more permissive than the thing it is modelled on: an author
-- could have shipped an interface that appeared in the studio and was missing
-- in the client, because the place it worked was the place it was checked.
--
-- `GetService` finds it in the tree, the same way `Players` and
-- `ReplicatedStorage` are found. It is `StarterGui` rather than a `PlayerGui`
-- because this is a scene an editor opens, and a player's copy is made when a
-- player spawns.
screen.Parent = game:GetService("StarterGui")

-- The card everything else sits in. Anchored at its own centre and placed at
-- the screen's centre, so it stays centred at any window size — which is the
-- assertion, not the decoration.
local card = Instance.new("Frame")
card.Name = "Card"
card.AnchorPoint = Vector2.new(0.5, 0.5)
card.Position = UDim2.new(0.5, 0, 0.5, 0)
card.Size = UDim2.new(0, 520, 0, 380)
card.BackgroundColor3 = PANEL
card.BorderSizePixel = 0
card.Parent = screen

local cardCorner = Instance.new("UICorner")
cardCorner.CornerRadius = UDim.new(0, 12)
cardCorner.Parent = card

local cardStroke = Instance.new("UIStroke")
cardStroke.Color = INK
cardStroke.Thickness = 2
cardStroke.Parent = card

local cardPadding = Instance.new("UIPadding")
cardPadding.PaddingTop = UDim.new(0, 16)
cardPadding.PaddingBottom = UDim.new(0, 16)
cardPadding.PaddingLeft = UDim.new(0, 16)
cardPadding.PaddingRight = UDim.new(0, 16)
cardPadding.Parent = card

-- **The card runs a list layout, so nothing below sets a `Position`.** A
-- position set here and ignored is how somebody concludes the layout is broken;
-- the section frames set only their sizes.
local cardList = Instance.new("UIListLayout")
cardList.FillDirection = Enum.FillDirection.Vertical
cardList.Padding = UDim.new(0, 12)
cardList.SortOrder = Enum.SortOrder.LayoutOrder
cardList.Parent = card

local function section(name: string, order: number, height: number): Frame
	local frame = Instance.new("Frame")
	frame.Name = name
	frame.LayoutOrder = order
	frame.Size = UDim2.new(1, 0, 0, height)
	frame.BackgroundColor3 = RAISED
	frame.BorderSizePixel = 0
	frame.Parent = card

	local corner = Instance.new("UICorner")
	corner.CornerRadius = UDim.new(0, 8)
	corner.Parent = frame

	return frame
end

-- --- the title row -----------------------------------------------------------

local header = section("Header", 1, 44)

local title = Instance.new("TextLabel")
title.Name = "Title"
title.Size = UDim2.new(1, -16, 1, 0)
title.Position = UDim2.new(0, 8, 0, 0)
title.BackgroundTransparency = 1
title.Text = "Interface example"
title.TextColor3 = PAPER
title.TextSize = 20
title.Font = Enum.Font.Bold

-- Left and Center, deliberately not both defaults. Reading the two alignment
-- enums as each other puts this in the middle and the clock on the left, which
-- is a swap somebody would stare at for a while.
title.TextXAlignment = Enum.TextXAlignment.Left
title.TextYAlignment = Enum.TextYAlignment.Center
title.Parent = header

local clock = Instance.new("TextLabel")
clock.Name = "Clock"
clock.AnchorPoint = Vector2.new(1, 0.5)
clock.Position = UDim2.new(1, -10, 0.5, 0)
clock.Size = UDim2.new(0, 120, 0, 24)
clock.BackgroundTransparency = 1
clock.Text = "0.0 s"
clock.TextColor3 = ACCENT
clock.TextSize = 16
clock.TextXAlignment = Enum.TextXAlignment.Right
clock.Parent = header

-- --- the swatch grid ---------------------------------------------------------

local palette = section("Palette", 2, 96)

local grid = Instance.new("UIGridLayout")
grid.CellSize = UDim2.new(0, 68, 0, 36)
grid.CellPadding = UDim2.new(0, 8, 0, 8)
grid.FillDirectionMaxCells = 6
grid.Parent = palette

local gridPadding = Instance.new("UIPadding")
gridPadding.PaddingTop = UDim.new(0, 10)
gridPadding.PaddingLeft = UDim.new(0, 10)
gridPadding.Parent = palette

for index, colour in SWATCHES do
	local swatch = Instance.new("TextButton")
	swatch.Name = "Swatch" .. tostring(index)
	swatch.LayoutOrder = index

	-- Deliberately wrong, and deliberately ignored: a grid decides both axes,
	-- which is the difference between it and a list.
	swatch.Size = UDim2.new(0, 5, 0, 5)

	swatch.BackgroundColor3 = colour
	swatch.BorderSizePixel = 0
	swatch.Text = ""
	swatch.Parent = palette

	local corner = Instance.new("UICorner")
	corner.CornerRadius = UDim.new(0, 6)
	corner.Parent = swatch

	-- **A swatch says it was clicked and changes nothing.** The panel above
	-- explains why this scene does not animate — the compiled list is kept until
	-- its signature moves, and a scene that rebuilt every frame would prove
	-- nothing about the cache — so recolouring on a click would undo that. A log
	-- line is the whole of the response, and it is the line `just client-smoke`
	-- reads back: an `Activated` that fires in the editor and not in a shipped
	-- client is the bug `just client-smoke` exists to catch, and until v0.15
	-- nothing anywhere could press a button in one.
	swatch.Activated:Connect(function()
		print("interface: swatch " .. tostring(index) .. " activated")
	end)
end

-- --- the clipped strip -------------------------------------------------------

local strip = section("Strip", 3, 56)
strip.ClipsDescendants = true

local wide = Instance.new("Frame")
wide.Name = "Wide"
wide.Position = UDim2.new(0, 12, 0, 12)

-- Four times the strip's width. Anything that draws past the strip's own
-- rounded edge is a clip that was replaced rather than intersected.
wide.Size = UDim2.new(4, 0, 0, 32)
wide.BackgroundColor3 = ACCENT
wide.BorderSizePixel = 0
wide.Parent = strip

local wideCorner = Instance.new("UICorner")
wideCorner.CornerRadius = UDim.new(0, 16)
wideCorner.Parent = wide

local wideLabel = Instance.new("TextLabel")
wideLabel.Size = UDim2.new(0, 300, 1, 0)
wideLabel.BackgroundTransparency = 1
wideLabel.Text = "clipped to the strip, not to the card"
wideLabel.TextColor3 = INK
wideLabel.TextSize = 14
wideLabel.TextXAlignment = Enum.TextXAlignment.Left
wideLabel.Position = UDim2.new(0, 14, 0, 0)
wideLabel.Parent = wide

-- --- the button row ----------------------------------------------------------

local actions = section("Actions", 4, 52)
actions.BackgroundTransparency = 1

local row = Instance.new("UIListLayout")
row.FillDirection = Enum.FillDirection.Horizontal
row.Padding = UDim.new(0, 10)
row.HorizontalAlignment = Enum.HorizontalAlignment.Right
row.VerticalAlignment = Enum.VerticalAlignment.Center
row.SortOrder = Enum.SortOrder.LayoutOrder
row.Parent = actions

local hint = Instance.new("TextLabel")
hint.Name = "Hint"
hint.LayoutOrder = 1
hint.Size = UDim2.new(0, 220, 0, 20)
hint.BackgroundTransparency = 1
hint.Text = "hover a button to see AutoButtonColor"
hint.TextColor3 = MUTED
hint.TextSize = 13
hint.TextXAlignment = Enum.TextXAlignment.Left
hint.Parent = actions

local function button(name: string, order: number, label: string, fill: Color3): TextButton
	local made = Instance.new("TextButton")
	made.Name = name
	made.LayoutOrder = order
	made.Size = UDim2.new(0, 96, 0, 32)
	made.BackgroundColor3 = fill
	made.BorderSizePixel = 0
	made.Text = label
	made.TextColor3 = PAPER
	made.TextSize = 15
	made.Parent = actions

	local corner = Instance.new("UICorner")
	corner.CornerRadius = UDim.new(0, 6)
	corner.Parent = made

	return made
end

local cancel = button("Cancel", 2, "Cancel", RAISED)
local confirm = button("Confirm", 3, "Confirm", ACCENT)

-- --- what the buttons do, which is the part that was missing ------------------
--
-- **This is the whole of the gui→script join, seen from a script.** Everything
-- above describes an interface; until `gui::Router`'s events reached a VM, none
-- of it could answer. `.Activated` is `SignalKind::GuiActivated` and arrives at
-- the barrier like every other signal, so a handler here sees the same world a
-- `Heartbeat` handler does.
--
-- The swatches are the proof that a *generated* element connects: they are made
-- in a loop above and nothing distinguishes them from the two named buttons.
-- If the join only worked for instances a script held a local for, this is the
-- half that would silently do nothing.
local picked = SWATCHES[1]
local clicks = 0

local function refresh()
	hint.Text = string.format("%d click%s · picked", clicks, clicks == 1 and "" or "s")
	card.BackgroundColor3 = picked
end

for index, colour in SWATCHES do
	local swatch = palette:FindFirstChild("Swatch" .. tostring(index))
	if swatch then
		swatch.Activated:Connect(function()
			picked = colour
			clicks += 1
			refresh()
		end)
	end
end

confirm.Activated:Connect(function()
	clicks += 1
	-- Something a person can see without reading a number: the card takes the
	-- accent, which is the one colour nothing else in this scene sets it to.
	picked = ACCENT
	refresh()
end)

cancel.Activated:Connect(function()
	clicks = 0
	picked = SWATCHES[1]
	refresh()
end)

-- --- the close button ---------------------------------------------------------
--
-- **`Enabled` on the collector, not `Visible` on the card.** A `ScreenGui` that
-- is disabled is not laid out at all — `gui::Layout` skips it before it asks how
-- big anything is — so closing this way costs nothing per frame afterwards.
-- Hiding the card instead would leave the whole subtree being measured and
-- compiled for something nobody can see, which is the difference between a
-- closed panel and an invisible one.
--
-- **And it cannot be reopened from inside itself**, which is correct rather than
-- a limitation: nothing under a disabled collector receives input either, so a
-- reopen has to come from somewhere that is still live. That is what a menu bar
-- is for, and this scene has none.
local close = Instance.new("TextButton")
close.Name = "Close"

-- Anchored to the card's own top-right corner, so it stays there whatever the
-- card's size resolves to.
close.AnchorPoint = Vector2.new(1, 0)
close.Position = UDim2.new(1, 0, 0, 0)
close.Size = UDim2.new(0, 22, 0, 22)
close.BackgroundColor3 = RAISED
close.BorderSizePixel = 0
close.Text = "X"
close.TextColor3 = MUTED
close.TextSize = 13
close.ZIndex = 2
close.Parent = card

local closeCorner = Instance.new("UICorner")
closeCorner.CornerRadius = UDim.new(0, 4)
closeCorner.Parent = close

close.Activated:Connect(function()
	screen.Enabled = false
end)

-- `MouseEnter` and `MouseLeave` carry `(x, y)` in canvas pixels, which is the
-- Roblox signature. Written onto the hint so the two arguments are visibly
-- arriving rather than merely declared.
confirm.MouseEnter:Connect(function(x: number, y: number)
	hint.Text = string.format("confirm @ %d, %d", x, y)
end)

confirm.MouseLeave:Connect(function()
	refresh()
end)

refresh()

-- --- the one thing that moves ------------------------------------------------

-- **A tenth of a second, not a frame.** The compiled list is kept while its
-- signature holds, so writing the clock every frame would rebuild every frame
-- and the cache would be doing nothing. At ten hertz the rebuild count against
-- the frame count is a number somebody can read off the statistics panel and
-- check.
local elapsed = 0
local shown = -1

RunService.Heartbeat:Connect(function(delta: number)
	elapsed += delta

	local tenths = math.floor(elapsed * 10)
	if tenths ~= shown then
		shown = tenths
		clock.Text = string.format("%.1f s", tenths / 10)
	end
end)
