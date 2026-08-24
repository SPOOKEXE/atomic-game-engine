--!strict
-- Two facing mirrors and a moving cube, for watching recursive history update.
--
-- The renderer resolves the stated same-frame levels deepest first. At the
-- recursion bound it samples the completed surface image from the prior frame,
-- which keeps the corridor continuing without an unbounded render graph. The
-- cube moves across both direct and historical levels, making a frozen or
-- incorrectly ordered history image visible immediately.

local RunService = game:GetService("RunService")

workspace.SurfaceBounces = 3
workspace.MaxSurfaces = 2

local floor = Instance.new("Part")
floor.Name = "Floor"
floor.Anchored = true
floor.Size = Vector3.new(44, 1, 38)
floor.Position = Vector3.new(0, -0.49, 0)
floor.Color = Color3.fromRGB(42, 45, 54)
floor.Parent = workspace

local function mirror(name: string, frame: CFrame)
	local pane = Instance.new("Part")
	pane.Name = name
	pane.Anchored = true
	pane.Size = Vector3.new(14, 9, 0.35)
	pane.CFrame = frame
	pane.Color = Color3.fromRGB(220, 228, 240)
	pane.Parent = workspace

	local camera = Instance.new("SurfaceCamera")
	camera.Name = name .. "Camera"
	camera.SurfaceSize = Vector3.new(512, 512, 0)
	camera.Face = Enum.NormalId.Front
	camera.FPS = 60
	camera.Parent = pane
end

mirror("NorthMirror", CFrame.new(0, 4.5, -14) * CFrame.Angles(0, math.rad(180), 0))
mirror("SouthMirror", CFrame.new(0, 4.5, 14))

local cube = Instance.new("Part")
cube.Name = "RecursiveCube"
cube.Anchored = true
cube.Size = Vector3.new(3, 3, 3)
cube.Color = Color3.fromRGB(238, 88, 58)
cube.Parent = workspace

for index = 1, 5 do
	local marker = Instance.new("Part")
	marker.Name = `DepthMarker_{index}`
	marker.Anchored = true
	marker.Size = Vector3.new(0.8, index, 0.8)
	marker.Position = Vector3.new(-6, index * 0.5, -8 + index * 3)
	marker.Color = Color3.fromRGB(50 + index * 30, 190 - index * 18, 110 + index * 20)
	marker.Parent = workspace
end

local phase = 0
RunService.Heartbeat:Connect(function(deltaTime: number)
	phase += deltaTime
	local x = math.sin(phase * 1.4) * 5
	local z = math.cos(phase * 0.9) * 4
	cube.CFrame = CFrame.new(x, 2.2, z) * CFrame.Angles(phase * 0.3, phase, 0)
end)

local view = Instance.new("Camera")
view.Name = "RecursiveMirrorCamera"
view.CFrame = CFrame.lookAt(Vector3.new(7, 6, 7), Vector3.new(0, 3.5, -14))
view.Parent = workspace
workspace.CurrentCamera = view

print("recursivemirrors: three same-frame levels, then last-frame history")
