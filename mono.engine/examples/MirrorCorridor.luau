--!strict
-- Two mirrors facing each other, which is the case that says whether a mirror
-- pass is a recursion or an iteration.
--
-- **The picture is the measurement.** A pane facing a pane shows that pane,
-- which shows the first one again, forever — so the number of nested rectangles
-- visible in the frame *is* the depth the renderer resolved. One rectangle is
-- one bounce. Two is two. A pass that draws the second rectangle flat has told
-- you exactly where it stopped, and no instrumentation is needed to read it.
--
-- **Why `MirrorDepth.luau` is not this scene.** That one turns the camera away
-- from the first pane to prove that *visibility* follows the chain, which is a
-- culling question. This one looks straight down the corridor, so nothing is
-- ever off screen and culling cannot be the explanation for anything missing.
-- What is left is the viewpoint: a surface camera is placed from the eye, so
-- the pane drawn inside another pane's picture is projected with a matrix taken
-- from the eye rather than from the camera that picture was rendered from. The
-- coordinate leaves the texture's 0..1 rectangle and `opaque.frag` falls back
-- to the plain lit pane — which is what the flat slab in the reflection is.
--
--     client --script MirrorCorridor.luau --frames 90 --capture out.bmp
--
-- The scene is static, so two captures of one build are byte-identical and any
-- difference between two builds is the change under test.

-- How far apart the two panes stand. Wide enough that each nesting is several
-- pixels across at 1280x720, which is what makes counting them possible.
local SEPARATION = 26

local floor = Instance.new("Part")
floor.Name = "Floor"
floor.Anchored = true
floor.Size = Vector3.new(60, 1, 60)
floor.CFrame = CFrame.new(Vector3.new(0, -0.5, 0))
floor.Color = Color3.new(0.20, 0.20, 0.23)
floor.Parent = workspace

-- **Coloured markers between the panes, and they are not decoration.** A
-- corridor of empty rectangles nested inside each other looks the same whether
-- the renderer resolved two levels or drew one and cleared the rest. Something
-- with structure in the gap means each nesting carries a recognisable picture,
-- so a level that is present and a level that is blank are told apart by eye as
-- well as by the variance measurement.
--
-- Off to one side, so they do not stand between the two panes and occlude the
-- corridor they are there to fill.
for index = 1, 6 do
	local marker = Instance.new("Part")
	marker.Name = "Marker" .. index
	marker.Anchored = true
	marker.Size = Vector3.new(1.2, 2 + index * 0.5, 1.2)
	marker.CFrame = CFrame.new(Vector3.new(-7, 1 + index * 0.25, -9 + index * 3.5))
	marker.Color = Color3.new(0.15 + index * 0.13, 0.85 - index * 0.09, 0.4)
	marker.Parent = workspace
end

-- A pane and the camera that makes it a mirror. `Front` is -Z, so a pane
-- reflects along its own -Z.
local function mirror(name: string, frame: CFrame): Part
	local pane = Instance.new("Part")
	pane.Name = name
	pane.Anchored = true
	pane.Size = Vector3.new(12, 8, 0.4)
	pane.CFrame = frame
	pane.Color = Color3.new(0.9, 0.92, 0.95)
	pane.Parent = workspace

	local eye = Instance.new("SurfaceCamera")
	eye.Name = name .. "Camera"
	eye.SurfaceSize = Vector3.new(1024, 1024, 0)
	eye.Face = Enum.NormalId.Front
	eye.Parent = pane

	return pane
end

-- Near pane faces down -Z; far pane is turned to face back up +Z. Both stand on
-- the floor at the same height, so the corridor is level and each nesting sits
-- inside the last rather than sliding off the edge.
mirror("PaneNear", CFrame.new(Vector3.new(0, 4, SEPARATION / 2)))
mirror("PaneFar", CFrame.new(Vector3.new(0, 4, -SEPARATION / 2)) * CFrame.Angles(0, math.rad(180), 0))

-- **Inside the corridor and off to one side**, which is two decisions and both
-- matter. Inside, because each pane reflects only what is in front of its own
-- face and a viewer standing behind the near pane sees its back rather than the
-- corridor. Off to one side, because a camera on the centre line puts its own
-- reflection dead centre in every nesting, and the panes then occlude the very
-- depth being counted.
--
-- **Off to one side but still within the panes' own width**, and the first
-- version of this scene got that wrong in a way worth recording. A surface
-- camera's frustum is fitted to *its own pane* and to nothing else, so from
-- outside the corridor the near pane's frustum leans hard away from the axis and
-- the far pane falls fifty studs outside it — correctly, since it genuinely does
-- not appear in that reflection. The chain then telescopes one way only, which
-- looks exactly like a recursion that stops after one level and is not one.
--
-- Inside the width, each pane's frustum covers the other and the corridor
-- telescopes both ways, which is what a depth measurement needs.
local view = Instance.new("Camera")
view.Name = "CorridorCamera"
view.CFrame = CFrame.lookAt(Vector3.new(4, 5, 6), Vector3.new(0, 4, -SEPARATION / 2))
view.Parent = workspace
workspace.CurrentCamera = view

print("mirrorcorridor: two panes facing across " .. SEPARATION .. " studs")
