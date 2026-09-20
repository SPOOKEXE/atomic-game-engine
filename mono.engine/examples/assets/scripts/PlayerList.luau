--!strict
-- Who is in this game, drawn from `Players` and kept visible while it changes.
--
-- **The first scene in this repository that reads the player list at all.**
-- `scene::AddPlayer` got its first production caller at v0.15 - before that
-- every world that ever ran had a `Players` service with nobody in it - so
-- `GetPlayers`, `PlayerAdded` and `PlayerRemoving` were bound, declared, tested
-- and had nothing to say. This is the scene that gives them something.
--
-- ## Where the rows are written, and why it is not `StarterGui`
--
-- The panel is built **into every player's own `PlayerGui`** rather than into
-- `StarterGui`. That is not a style choice:
--
-- - `StarterGui` is a *template*. `gui::ResetPlayerGui` clones it into a
--   `PlayerGui` when a character spawns, so a row added to the template after
--   somebody joined never reaches the copy they are looking at. A list built
--   there is a list that is correct for exactly one instant per player.
-- - A `PlayerGui` is **private to its player on the wire** - that is what
--   `scene::PlayerOwning` decides, and `just check` asserts a client holds four
--   containers of its own and zero of anybody else's. So writing into each
--   player's copy is both live and correctly scoped, and it is the only place
--   both are true at once.
--
-- So a dedicated server hosting this scene keeps every connected client's list
-- up to date, and no client sees another's panel.
--
-- The panel is always visible. Its state belongs to the player list, so camera
-- and character input cannot hide it.
--
-- ## Running it
--
--     SCENE=PlayerList.luau scripts/demos/run-local-server.sh 4
--     ./client --script PlayerList.luau           # one local player
--
-- The server is what makes it worth looking at: four clients means four names,
-- and each one appearing and vanishing as a process starts and stops. A
-- A standalone client is its own single-player host, so it establishes one
-- local player before this script starts. The panel therefore contains that
-- player. A networked run replaces the interface world with the replica after
-- admission and shows everybody the authority has actually admitted.

local Players = game:GetService("Players")
local RunService = game:GetService("RunService")
local workspace = game:GetService("Workspace")

-- The palette, in one place, for `Interface.luau`'s reason: a literal colour at
-- a point of use is one that cannot be changed without finding every copy.
local INK = Color3.fromRGB(18, 20, 26)
local PANEL = Color3.fromRGB(30, 34, 44)
local ROW = Color3.fromRGB(44, 50, 64)
local MINE = Color3.fromRGB(52, 78, 104)
local PAPER = Color3.fromRGB(232, 236, 244)
local MUTED = Color3.fromRGB(150, 160, 178)
local ACCENT = Color3.fromRGB(96, 176, 232)

local GROUND = Color3.fromRGB(58, 64, 58)
local PAD = Color3.fromRGB(96, 132, 96)

-- --- somewhere to stand ------------------------------------------------------

-- **A scene of its own rather than a panel bolted onto `Playground.luau`.** A
-- demo that needs another scene to be useful is one nobody runs on its own, and
-- the point of this one is the *list* - so it carries the least world that lets
-- several people be in it at once.

local function block(name: string, size: Vector3, at: Vector3, colour: Color3): Part
	local part = Instance.new("Part")
	part.Name = name
	part.Size = size
	part.Position = at
	part.Color = colour
	part.Anchored = true
	part.Parent = workspace
	return part
end

-- Clear of a baseplate's top face at y = 0: coplanar surfaces do not flicker in
-- this engine, they resolve stably to whichever one the draw order happens to
-- favour. `scripts/demos/zfight-scan.sh` holds the control for that.
local GROUND_LIFT = 0.01

block("Ground", Vector3.new(160, 4, 160), Vector3.new(0, -2 + GROUND_LIFT, 0), GROUND)

-- The name is the contract: `scene::FindSpawn` looks for exactly this under
-- `Workspace`, and without it every character arrives at the origin inside the
-- floor.
block("SpawnLocation", Vector3.new(24, 1, 24), Vector3.new(0, 0.5, 0), PAD)

-- Four posts, so a player can tell they have moved. A featureless plane is a
-- scene where walking looks like standing still.
for index = 1, 4 do
	local angle = (index - 1) * math.pi * 0.5
	block(
		"Post" .. tostring(index),
		Vector3.new(3, 14, 3),
		Vector3.new(math.cos(angle) * 40, 7, math.sin(angle) * 40),
		PAD
	)
end

-- --- the panel ---------------------------------------------------------------

local ROW_HEIGHT = 28
local PANEL_WIDTH = 260

-- Builds an empty panel into one player's own container.
--
-- Returns the frame the rows go under, so the caller never has to walk back
-- down the tree it just built.
local function buildPanel(container: Instance): Frame
	local screen = Instance.new("ScreenGui")
	screen.Name = "PlayerList"

	-- **False, so the panel survives a respawn.** `gui::ResetPlayerGui` does
	-- Roblox's rule - a collector with `ResetOnSpawn` true is destroyed and
	-- re-cloned every life - and a list that vanished every time somebody died
	-- would be rebuilt from scratch by the loop below, which works and is
	-- visibly wrong for one frame each time.
	screen.ResetOnSpawn = false
	screen.Parent = container

	local card = Instance.new("Frame")
	card.Name = "Card"
	card.AnchorPoint = Vector2.new(1, 0)
	card.Position = UDim2.new(1, -16, 0, 16)
	card.Size = UDim2.new(0, PANEL_WIDTH, 0, 44)
	card.BackgroundColor3 = PANEL
	card.BorderSizePixel = 0
	card.Parent = screen

	local corner = Instance.new("UICorner")
	corner.CornerRadius = UDim.new(0, 10)
	corner.Parent = card

	local stroke = Instance.new("UIStroke")
	stroke.Color = INK
	stroke.Thickness = 2
	stroke.Parent = card

	local padding = Instance.new("UIPadding")
	padding.PaddingTop = UDim.new(0, 10)
	padding.PaddingBottom = UDim.new(0, 10)
	padding.PaddingLeft = UDim.new(0, 10)
	padding.PaddingRight = UDim.new(0, 10)
	padding.Parent = card

	local list = Instance.new("UIListLayout")
	list.FillDirection = Enum.FillDirection.Vertical
	list.Padding = UDim.new(0, 4)

	-- **`LayoutOrder` and not the order rows were created in.** A player
	-- leaving is a row destroyed from the middle, so creation order stops
	-- meaning anything after the first departure - and a list that reshuffled
	-- itself when somebody left is the bug a sort exists to prevent.
	list.SortOrder = Enum.SortOrder.LayoutOrder
	list.Parent = card

	local header = Instance.new("TextLabel")
	header.Name = "Header"
	header.LayoutOrder = 0
	header.Size = UDim2.new(1, 0, 0, 24)
	header.BackgroundTransparency = 1
	header.Text = "Players"
	header.TextColor3 = ACCENT
	header.TextSize = 18
	header.Font = Enum.Font.Bold
	header.TextXAlignment = Enum.TextXAlignment.Left
	header.Parent = card

	local rows = Instance.new("Frame")
	rows.Name = "Rows"
	rows.LayoutOrder = 1
	rows.Size = UDim2.new(1, 0, 0, 0)
	rows.BackgroundTransparency = 1
	rows.Parent = card

	local rowList = Instance.new("UIListLayout")
	rowList.FillDirection = Enum.FillDirection.Vertical
	rowList.Padding = UDim.new(0, 4)
	rowList.SortOrder = Enum.SortOrder.LayoutOrder
	rowList.Parent = rows

	return rows
end

-- One name in the list.
local function buildRow(rows: Frame, order: number, name: string, mine: boolean)
	local row = Instance.new("Frame")
	row.Name = "Row" .. tostring(order)
	row.LayoutOrder = order
	row.Size = UDim2.new(1, 0, 0, ROW_HEIGHT)

	-- **This player's own row is tinted, which is the one thing a list of
	-- names cannot say on its own.** With two clients called `Player1` and
	-- `Player2` there is otherwise nothing on screen that tells a person which
	-- of the two they are - and `Players.LocalPlayer` is exactly the fact that
	-- answers it, on the client where it means something.
	row.BackgroundColor3 = if mine then MINE else ROW
	row.BorderSizePixel = 0
	row.Parent = rows

	local corner = Instance.new("UICorner")
	corner.CornerRadius = UDim.new(0, 6)
	corner.Parent = row

	local label = Instance.new("TextLabel")
	label.Name = "Name"
	label.Position = UDim2.new(0, 8, 0, 0)
	label.Size = UDim2.new(1, -16, 1, 0)
	label.BackgroundTransparency = 1
	label.Text = name
	label.TextColor3 = if mine then PAPER else MUTED
	label.TextSize = 15
	label.TextXAlignment = Enum.TextXAlignment.Left
	label.TextYAlignment = Enum.TextYAlignment.Center
	label.Parent = row
end

-- --- keeping the rows in step ------------------------------------------------

-- The panel each player is looking at, so it is built once rather than every
-- time the list changes.
local panels: { [Instance]: Frame } = {}

local function existingRows(container: Instance): Frame?
	local screen = container:FindFirstChild("PlayerList")
	if screen == nil or not screen:IsA("ScreenGui") then
		return nil
	end
	local card = screen:FindFirstChild("Card")
	if card == nil or not card:IsA("Frame") then
		return nil
	end
	local rows = card:FindFirstChild("Rows")
	if rows == nil or not rows:IsA("Frame") then
		return nil
	end
	return rows :: Frame
end

-- **Every one of these takes a `Player` and `GetPlayers` hands back
-- `{ Instance }`, so each caller casts once at the loop.** That is the union
-- `D00114` describes rather than a gap: the buffer narrows wherever it writes a
-- class down - `Instance.new`, `game:GetService`, `:Clone()` - and a list's
-- element type is not one of those places. A cast at the loop is one statement
-- of a fact the engine cannot infer; a cast at every member read would be
-- several.
local function panelFor(player: Player): Frame?
	local existing = panels[player]
	if existing and existing.Parent ~= nil then
		return existing
	end
	panels[player] = nil

	-- `PlayerGui` is a computed property: the container has existed since v0.8
	-- and nothing could name it until v0.15.
	local container = player.PlayerGui
	if not container then
		return nil
	end

	local rows = existingRows(container)
	if rows == nil then
		local stale = container:FindFirstChild("PlayerList")
		if stale ~= nil then
			stale:Destroy()
		end
		rows = buildPanel(container)
	end
	panels[player] = rows
	return rows
end

-- Rebuilds one player's rows from the current list.
--
-- **Rebuilt whole rather than diffed**, and that is a decision about what this
-- costs: a list is a handful of rows and a change happens when somebody joins
-- or leaves, so the diff would be more code than the rebuild for something that
-- runs approximately never. `gui::Compiled` is what stops a rebuild being a
-- repaint - the draw list is kept until its signature moves.
local function refresh(player: Player)
	local rows = panelFor(player)
	if not rows then
		return
	end

	for _, child in rows:GetChildren() do
		-- The layout is a child too, and destroying it would take the
		-- arrangement with it.
		if child:IsA("Frame") then
			child:Destroy()
		end
	end

	local order = 0
	for _, entry in Players:GetPlayers() do
		local other = entry :: Player
		order += 1
		buildRow(rows, order, other.Name, other == player)
	end

	local card = rows.Parent :: Frame?
	if card then
		-- The card grows with the list. `AutomaticSize` would do this without
		-- arithmetic and is not implemented here, so the height is what the
		-- rows add up to plus the header and the padding.
		card.Size = UDim2.new(0, PANEL_WIDTH, 0, 44 + order * (ROW_HEIGHT + 4))
	end
end

local function refreshEverybody()
	for _, entry in Players:GetPlayers() do
		refresh(entry :: Player)
	end
end

-- **Everybody's list changes when anybody joins**, which is why neither of
-- these refreshes only the player it is about. A list that updated one client's
-- panel would leave every other client showing the lobby as it was when they
-- arrived.
Players.PlayerAdded:Connect(function(_player: Instance)
	refreshEverybody()
end)

Players.PlayerRemoving:Connect(function(player: Instance)
	-- Dropped before the rebuild, so the departing player's own panel is not
	-- rebuilt into a container that is about to be destroyed.
	panels[player] = nil
	refreshEverybody()
end)

-- Whoever is already here. `PlayerAdded` fires for arrivals and this scene may
-- be loaded into a world that has some - a studio Play run adds its player
-- before the scripts start.
refreshEverybody()

-- **Kept in step on a clock as well as on the signals**, and the reason is the
-- one this scene exists to demonstrate: a player's *container* is made when
-- they join and their `PlayerGui` may not exist on the tick `PlayerAdded`
-- fires. A rebuild that only ever ran from the signal would leave the first
-- player of a session with no panel at all, and it would look like the signal
-- not firing.
local sinceRefresh = 0.0
RunService.Heartbeat:Connect(function(delta: number)
	sinceRefresh += delta
	if sinceRefresh < 1.0 then
		return
	end
	sinceRefresh = 0.0

	local missing = false
	for _, entry in Players:GetPlayers() do
		local rows = panels[entry]
		if not rows or rows.Parent == nil then
			missing = true
		end
	end

	if missing then
		refreshEverybody()
	end
end)
