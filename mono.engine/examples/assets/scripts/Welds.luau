-- Legacy Weld and direct WeldConstraint assemblies driven by anchored roots.

local function part(name: string, position: Vector3, anchored: boolean, colour: Color3): Part
	local value = Instance.new("Part")
	value.Name = name
	value.Size = Vector3.new(2, 1, 1)
	value.Position = position
	value.Anchored = anchored
	value.Color = colour
	value.Parent = workspace
	return value
end

local legacyRoot = part("LegacyRoot", Vector3.new(-4, 5, 0), true, Color3.new(1, 0.45, 0.1))
local legacyChild = part("LegacyChild", Vector3.new(-1.5, 5, 0), false, Color3.new(1, 0.75, 0.2))
local legacy = Instance.new("Weld")
legacy.Name = "OffsetWeld"
legacy.Part0 = legacyRoot
legacy.Part1 = legacyChild
legacy.C0 = CFrame.new(2.5, 0, 0)
legacy.C1 = CFrame.new()
legacy.Parent = legacyRoot

local directRoot = part("DirectRoot", Vector3.new(3, 5, 0), true, Color3.new(0.1, 0.65, 1))
local directMiddle = part("DirectMiddle", Vector3.new(5.5, 5, 0), false, Color3.new(0.2, 0.85, 1))
local directEnd = part("DirectEnd", Vector3.new(8, 5, 0), false, Color3.new(0.4, 1, 0.8))

local first = Instance.new("WeldConstraint")
first.Name = "FirstLink"
first.Part0 = directRoot
first.Part1 = directMiddle
first.Parent = directRoot

local second = Instance.new("WeldConstraint")
second.Name = "SecondLink"
second.Part0 = directMiddle
second.Part1 = directEnd
second.Parent = directMiddle

local elapsed = 0
game:GetService("RunService").Heartbeat:Connect(function(delta)
	elapsed += delta
	legacyRoot.CFrame = CFrame.new(-4, 5 + math.sin(elapsed) * 1.5, 0) * CFrame.Angles(0, elapsed, 0)
	directRoot.CFrame = CFrame.new(3, 5, 0) * CFrame.Angles(0, -elapsed * 0.7, 0)
end)

print("welds: legacy offset and three-part WeldConstraint chain created")
