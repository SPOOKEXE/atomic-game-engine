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
-- With no `--cdn` the scene runs and stands empty, and says so on the pedestal
-- row. That is the honest degraded state.
--
-- ## Why this file no longer names any mesh
--
-- **It used to hold nine string literals, and on any store but the one it was
-- written against every single cell drew a cube.** Nothing warned: an
-- unregistered mesh falls back to a cube, which is also what a mesh that has
-- not streamed in yet looks like — so a scene that had never worked and a scene
-- that was still loading were the same picture.
--
-- The names were never this file's to know. A publisher decides them, and
-- `ContentService:GetMeshes()` reports the ones that actually arrived in this
-- world — so what the grid shows is exactly what loaded, which is the thing the
-- demo is *for*. A cell on screen is proof; a cell that is missing is content
-- that is not here, and there is no third case where a name was misspelt.
--
-- **Built as content arrives rather than at startup.** Delivery is asynchronous
-- — a fetch spans frames and `AGENTS.md` rule 5 forbids it becoming visible
-- mid-tick — so at the moment this chunk runs the catalogue is empty and stays
-- empty for a while. The grid therefore grows on `Heartbeat` as meshes land.

local RunService = game:GetService("RunService")

local COLUMNS = 3
local SPACING = 7

-- How tall a model stands, in metres. **This is metres only because the bake
-- was `--model-size 1`** — see the header. `assetc` fitted every import's
-- longest axis to one and centred it, so this number scales a unit mesh the
-- same way it scales a built-in cube, and a 2M-triangle wizard ends up the same
-- height as a low-poly fox.
local MODEL_SIZE = 4

local PEDESTAL_HEIGHT = 0.6

-- **How many cells the plate and the mirror are built for, rather than how many
-- there are.** The grid grows as content arrives, and a floor that resized every
-- time a mesh landed would be a scene that visibly rebuilds itself. Twelve is
-- three full rows, which is more than this repository's seed content has.
local CELLS = 12
local ROWS = math.ceil(CELLS / COLUMNS)

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

-- What is on the plate already, keyed by mesh name, so a mesh that arrives
-- twice is not built twice.
local placed: { [string]: { Part: MeshPart, Pedestal: Part } } = {}
local placedCount = 0

-- A label for a cell, from the name the publisher gave it.
--
-- **The file name and its extension**, because that is the only thing this
-- knows about a mesh — and it is genuinely useful: `.amesh` is baked geometry
-- and anything else is a source format that got published raw, which is a
-- content-pipeline mistake worth being able to see from the scene.
local function labelFor(mesh: string): string
	local slash = string.find(mesh, "/[^/]*$")
	if slash then
		return string.sub(mesh, slash + 1)
	end
	return mesh
end

-- Builds one cell. Position is applied separately, because a cell's place
-- depends on how many others exist and that changes as content lands.
local function buildCell(mesh: string)
	local pedestal = Instance.new("Part")
	pedestal.Name = `Pedestal_{mesh}`
	pedestal.Anchored = true
	pedestal.Size = Vector3.new(SPACING * 0.7, PEDESTAL_HEIGHT, SPACING * 0.7)
	pedestal.Parent = workspace

	local part = Instance.new("MeshPart")
	part.Name = labelFor(mesh)
	part.Anchored = true

	-- **The one string that has to match, and it came from the engine.** This
	-- is a name `ContentService` reported, so it is by construction a mesh this
	-- world holds — which is the whole difference between this file and the
	-- version that carried literals.
	part.MeshId = mesh

	part.Size = Vector3.new(MODEL_SIZE, MODEL_SIZE, MODEL_SIZE)

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

	placed[mesh] = { Part = part, Pedestal = pedestal }

	table.insert(turntables, {
		Part = part,

		-- Different rates, so two neighbouring cells are never in step. A grid
		-- turning in unison reads as one object and hides which cell is which.
		Rate = 0.35 + (placedCount % 4) * 0.12,
	})
end

-- Puts every cell where its place in the sorted list says.
--
-- **Re-applied whenever the set grows, and that is deliberate.**
-- `ContentService` sorts, so the final layout is a function of the names alone
-- and is the same on every run — but a mesh arriving mid-load may sort *before*
-- ones already on the plate, so the cells shuffle while content streams and
-- settle once it stops. The alternative, placing by arrival order, would be
-- stable on screen and different every run.
local function layout(names: { string })
	for index, mesh in ipairs(names) do
		local cell = placed[mesh]
		if cell then
			local at = cellPosition(index)
			cell.Pedestal.CFrame = CFrame.new(at + Vector3.new(0, PEDESTAL_HEIGHT / 2, 0))
			cell.Part.CFrame = CFrame.new(at + Vector3.new(0, PEDESTAL_HEIGHT + MODEL_SIZE / 2, 0))
		end
	end
end

-- **A sign, so an empty plate is a sentence rather than a mystery.** With no
-- `--cdn` this scene has nothing to show, and a bare floor gives no clue
-- whether the scene failed or the content did.
local sign = Instance.new("Part")
sign.Name = "Status"
sign.Anchored = true
sign.Size = Vector3.new(SPACING * 1.6, 1.2, 0.2)
sign.CFrame = CFrame.new(Vector3.new(0, 1.2, PLATE_Z / 2 - 1))
sign.Color = Color3.new(0.18, 0.2, 0.24)
sign.Parent = workspace

local label = Instance.new("SurfaceGui")
label.Face = Enum.NormalId.Front
label.Parent = sign

local text = Instance.new("TextLabel")
text.Size = UDim2.new(1, 0, 1, 0)
text.BackgroundTransparency = 1
text.TextColor3 = Color3.new(0.85, 0.87, 0.92)
text.Text = "waiting for content..."
text.Parent = label

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

-- **The grid grows here, and this is the whole change.** Content is fetched
-- across frames, so the catalogue is empty when this chunk runs and fills as
-- bundles verify. Polling it is the honest shape: there is no callback a fetch
-- could fire without becoming visible mid-tick, which is exactly what rule 5
-- forbids.
--
-- Cheap when nothing changes: `GetMeshes` builds a sorted list of what this
-- world holds, and the common case is that its length has not moved.
local known = 0

RunService.Heartbeat:Connect(function()
	local meshes = ContentService:GetMeshes()
	if #meshes == known then
		return
	end
	known = #meshes

	for index, mesh in ipairs(meshes) do
		if index > CELLS then
			-- **Said out loud rather than silently dropped.** A store with more
			-- meshes than the plate has cells is an ordinary thing to have, and
			-- a grid that quietly showed the first twelve would read as content
			-- that failed to arrive.
			print(`mesh grid: {#meshes - CELLS} more mesh(es) than the {CELLS}-cell plate holds`)
			break
		end
		if not placed[mesh] then
			buildCell(mesh)
			placedCount += 1
		end
	end

	layout(meshes)

	text.Text = if placedCount == 0
		then "no meshes in this world — run with --cdn"
		else `{placedCount} mesh(es)`

	print(`mesh grid: {placedCount} mesh(es) placed`)
end)

RunService.Heartbeat:Connect(function(deltaTime: number)
	for _, turntable in turntables do
		-- Composed onto the existing frame rather than recomputed from an
		-- accumulated angle, which keeps the position the layout gave it and
		-- stays a unit rotation without anything normalising it.
		turntable.Part.CFrame *= CFrame.Angles(0, turntable.Rate * deltaTime, 0)
	end
end)

print(`mesh grid: waiting for content, {COLUMNS}x{ROWS} plate ready`)

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

-- How long the cell count has to hold still before the report is believed.
--
-- **A bundle arrives all at once and the next one may not have started**, so a
-- gap between two arrivals is ordinary and a report that fired in one would be
-- describing half the content.
local SETTLE_SECONDS = 2

local reported = false
local waited = 0
local counted = -1
local settled = 0

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

	-- **Nothing missing is no longer enough to report, and that is a bug this
	-- change introduced.** The grid used to hold every cell from the first
	-- frame, so "none are missing" meant "all of them arrived". Now the cells
	-- appear as content does — so on the frame the built-in meshes resolve,
	-- nothing is missing because nothing else exists yet, and the report fired
	-- before a single import had landed. It said `0 unresolved` and was
	-- describing six primitives.
	--
	-- So the report waits for the *set* to stop growing as well. `settled` is
	-- how long the count has been unchanged; content arriving resets it.
	if #turntables ~= counted then
		counted = #turntables
		settled = 0
		return
	end
	settled += deltaTime

	if (missing > 0 or settled < SETTLE_SECONDS) and waited < REPORT_TIMEOUT then
		return
	end
	reported = true

	for _, turntable in turntables do
		print(`  {turntable.Part.Name}: {turntable.Part.TrianglesCount} triangle(s)`)
	end
	print(`mesh grid: {total} triangle(s) across {#turntables} model(s), {missing} unresolved`)
end)
