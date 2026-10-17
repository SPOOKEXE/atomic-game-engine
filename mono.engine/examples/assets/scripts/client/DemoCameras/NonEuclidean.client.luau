--!strict
-- Each player owns the portal tour camera and its editable playback controls.

local RunService = game:GetService("RunService")
local Players = game:GetService("Players")

local function hasPlayerCharacter(): boolean
	local player = Players.LocalPlayer :: Player?
	local character = if player ~= nil then player.Character else nil
	return character ~= nil and character:FindFirstChildOfClass("Humanoid") ~= nil
end

local playerCamera = workspace.CurrentCamera :: Camera?
if playerCamera ~= nil and hasPlayerCharacter() then
	return
end

local view = Instance.new("Camera")
local cameraName = script:GetAttribute("Name")
view.Name = if typeof(cameraName) == "string" then cameraName else "Viewer"
local initialFrame = script:GetAttribute("CFrame")
view.CFrame = if typeof(initialFrame) == "CFrame" then initialFrame else CFrame.new(Vector3.new(0, 5, 16))
view.FieldOfView = 70
view.Parent = workspace
workspace.CurrentCamera = view
view.CameraType = Enum.CameraType.Scriptable
local ownership = Instance.new("ObjectValue")
ownership.Name = "PublishedCamera"
ownership.Value = view
ownership.Parent = script

local lastStage = script:GetAttribute("LastStage")
local LAST = if typeof(lastStage) == "number" then lastStage else 220
local PACE = 7
local entrances: { Part } = {}
local entrancesReady = false
local entranceMetadata = script:GetAttribute("EntranceCount")
local entranceCount = if typeof(entranceMetadata) == "number" then entranceMetadata else 0
local elapsed = 0
local lastMode = "overview"
local previewReleased = false

type TourShot = { Direction: Vector3, Near: boolean?, Away: boolean?, Roll: number? }
local shots: { TourShot } = {}
for _, elevation in { -30, 0, 30 } do
	for _, azimuth in { -175, -135, -95, -85, -45, -5, 5, 45, 85, 95, 135, 175 } do
		local yaw = math.rad(azimuth)
		local pitch = math.rad(elevation)
		table.insert(shots, {
			Direction = Vector3.new(math.sin(yaw) * math.cos(pitch), math.sin(pitch), math.cos(yaw) * math.cos(pitch)),
		})
	end
end
for _, shot: TourShot in
	{
		{ Direction = Vector3.new(0, 0, 1) },
		{ Direction = Vector3.new(0, 0, -1) },
		{ Direction = Vector3.new(1, 0.25, 2).Unit, Roll = 0.4 },
		{ Direction = Vector3.new(0.35, 0.1, 0.02), Near = true },
		{ Direction = Vector3.new(0.35, 0.1, -0.02), Near = true },
		{ Direction = Vector3.new(0.6, 0.2, 1).Unit },
		{ Direction = Vector3.new(0.6, 0.2, -1).Unit },
		{ Direction = Vector3.new(0, 0, 1), Away = true },
		{ Direction = Vector3.new(0, 0, -1), Away = true },
	}
do
	table.insert(shots, shot)
end

-- These attributes are editable in the inspector. A nonnegative sample seeks
-- one fixed shot; pause holds automatic playback and step advances one shot.
view:SetAttribute("TourMode", "overview")
view:SetAttribute("TourSample", -1)
view:SetAttribute("TourPaused", false)
view:SetAttribute("TourStep", 0)
view:SetAttribute("TourCount", #shots * entranceCount)

local function angleShot(sample: number)
	local index = sample % (#shots * #entrances)
	local pane = entrances[math.floor(index / #shots) + 1]
	local shot = shots[index % #shots + 1]
	local size = pane.Size
	local frame = pane.CFrame
	local centre = frame:PointToWorldSpace(Vector3.new(0, 0, size.Z / 2))
	local offset = if shot.Near
		then Vector3.new(shot.Direction.X * size.X, shot.Direction.Y * size.Y, shot.Direction.Z)
		else shot.Direction * math.max(size.X, size.Y)
	local eye = frame:PointToWorldSpace(Vector3.new(0, 0, size.Z / 2) + offset)
	local toward = if shot.Away then eye + (eye - centre) else centre
	local pose = CFrame.lookAt(eye, toward) * CFrame.Angles(0, 0, shot.Roll or 0)
	if not view.CFrame:FuzzyEq(pose, 1e-6) then
		-- A seek is a camera cut. It cannot masquerade as a walk through every
		-- portal between two independently authored shots.
		view:CutTo(pose)
	end
	if view:GetAttribute("TourPane") ~= pane.Name then
		view:SetAttribute("TourPane", pane.Name)
	end
end

RunService.Heartbeat:Connect(function(deltaTime: number)
	if previewReleased then
		return
	end
	-- A sliced join may deliver the player rig after this local script starts.
	if hasPlayerCharacter() then
		previewReleased = true
		local current = workspace.CurrentCamera
		if current ~= nil and current:Equals(view) then
			if playerCamera ~= nil and playerCamera:IsDescendantOf(workspace) then
				workspace.CurrentCamera = playerCamera
				playerCamera.CameraType = Enum.CameraType.Custom
			else
				view.CameraType = Enum.CameraType.Custom
				return
			end
		end
		view:Destroy()
		return
	end
	if not entrancesReady then
		for _, item in workspace:GetDescendants() do
			local order = item:GetAttribute("DemoTourEntranceIndex")
			if typeof(order) == "number" and order >= 1 and order <= entranceCount then
				entrances[order] = item :: Part
			end
		end
		for index = 1, entranceCount do
			if entrances[index] == nil then
				return
			end
		end
		view:SetAttribute("TourCount", #shots * entranceCount)
		entrancesReady = true
	end
	-- Play owns its humanoid camera. The preview tour never takes it back.
	local current = workspace.CurrentCamera
	if current == nil or not current:Equals(view) then
		return
	end
	if view:GetAttribute("TourPaused") ~= true then
		elapsed += deltaTime
	end
	if view:GetAttribute("TourMode") == "angles" then
		local count = #shots * #entrances
		local selected = view:GetAttribute("TourSample")
		local sample = if typeof(selected) == "number"
				and selected >= 0
				and selected < math.huge
			then math.floor(selected) % count
			else math.floor(elapsed * 2) % count
		local step = view:GetAttribute("TourStep")
		if typeof(step) == "number" and step == step and math.abs(step) < math.huge then
			sample += math.floor(step) % count
		end
		angleShot(sample)
		lastMode = "angles"
		return
	end

	-- A sweep along the row and back, seen from three-quarters on.
	--
	-- **Not square on, and that is what makes half of these legible.** The first
	-- two exhibits are about how long a tunnel *looks* from outside, which a
	-- camera staring straight down the mouth cannot show - it sees the hole and
	-- none of the shell. Standing off to one side puts the outside and the
	-- inside in one frame, which is the comparison every one of them is making.
	local travelled = elapsed * PACE
	local lap = travelled % (LAST * 2)
	local along = if lap > LAST then LAST * 2 - lap else lap
	local sway = math.sin(travelled * 0.05) * 6

	local pose = CFrame.lookAt(
		Vector3.new(along + 15 + sway, 8 + math.sin(travelled * 0.031) * 2, 21),
		Vector3.new(along, 3.5, -7)
	)
	if lastMode == "angles" then
		view:CutTo(pose)
	else
		view.CFrame = pose
	end
	lastMode = "overview"
end)
