--!strict
-- Every imported mesh at once, on turntables, in a grid.
--
-- `Meshes.luau` proves one glTF and one PMX arrive and draw. This is the wider
-- check: every mesh the store published *and* every mesh the engine ships, laid
-- out so a single frame says which of them made it. **A grid is the point** - a
-- mesh that failed to resolve draws as `MeshTable`'s fallback cube, so a cube
-- standing in a cell where a character should be is a name that did not match,
-- and it is obvious at a glance rather than after a log hunt.
--
-- Each model turns on its own pedestal, because a still frame hides the half of
-- a mesh facing away - and a reversed winding is exactly the bug that looks
-- fine from the front.
--
-- ## What it is checking
--
-- The plate mixes two sources on purpose, and the mix is what makes one frame
-- diagnostic rather than decorative:
--
--   * **The engine's own six** - `engine.Cube` and its siblings, generated in
--     process by `assets::MakeBuiltin`. They cannot fail to resolve, so a cell
--     of theirs drawing wrong is a renderer fault and never a content one. They
--     are the control.
--   * **Whatever the store published**, which on this repository is a glTF fox
--     and six MikuMikuDance characters - PMX, with real texture sheets,
--     non-ASCII file names and per-submesh materials. If those draw textured
--     then `bake::ReadPmx`, the UTF-16 path decoding, the texture rewriting in
--     `assetc`, the manifest, the signature, the bundle fetch and `TextureTable`
--     are all working at once. That is a lot of pipeline riding on one glance.
--
-- **The mirror across the back is doing the same job from the other side.** It
-- shows every model's far face at the same time as the camera shows the near
-- one, so a winding fault is visible without waiting for a turntable to come
-- round - and it puts the whole grid through `SurfaceCamera`'s path, so a frame
-- where the grid is right and the reflection is wrong says the fault is in
-- surface rendering rather than in content.
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
-- ## Which meshes it names, and which it discovers
--
-- **It used to hold nine string literals, and on any store but the one it was
-- written against every single cell drew a cube.** Nothing warned: an
-- unregistered mesh falls back to a cube, which is also what a mesh that has
-- not streamed in yet looks like - so a scene that had never worked and a scene
-- that was still loading were the same picture.
--
-- Then it named nothing at all and took whatever `ContentService:GetMeshes()`
-- reported. That fixed the lie and introduced a different one: with no store,
-- the grid was an empty plate and a sign saying so, which is the correct message
-- and a useless *default scene*. A template that shows nothing until somebody
-- has run a four-minute content pipeline is a template nobody sees working.
--
-- **So it does both, and the split is the whole idea an id is for.**
--
--   * `BUILTIN` below is six ids the engine resolves with no store, no manifest
--     and no delivery client - `assets::MakeBuiltin` generates them and
--     `MeshTable::Initialise` registers them before the first frame. Naming them
--     is safe *because* they cannot be missing, which is exactly what the nine
--     literals were not.
--   * everything else comes from `ContentService:GetPublishedMeshes()`, which
--     reports what the *store* published - the signed manifest, verified before
--     anything could be fetched. On this repository's own store those ids are 64
--     hex characters and an extension - the content hash - and a cell drawn from
--     one is proof the whole pipeline ran.
--
-- **Naming one is what fetches it, and that is the point rather than a side
-- effect.** Content arrives because a world names it - `client/ContentDemand.hpp`
-- carries why asking by kind had to stop, and the 6.9 GB it used to pull through
-- one synchronous function. So this reads a few hundred *strings*, picks at most
-- `CELLS` of them, and sets a `MeshId`; the next content pump fetches exactly
-- those. The plate is the demand.
--
-- It used to read `GetMeshes()` instead, which reports what this world has
-- already loaded - and since nothing else in a fresh place names an import, that
-- list was the six built-ins forever. A scene that only reads what arrived can
-- never be the thing that asks.
--
-- A cell on screen is still proof and a missing cell is still content that is
-- not here. What changed is that six cells are always proof of *something*, so
-- an empty plate now means an empty store rather than a scene that might be
-- broken.
--
-- **Built as content arrives rather than at startup.** Delivery is asynchronous
-- - a fetch spans frames and `AGENTS.md` rule 5 forbids it becoming visible
-- mid-tick - so at the moment this chunk runs the catalogue holds the built-ins
-- and nothing else. The grid therefore grows on `Heartbeat` as meshes land.

local RunService = game:GetService("RunService")

-- The ids that need no store.
--
-- **Written out rather than asked for, because there is no service that lists
-- them** - and because a default scene should say in one place what it is made
-- of. These are the same strings `MeshPart.MeshId` takes from the studio's mesh
-- picker, which offers them at the top of the list for this reason.
local BUILTIN: { string } = {
	"engine.Cube",
	"engine.Sphere",
	"engine.Cylinder",
	"engine.Wedge",
	"engine.CornerWedge",
	"engine.Plane",
}

local COLUMNS = 3
local SPACING = 7

-- How tall a model stands, in metres. **This is metres only because the bake
-- was `--model-size 1`** - see the header. `assetc` fitted every import's
-- longest axis to one and centred it, so this number scales a unit mesh the
-- same way it scales a built-in cube, and a 2M-triangle wizard ends up the same
-- height as a low-poly fox.
local MODEL_SIZE = 4

local PEDESTAL_HEIGHT = 0.6

-- **How many cells the plate and the mirror are built for, rather than how many
-- there are.** The grid grows as content arrives, and a floor that resized every
-- time a mesh landed would be a scene that visibly rebuilds itself. Fifteen is
-- five full rows: the six built-ins plus room for nine imports, which covers
-- this repository's own seed content with two cells spare.
local CELLS = 15
local ROWS = math.ceil(CELLS / COLUMNS)

local PLATE_X = COLUMNS * SPACING + SPACING
local PLATE_Z = ROWS * SPACING + SPACING

-- Clear of a baseplate's top face at y = 0: coplanar surfaces do not flicker in
-- this engine, they resolve stably to whichever one the draw order happens to
-- favour. `scripts/demos/zfight-scan.sh` holds the control for that.
local GROUND_LIFT = 0.01

local floor = Instance.new("Part")
floor.Name = "Baseplate"
floor.Anchored = true
floor.Size = Vector3.new(PLATE_X, 0.5, PLATE_Z)
floor.CFrame = CFrame.new(Vector3.new(0, -0.25 + GROUND_LIFT, 0))
floor.Color = Color3.new(0.26, 0.28, 0.32)
floor.Parent = workspace

-- Where a cell's centre sits, centred on the origin rather than growing out of
-- one corner. Rows advance into +Z and the camera stands at +Z, so `MODELS[1]`
-- is the *back* left cell and the last entry is front right - worth knowing
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
-- knows about a mesh - and it is genuinely useful: `.amesh` is baked geometry
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
	-- world holds - which is the whole difference between this file and the
	-- version that carried literals.
	part.MeshId = mesh

	part.Size = Vector3.new(MODEL_SIZE, MODEL_SIZE, MODEL_SIZE)

	-- **Both halves named, and that is the point of this line.** A `MeshPart`
	-- that sets only `MeshId` shows whatever each of its submeshes recorded at
	-- bake time - which works, and which leaves an author no handle on it: the
	-- sheet names live inside the mesh file, so there is nothing to read and
	-- nothing to put back.
	--
	-- `ContentService:GetMeshTextures` is that handle. It answers what the
	-- geometry is wearing, in submesh order, and setting `TextureID` to one of
	-- them makes the pairing explicit - the same picture, said out loud. From
	-- there swapping an outfit is assigning a different name, which is exactly
	-- what `swatches` below does.
	--
	-- **A whole-instance override, not a per-submesh one.** `TextureID` replaces
	-- the sheet for *every* run in the mesh - Roblox's rule and this engine's -
	-- so a twenty-submesh character given one sheet wears that sheet all over.
	-- That is why this takes the first only when there is exactly one: naming
	-- the first of six would be a deliberate mistake dressed up as a default.
	local sheets = ContentService:GetMeshTextures(mesh)
	if #sheets == 1 then
		part.TextureID = sheets[1]
	end

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
-- and is the same on every run - but a mesh arriving mid-load may sort *before*
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

-- **The swap row: one mesh, several sheets.** Whatever the cells above are
-- wearing, this row proves the two halves are independent - every cube here has
-- the same `MeshId` and a different `TextureID`, so the only thing that differs
-- between them is a string. Change that string and you have changed the outfit.
--
-- **Built from what the models brought with them.** The sheets come from
-- `GetMeshTextures` over the meshes already placed, so this row costs no content
-- of its own: it is dressing a cube in the characters' own textures. On a
-- machine with an empty store there is nothing to show and the row is simply
-- absent, which is the honest answer rather than four purple cubes.
local SWATCHES = 4
local swatches: { MeshPart } = {}

local function dressSwatches()
	if #swatches >= SWATCHES then
		return
	end

	-- Gathered fresh each time, because the cells arrive over several frames and
	-- a list taken once would be whatever had landed by the first Heartbeat.
	local sheets: { string } = {}
	for mesh in pairs(placed) do
		for _, sheet in ipairs(ContentService:GetMeshTextures(mesh)) do
			if sheet ~= "" and not table.find(sheets, sheet) then
				table.insert(sheets, sheet)
			end
		end
	end

	-- Sorted, so the row is the same on every run for a given store - the same
	-- rule the plate's own layout follows.
	table.sort(sheets)

	while #swatches < SWATCHES and #swatches < #sheets do
		local slot = #swatches + 1

		local part = Instance.new("MeshPart")
		part.Name = `Swatch_{slot}`
		part.Anchored = true

		-- **The same mesh every time.** If these differed in geometry as well as
		-- in sheet, the row would prove nothing about which half is doing the
		-- work.
		part.MeshId = "engine.Cube"
		part.TextureID = sheets[slot]

		part.Size = Vector3.new(MODEL_SIZE * 0.6, MODEL_SIZE * 0.6, MODEL_SIZE * 0.6)
		part.CFrame = CFrame.new(
			Vector3.new((slot - (SWATCHES + 1) / 2) * SPACING, MODEL_SIZE * 0.4, SPACING * 2.6)
		)
		part.Color = Color3.new(1, 1, 1)
		part.Parent = workspace

		table.insert(swatches, part)

		-- **Said out loud, because the whole row is one claim.** Printing the
		-- pairing is what lets somebody check from the log that a cube is
		-- wearing a character's sheet rather than its own - which is the thing
		-- being demonstrated and is otherwise a judgement about pixels.
		print(`mesh grid: swatch {slot} is engine.Cube wearing {sheets[slot]}`)
	end
end

-- **One part that asks for a sheet nobody published**, and it is here so this
-- scene shows all three of the renderer's texture answers at once. The cells
-- above name no texture and draw as the default white plastic until their own
-- baked sheets land; this one names something that will never come and wears the
-- purple checkerboard from the first frame.
--
-- **The distinction it exists to prove is the timing one.** Until `D00107` the
-- cells wore that same checkerboard for the frames their sheets took to arrive,
-- so a load looked like a plate full of mistakes and a real misspelling was
-- invisible among them. The marker means *nothing is coming* now - so this part
-- is purple immediately, the cells never are, and the difference is visible
-- without reading a log.
--
-- A built-in mesh rather than an imported one, deliberately: the point is the
-- texture, and geometry that also had to arrive would confuse which half is
-- being shown. It is also why this cell needs no content at all - it draws the
-- same on a machine with no `--cdn`.
local absent = Instance.new("MeshPart")
absent.Name = "MissingTexture"
absent.Anchored = true
absent.MeshId = "engine.Cube"
absent.TextureID = "examples/there-is-no-such-sheet.atex"
absent.Size = Vector3.new(MODEL_SIZE, MODEL_SIZE, MODEL_SIZE)
absent.CFrame = CFrame.new(Vector3.new(0, MODEL_SIZE * 0.5 + PEDESTAL_HEIGHT, SPACING * 1.6))
absent.Parent = workspace

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
-- reads. The client then keeps its own placeholder and orbits it - which
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
-- How many of the ids on the plate came from the store rather than the engine.
-- Read by the sign, so "six primitives" and "six primitives and seven
-- characters" are different sentences.
local imported = 0

local known = -1

-- Every id to place, built-ins first and content after.
--
-- **The built-ins lead and are not sorted in with the rest.** `GetMeshes` sorts,
-- so the content half has a layout that is a function of the names alone and is
-- the same on every run - but the six that are always there should be the six
-- leftmost cells whatever a store happens to hold, or adding one asset would
-- reshuffle the part of the plate that never changes.
-- **Imports first, built-ins after, and the order decides what gets dropped.**
-- The plate holds `CELLS`; anything past that is reported and left off. Putting
-- the built-ins last means a full store spends its cells on the models somebody
-- imported and a bare one still shows six primitives - where built-ins-first
-- filled half the plate with cubes and cylinders and pushed the characters off
-- the end, which is a demo of the wrong thing.
--
-- Both halves are deterministic: `GetPublishedMeshes` sorts, and `BUILTIN` is a
-- literal. So the layout is a function of what the store holds and is the same
-- on every run.
local function everyMesh(): { string }
	local ids: { string } = {}

	for _, mesh in ipairs(ContentService:GetPublishedMeshes()) do
		-- **Skipped rather than placed twice.** A built-in is not published and
		-- should not come back from this, but a store is free to hold a file
		-- called anything - and a cell built twice is a mesh that turns in two
		-- places and a plate one short.
		if not table.find(BUILTIN, mesh) then
			table.insert(ids, mesh)
		end
	end

	imported = #ids

	for _, mesh in ipairs(BUILTIN) do
		table.insert(ids, mesh)
	end

	return ids
end

RunService.Heartbeat:Connect(function()
	-- **Outside the early return below.** The swatches depend on the meshes\'
	-- sheets rather than on how many meshes there are, and both arrive over
	-- several frames - so a row built only when the *count* changes is a row
	-- built on one frame and never corrected. It returns immediately once it is
	-- full.
	dressSwatches()


	local meshes = everyMesh()
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

	-- **Three states, not two.** "Only the built-ins" is the ordinary state of a
	-- fresh install and is not a failure; saying "no meshes" there would send
	-- somebody looking for a content bug that is not one, and saying nothing
	-- would leave a plate of primitives looking like a plate of failed imports.
	text.Text = if placedCount == 0
		then "no meshes in this world - run with --cdn"
		elseif imported == 0 then `{placedCount} built-in mesh(es) - publish content for more`
		else `{placedCount} mesh(es), {imported} imported`

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
-- manifest has been verified - every cell would report zero and the report
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
	-- appear as content does - so on the frame the built-in meshes resolve,
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
