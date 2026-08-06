--!strict
-- Every imported mesh at once, on turntables, in a grid.
--
-- `Meshes.luau` proves one glTF and one PMX arrive and draw. This is the wider
-- check: nine models from three importers, laid out so a single frame says
-- which of them made it. **A grid is the point** — a mesh that failed to
-- resolve draws as `MeshTable`'s fallback cube, so a cube standing in a cell
-- where a dragon should be is a name that did not match, and it is obvious at a
-- glance rather than after a log hunt.
--
-- Each model turns on its own pedestal, because a still frame hides the half of
-- a mesh facing away — and a reversed winding is exactly the bug that looks
-- fine from the front.
--
-- ## What is in it and where it came from
--
-- Three importers, and the mix is deliberate rather than decorative:
--
--   * **`props/fox.amesh`** — glTF, authored elsewhere and untextured, so it
--     draws in its materials' base colours. Ten of them.
--   * **`props/dragon.amesh`, `characters/wizard.amesh`** — glTF exported from
--     Blender. Their materials are *procedural shader graphs*, which glTF has
--     no way to carry, so these arrive with base colours and no sheets. That is
--     the exporter's limit and not the pipeline's, and it is worth seeing next
--     to the PMX characters, which do carry sheets.
--   * **the five PMX models** — MikuMikuDance characters with real texture
--     sheets, non-ASCII file names, and per-submesh materials. If these draw
--     textured, then `bake::ReadPmx`, the UTF-16 path decoding, the texture
--     rewriting in `assetc`, the manifest, the signature and `TextureTable`
--     are all working at once.
--
-- ## Run it
--
--     assetc --input ART --output content --no-copy --model-size 1
--     cdn --publish content --store store --signing-key HEX
--     client --cdn dir:store --publisher-key PUBLIC --script MeshGrid.luau \
--            --entities 2048
--
-- **`--model-size 1` is load-bearing and easy to get wrong.** A `MeshPart`'s
-- `Size` multiplies the mesh's own coordinates rather than fitting the mesh
-- into a box, so a model baked at `--model-size 4` and given `Size = 4` draws
-- sixteen metres across and overlaps its neighbours. Baking imports as *unit*
-- meshes makes them behave exactly like the built-ins, and then `Size` means
-- metres for every part in the scene rather than metres for some and a
-- multiplier for others.
--
-- **`--entities` is also the view channel's byte ceiling**, and this scene is
-- large. Leave it at the default and the channel refuses to publish anything,
-- which looks exactly like a content failure and is not one.
--
-- With no `--cdn` the scene still runs and every cell is a cube. That is the
-- honest degraded state, and it is the first thing to check when a cell is
-- wrong: if *every* cell is a cube the content never arrived, and if one is,
-- the name in `MODELS` below does not match what the publisher named.

local RunService = game:GetService("RunService")

-- The names a publisher gave these are the names this asks for, verbatim. There
-- is no lookup table between the two anywhere — the manifest's name *is* the
-- mesh's name, which is `AGENTS.md` rule 4 surviving a signature and a network
-- hop.
local MODELS = {
	{ mesh = "props/fox.amesh", label = "fox (glTF)" },
	{ mesh = "props/dragon.amesh", label = "dragon (glTF)" },
	{ mesh = "characters/wizard.amesh", label = "wizard (glTF)" },
	{ mesh = "characters/xiao/魈.amesh", label = "xiao (PMX)" },
	{ mesh = "characters/barbara/芭芭拉.amesh", label = "barbara (PMX)" },
	{ mesh = "characters/ruriki/梦见月瑞希.amesh", label = "ruriki (PMX)" },
	{ mesh = "characters/ruriki/梦见月瑞希_垂尾.amesh", label = "ruriki tail (PMX)" },
	{ mesh = "characters/ruriki/食梦貘.amesh", label = "baku (PMX)" },
	{ mesh = "characters/ruriki/托盘.amesh", label = "tray (PMX)" },
}

local COLUMNS = 3
local SPACING = 7

-- How tall a model stands, in metres. **This is metres only because the bake
-- was `--model-size 1`** — see the header. `assetc` fitted every import's
-- longest axis to one and centred it, so this number scales a unit mesh the
-- same way it scales a built-in cube, and a 2M-triangle wizard ends up the same
-- height as a low-poly fox.
local MODEL_SIZE = 4

local PEDESTAL_HEIGHT = 0.6
local ROWS = math.ceil(#MODELS / COLUMNS)

-- Enough plate to stand on with a margin, derived rather than typed so adding a
-- tenth model does not leave it hanging off the edge.
local PLATE_X = COLUMNS * SPACING + SPACING
local PLATE_Z = ROWS * SPACING + SPACING

local floor = Instance.new("Part")
floor.Name = "Baseplate"
floor.Anchored = true
floor.Size = Vector3.new(PLATE_X, 0.5, PLATE_Z)
floor.CFrame = CFrame.new(Vector3.new(0, -0.25, 0))
floor.Color = Color3.new(0.26, 0.28, 0.32)
floor.Parent = workspace

-- Where a cell's centre sits, centred on the origin rather than growing out of
-- one corner. Rows advance into +Z and the camera stands at +Z, so `MODELS[1]`
-- is the *back* left cell and the last entry is front right — worth knowing
-- when a particular model is the one being checked.
local function cellPosition(index: number): Vector3
	local column = (index - 1) % COLUMNS
	local row = math.floor((index - 1) / COLUMNS)

	return Vector3.new(
		(column - (COLUMNS - 1) / 2) * SPACING,
		0,
		(row - (ROWS - 1) / 2) * SPACING
	)
end

local turntables: { { Part: MeshPart, Rate: number } } = {}

for index, model in ipairs(MODELS) do
	local at = cellPosition(index)

	-- A pedestal per cell, and it earns its place twice: it separates the cells
	-- visually, and it is a part the renderer definitely draws — so a cell
	-- showing a pedestal and nothing above it is a mesh that failed, while a
	-- cell showing nothing at all is a scene that never loaded.
	local pedestal = Instance.new("Part")
	pedestal.Name = `Pedestal_{index}`
	pedestal.Anchored = true
	pedestal.Size = Vector3.new(SPACING * 0.7, PEDESTAL_HEIGHT, SPACING * 0.7)
	pedestal.CFrame = CFrame.new(at + Vector3.new(0, PEDESTAL_HEIGHT / 2, 0))

	-- Alternating, so the grid's rows and columns read as a grid. Nothing
	-- depends on it.
	local tint = (index % 2 == 0) and 0.42 or 0.34
	pedestal.Color = Color3.new(tint, tint * 0.98, tint * 1.08)
	pedestal.Parent = workspace

	local part = Instance.new("MeshPart")
	part.Name = model.label
	part.Anchored = true

	-- **The one string that has to match.** Everything else in this file is
	-- arithmetic; this is the name the publisher signed.
	part.MeshId = model.mesh

	part.Size = Vector3.new(MODEL_SIZE, MODEL_SIZE, MODEL_SIZE)

	-- Sat on top of its pedestal rather than at a fixed height, so the row
	-- lines up whatever the pedestal is.
	part.CFrame = CFrame.new(at + Vector3.new(0, PEDESTAL_HEIGHT + MODEL_SIZE / 2, 0))

	-- **No `TextureID`.** A `MeshPart` that names none samples whatever texture
	-- each of its own submeshes recorded at bake time — which is how a
	-- character with six sheets works without this file knowing any of them,
	-- and why the PMX models need no entry here while the glTF ones need no
	-- exception.
	--
	-- The colour is the *untextured* fallback and multiplies with anything
	-- sampled, so it stays near white: a tint here would look like a texture
	-- bug on the models that do have sheets.
	part.Color = Color3.new(0.95, 0.94, 0.92)
	part.Parent = workspace

	part:AddTag("Imported")

	table.insert(turntables, {
		Part = part,

		-- Different rates, so two neighbouring cells are never in step. A grid
		-- turning in unison reads as one object and hides which cell is which.
		Rate = 0.35 + (index % 4) * 0.12,
	})
end

-- **A mirror across the back**, which is worth more here than it looks: it
-- shows every model's far side at the same time as the camera shows the near
-- one, so a face winding the wrong way is visible without waiting for the
-- turntable to come round. It also puts the whole grid through
-- `SurfaceCamera`'s path, so a frame where the grid is right and the
-- reflection is wrong says the fault is in surface rendering and not in
-- content.
local pane = Instance.new("Part")
pane.Name = "BackMirror"
pane.Anchored = true
pane.Size = Vector3.new(PLATE_X, 10, 0.3)
pane.CFrame = CFrame.new(Vector3.new(0, 5, -PLATE_Z / 2))
pane.Color = Color3.new(0.92, 0.95, 1.0)
pane.Parent = workspace

local reflection = Instance.new("SurfaceCamera")
reflection.Name = "BackMirrorCamera"
reflection.SurfaceSize = Vector3.new(1024, 1024, 0)

-- `Back` is +Z, so a pane at -Z looks forward across the grid. The enum rather
-- than the string it accepts, because only the enum typechecks.
reflection.Face = Enum.NormalId.Back
reflection.Parent = pane

local view = Instance.new("Camera")
view.Name = "Eye"

-- **Aimed with `lookAt` rather than placed with `new`.** A bare `CFrame.new`
-- carries no rotation, so a camera lifted above the grid still stares
-- horizontally and the models sit in the bottom of the frame. The target is the
-- middle of the grid at model height, and both ends are derived from the layout
-- so adding a row keeps the framing.
-- **High enough to see past the front row.** At eye height the nearest models
-- fill the frame and the back row hides behind them, which reads as a grid that
-- only half loaded. Looking down the rows separates them, and the target sits
-- low and slightly back so the whole plate stays in shot.
view.CFrame = CFrame.lookAt(
	Vector3.new(0, PLATE_Z * 0.40, PLATE_Z * 0.78),
	Vector3.new(0, MODEL_SIZE * 0.35, -SPACING * 0.5)
)
view.FieldOfView = 60
view.Parent = workspace

-- **Parenting a camera is not aiming with it, and forgetting this line is
-- invisible.** A world holds as many cameras as it likes and `ActiveCamera`
-- names the live one, so a `Camera` that is only parented is a row nothing
-- reads. The client then keeps its own placeholder and orbits it — which
-- renders a perfectly good picture from the wrong place, so the scene looks
-- built and merely badly framed rather than broken.
workspace.CurrentCamera = view

RunService.Heartbeat:Connect(function(deltaTime: number)
	for _, turntable in turntables do
		-- Composed onto the existing frame rather than recomputed from an
		-- accumulated angle, which keeps the position the layout gave it and
		-- stays a unit rotation without anything normalising it.
		turntable.Part.CFrame *= CFrame.Angles(0, turntable.Rate * deltaTime, 0)
	end
end)

print(`mesh grid: {#MODELS} model(s) in a {COLUMNS}x{ROWS} grid, turning`)

-- **What each cell actually got, once the content has landed.** `MeshId` is a
-- string this file asks for and `TrianglesCount` is what the world found behind
-- it, so the two printed together are the whole of "did the pipeline work":
-- a real number means the mesh arrived, and zero means this part is drawing the
-- fallback cube because nothing published that name.
--
-- **Waits for the answer rather than reading it once.** Content arrives over a
-- network, so a count read on the first frame is a count read before the
-- manifest has been verified — every cell would report zero and the report
-- would be about timing rather than about content. This settles when every cell
-- has resolved, and gives up after `REPORT_TIMEOUT` so a run with no `--cdn`
-- still says what it found instead of staying silent.
local REPORT_TIMEOUT = 10

local reported = false
local waited = 0

RunService.Heartbeat:Connect(function(deltaTime: number)
	if reported then
		return
	end
	waited += deltaTime

	local total = 0
	local missing = 0
	for _, turntable in turntables do
		local count = turntable.Part.TrianglesCount
		total += count
		if count == 0 then
			missing += 1
		end
	end

	if missing > 0 and waited < REPORT_TIMEOUT then
		return
	end
	reported = true

	for _, turntable in turntables do
		print(`  {turntable.Part.Name}: {turntable.Part.TrianglesCount} triangle(s)`)
	end
	print(`mesh grid: {total} triangle(s) across {#turntables} model(s), {missing} unresolved`)
end)
