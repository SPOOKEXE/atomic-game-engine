-- Every concrete ValueBase leaf, with values that make type mixups visible.
--
-- Run this scene to exercise creation, typed property writes, hierarchy and
-- ObjectValue references through the public Luau surface.

local anchor = Instance.new("Part")
anchor.Name = "ValueTarget"
anchor.Anchored = true
anchor.Position = Vector3.new(0, 3, 0)
anchor.Parent = workspace

local boolean = Instance.new("BoolValue")
boolean.Name = "Enabled"
boolean.Value = true
boolean.Parent = anchor

local frame = Instance.new("CFrameValue")
frame.Name = "Placement"
frame.Value = CFrame.new(1, 2, 3)
frame.Parent = anchor

local colour = Instance.new("Color3Value")
colour.Name = "Tint"
colour.Value = Color3.new(0.25, 0.5, 0.75)
colour.Parent = anchor

local integer = Instance.new("IntValue")
integer.Name = "Score"
integer.Value = 9000000000
integer.Parent = anchor

local number = Instance.new("NumberValue")
number.Name = "Ratio"
number.Value = 3.141592653589793
number.Parent = anchor

local object = Instance.new("ObjectValue")
object.Name = "Target"
object.Value = anchor
object.Parent = anchor

local text = Instance.new("StringValue")
text.Name = "Message"
text.Value = "value objects are live"
text.Parent = anchor

local vector = Instance.new("Vector3Value")
vector.Name = "Direction"
vector.Value = Vector3.new(-4, 5, 6)
vector.Parent = anchor

assert(boolean:IsA("ValueBase"))
assert(object.Value:Equals(anchor))
assert(text.Value == "value objects are live")

print("valueobjects: 8 typed values created under " .. anchor:GetFullName())
