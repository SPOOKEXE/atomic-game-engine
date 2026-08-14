--!strict
-- Three mirrors in a chain, seen from a camera turned away from the first.
--
-- **The scene the surface-visibility fix was measured on.** `Mirrors-1-world`
-- looks straight at its panes, which is the case that was never broken: a pane
-- directly on screen reveals whatever it can see for free, so the chain is only
-- ever one grant deep and one round of the grant sweep is enough.
--
-- Turning the camera is what makes it two. Once pane A leaves the viewer's
-- frustum, everything A was revealing drops a level - and until v0.15 the sweep
-- followed exactly one, while the surface pass drew `SetSurfaceBounces` of them.
-- The levels past the first were culled rather than late, which is why the
-- symptom was an angle rather than a scene.
--
-- A is behind the viewer's shoulder, B faces it, C faces B. At `YAW` degrees off
-- centre, A is off screen, so C is two grants away and needs two rounds.
--
--     client --script MirrorDepth.luau --frames 240 --capture out.bmp
--
-- Compare a run with `--surface-bounces 1` against what this scene asks for:
-- the chain's far end is what changes.

-- Far enough off centre that pane A leaves the frustum. Sixty rather than the
-- fifty the symptom was first reported at, so the case is unambiguous rather
-- than marginal.
local YAW = 60

local SURFACE_SIZE = Vector3.new(512, 512, 0)

-- **The world states its own depth, and a measurement scene is exactly what
-- should.** Left alone the renderer works the depth out from the frame before,
-- which means the first frames of a run are drawn at one level, then two - so a
-- capture taken early is a capture of the ramp rather than of the scene, and two
-- builds that ramp differently differ for a reason that has nothing to do with
-- the change under test. Three is the length of A -> B -> C; stating it makes
-- every frame of every run the same depth.
--
-- It measures out at what the automatic rule settles on here, which is the
-- answer that gives confidence in both: what this buys is that it is the depth
-- from frame one rather than from frame three.
--
-- `MirrorCorridor.luau` deliberately says nothing, so the two scenes cover both
-- halves. `--surface-bounces N` overrides this from outside.
workspace.SurfaceBounces = 3

local floor = Instance.new("Part")
floor.Name = "Floor"
floor.Anchored = true
floor.Size = Vector3.new(120, 1, 120)
floor.CFrame = CFrame.new(Vector3.new(0, -0.5, 0))
floor.Color = Color3.new(0.20, 0.20, 0.23)
floor.Parent = workspace

-- Something with structure to see in the reflections. A flat colour would make
-- a culled bounce and a working one look the same.
for index = 1, 12 do
	local marker = Instance.new("Part")
	marker.Name = "Marker" .. index
	marker.Anchored = true
	marker.Size = Vector3.new(1.5, 1.5 + index * 0.4, 1.5)
	marker.CFrame = CFrame.new(Vector3.new(-14 + index * 2.5, 1 + index * 0.2, -4))
	marker.Color = Color3.new(0.2 + index * 0.06, 0.9 - index * 0.05, 0.35)
	marker.Parent = workspace
end

-- A pane and the camera that makes it a mirror. `Front` is -Z, so a pane looks
-- back along its own -Z.
local function mirror(name: string, frame: CFrame): Part
	local pane = Instance.new("Part")
	pane.Name = name
	pane.Anchored = true
	pane.Size = Vector3.new(14, 9, 0.4)
	pane.CFrame = frame
	pane.Color = Color3.new(0.85, 0.9, 1.0)
	pane.Parent = workspace

	local eye = Instance.new("SurfaceCamera")
	eye.Name = name .. "Camera"
	eye.SurfaceSize = SURFACE_SIZE
	eye.Face = Enum.NormalId.Front
	eye.Parent = pane

	return pane
end

-- A sits beside the viewer facing across the room; B faces A; C faces B. The
-- chain is A sees B sees C.
mirror("PaneA", CFrame.new(Vector3.new(-22, 4.5, 6)) * CFrame.Angles(0, math.rad(70), 0))
mirror("PaneB", CFrame.new(Vector3.new(16, 4.5, -2)) * CFrame.Angles(0, math.rad(-110), 0))
mirror("PaneC", CFrame.new(Vector3.new(2, 4.5, 26)) * CFrame.Angles(0, math.rad(180), 0))

local view = Instance.new("Camera")
view.Name = "TurnedCamera"

-- **Turned rather than moved.** The whole point is that the eye stays where it
-- was and only its facing changes, because that is what the report described
-- and what the frustum test is sensitive to.
view.CFrame = CFrame.new(Vector3.new(0, 6, 18)) * CFrame.Angles(0, math.rad(YAW), 0)
view.Parent = workspace
workspace.CurrentCamera = view

print(`mirrordepth: camera turned {YAW} degrees, three panes chained A -> B -> C`)
