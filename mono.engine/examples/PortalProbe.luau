-- The smallest scene that shows every way a portal can be wrong.
--
-- **A reproduction rather than a demonstration**, which is why it is ugly and
-- why nothing in it moves. `Portals-1-world.luau` is the scene that shows the
-- feature working; this one exists so a specific complaint can be looked at:
-- that a hole does not draw the far side when you stand on the entry side, and
-- that a body straddling the seam is not drawn in both rooms and in the
-- picture.
--
-- The claim being tested is the strong one - **a portal is a wormhole and the
-- far room should render as if it were simply there**. So every check here is
-- about what a still frame shows, and each object is placed to make one failure
-- visually unmistakable rather than to look like anything.
--
-- ## What is in it, and what each thing is for
--
--   * A black baseplate under both rooms, so anything the portal draws stands
--     out against it rather than against a lit floor.
--   * **A quarter-turn pair.** Room B is down +X and rotated ninety degrees, so
--     the map is a rotation and not a translation - a hole that only works when
--     the two frames are parallel passes a straight-through pair and fails this
--     one. That is the case `NON-EUCLIDEAN.md` records as the authoring
--     mistake nothing in the engine can catch.
--   * **A floating block on each side, in different colours.** Orange in room A,
--     cyan in room B. Standing in A looking at the pane, the cyan block is what
--     has to appear in the hole. Seeing orange means the pane is a mirror;
--     seeing nothing means it is a wall.
--   * **A tall block halfway through the seam.** It has to be drawn in room A,
--     in room B, *and* inside the picture the pane shows - three places, and
--     the artefact this repository has chased twice is any one of them missing.
--   * **Two cameras' worth of viewpoints, taken as stills.** `PROBE_VIEW`
--     selects which, so one scene answers "what does the entry side look like"
--     and "what does the exit side look like" without authoring two files.

local WORLD = workspace

-- Black, so anything drawn on it is the thing being tested and not the floor.
local function baseplate(name: string, at: Vector3, span: number)
	local slab = Instance.new("Part")
	slab.Name = name
	slab.Size = Vector3.new(span, 1, span)
	slab.CFrame = CFrame.new(at)
	slab.Color = Color3.new(0.02, 0.02, 0.03)
	slab.Anchored = true
	slab.Parent = WORLD
	return slab
end

local function block(name: string, at: CFrame, size: Vector3, colour: Color3)
	local part = Instance.new("Part")
	part.Name = name
	part.Size = size
	part.CFrame = at
	part.Color = colour
	part.Anchored = true
	part.Parent = WORLD
	return part
end

-- The pane itself: thin, because a hole is its face and not its box, and a
-- thick one puts "inside the pane" somewhere a body can stand.
local function pane(name: string, at: CFrame): Part
	return block(name, at, Vector3.new(8, 6, 0.25), Color3.new(0.6, 0.6, 0.65))
end

-- **`Front` on both, and the slab is thin along the face's own axis.** That is
-- the part this scene got wrong first and it is worth writing down: a pane's
-- hole is one *face* of its box, so `Face = Right` wants a slab thin in X and
-- `Face = Front` one thin in Z. Author it the other way and the rectangle the
-- camera is fitted to is the slab's thin edge - a sliver - which draws a hole
-- the width of a pencil and looks exactly like a portal that does not work.
--
-- `Face` is resolved on the *destination* as well as the source, so the pair
-- works when each pane's own front points into the room it is seen from. Room B
-- is turned a quarter, which is what makes going through a corner.
local function link(from: Part, to: Part, name: string)
	local portal = Instance.new("Portal")
	portal.Name = name
	portal.SurfaceSize = Vector3.new(1024, 768)
	portal.Face = Enum.NormalId.Front
	portal.Destination = to
	portal.ImageTransparency = 0
	portal.Parent = from
end

baseplate("GroundA", Vector3.new(0, 0, 0), 60)
baseplate("GroundB", Vector3.new(100, 0, 0), 60)

-- The pair. A is unrotated at the origin; B is a hundred out and turned a
-- quarter, so going through is a ninety-degree corner.
local paneA = pane("PaneA", CFrame.new(Vector3.new(0, 3.5, 0)))
-- **Turned minus a quarter, and the sign is the whole of the authoring rule.**
-- `Front` is local -Z, and rotating that by +90 degrees about Y gives -X - which
-- points back down the corridor at room A, so the hole showed room A from a
-- hundred studs away and looked for all the world like a portal that does not
-- work. Minus a quarter puts B's front at +X, into its own room. This is the
-- mistake `NON-EUCLIDEAN.md` says the engine cannot catch, made here on
-- purpose so the scene records it.
local paneB = pane("PaneB", CFrame.new(Vector3.new(100, 3.5, 0)) * CFrame.Angles(0, -math.pi / 2, 0))

link(paneA, paneB, "AtoB")
link(paneB, paneA, "BtoA")

-- **One block per room, in colours nothing else uses.** Orange lives in room A
-- and cyan in room B, so what the hole shows names which room it is showing.
block("MarkerA", CFrame.new(Vector3.new(-5, 5, 8)), Vector3.new(2, 2, 2), Color3.new(1.0, 0.45, 0.05))
block("MarkerB", CFrame.new(Vector3.new(108, 5, 0)), Vector3.new(2, 2, 2), Color3.new(0.1, 0.8, 1.0))

-- **Halfway through the seam, and deliberately taller than it is wide** so that
-- a copy drawn at the wrong size or the wrong depth is obvious rather than
-- arguable. `scene::AppendPortalClones` is what has to put its far half in room
-- B, and the surface or portal pass is what has to put that half in the picture.
block("InTheSeam", CFrame.new(Vector3.new(1.5, 3.0, -0.125)), Vector3.new(1.5, 4, 1.5), Color3.new(0.95, 0.9, 0.1))

-- Where it is watched from.
--
-- **Two stills rather than a moving camera**, because a still is what a bug
-- report contains. `PROBE_VIEW=exit` looks back through B; anything else looks
-- at A from room A, which is the entry side the complaint is about.
local view = Instance.new("Camera")
view.Name = "Viewer"
view.FieldOfView = 70

-- **A line this file rewrites rather than a global.** `_G` is readonly in the
-- script sandbox, so nothing outside a scene can set one - the capture scripts
-- copy the file and substitute the line below, which is the only mechanism that
-- actually works from a shell.
local VIEW = ""
if VIEW == "exit" then
	view.CFrame = CFrame.new(Vector3.new(114, 5, 0)) * CFrame.Angles(0, math.pi / 2, 0)
else
	view.CFrame = CFrame.new(Vector3.new(0, 5, -14)) * CFrame.Angles(0, math.pi, 0)
end

view.Parent = WORLD
WORLD.CurrentCamera = view

print("portal probe: A at origin, B at +100 turned a quarter; orange in A, cyan in B, yellow in the seam")
