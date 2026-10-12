--!strict
-- A click carries only the camera ray. Arena.luau decides what it strikes.
local UserInputService = game:GetService("UserInputService")
local ReplicatedStorage = game:GetService("ReplicatedStorage")
local HttpService = game:GetService("HttpService")
if not game:GetService("RunService"):IsReplica() then
	warn("arena: connect to a server hosting Arena.luau to fire at the targets")
	return
end
local remote = ReplicatedStorage:WaitForChild("ArenaFire", 10)
if remote == nil then
	warn("arena: connect to a server hosting Arena.luau to fire at the targets")
	return
end
local fire = remote :: RemoteEvent

UserInputService.InputBegan:Connect(function(input: InputObject, processed: boolean)
	if processed then return end
	if input.UserInputType ~= Enum.UserInputType.MouseButton1 and input.KeyCode ~= Enum.KeyCode.ButtonR2 then
		return
	end
	local camera = workspace.CurrentCamera :: Camera?
	if camera == nil then return end
	local frame = camera.CFrame
	local origin = frame.Position
	local direction = frame.LookVector
	fire:FireServer(HttpService:JSONEncode({
		x = origin.X, y = origin.Y, z = origin.Z,
		dx = direction.X, dy = direction.Y, dz = direction.Z,
	}))
end)
