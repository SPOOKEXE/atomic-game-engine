-- Every concrete 3D adornment, arranged so shape and offset errors are visible.

local function target(name: string, position: Vector3): Part
	local part = Instance.new("Part")
	part.Name = name
	part.Anchored = true
	part.Size = Vector3.new(3, 2, 2)
	part.Position = position
	part.Parent = workspace
	return part
end

local function adorn(decoration: any, part: Part, colour: Color3): any
	decoration.Adornee = part
	decoration.Color3 = colour
	decoration.AlwaysOnTop = true
	decoration.Parent = part
	return decoration
end

local blue = Color3.new(0.1, 0.65, 1)
local orange = Color3.new(1, 0.45, 0.1)
local green = Color3.new(0.2, 1, 0.45)

local selectionBox = adorn(Instance.new("SelectionBox"), target("SelectionBoxTarget", Vector3.new(-8, 5, 0)), blue)
selectionBox.LineThickness = 0.08
selectionBox.SurfaceColor3 = blue
selectionBox.SurfaceTransparency = 0.8

local selectionSphere = adorn(Instance.new("SelectionSphere"), target("SelectionSphereTarget", Vector3.new(-4, 5, 0)), orange)
selectionSphere.LineThickness = 0.06

local box = adorn(Instance.new("BoxHandleAdornment"), target("BoxHandleTarget", Vector3.new(0, 5, 0)), green)
box.Size = Vector3.new(1.5, 1, 0.75)
box.SizeRelativeOffset = Vector3.new(0, 1, 0)

local sphere = adorn(Instance.new("SphereHandleAdornment"), target("SphereHandleTarget", Vector3.new(4, 5, 0)), blue)
sphere.Radius = 0.7
sphere.SizeRelativeOffset = Vector3.new(0, 1, 0)

local cylinder = adorn(Instance.new("CylinderHandleAdornment"), target("CylinderHandleTarget", Vector3.new(8, 5, 0)), orange)
cylinder.Radius = 0.65
cylinder.InnerRadius = 0.25
cylinder.Height = 1.5
cylinder.Angle = 270
cylinder.SizeRelativeOffset = Vector3.new(0, 1, 0)

local line = adorn(Instance.new("LineHandleAdornment"), target("LineHandleTarget", Vector3.new(-6, 1, 0)), green)
line.Length = 2
line.Thickness = 0.08
line.SizeRelativeOffset = Vector3.new(0, 1, 0)

local cone = adorn(Instance.new("ConeHandleAdornment"), target("ConeHandleTarget", Vector3.new(-2, 1, 0)), blue)
cone.Height = 1.8
cone.Radius = 0.7
cone.Hollow = false
cone.SizeRelativeOffset = Vector3.new(0, 1, 0)

local handles = adorn(Instance.new("Handles"), target("HandlesTarget", Vector3.new(2, 1, 0)), orange)
handles.Faces = 63

local arcs = adorn(Instance.new("ArcHandles"), target("ArcHandlesTarget", Vector3.new(6, 1, 0)), green)
arcs.Axes = 7

print("adornments: selection, shape handles, face handles and arc handles created")
