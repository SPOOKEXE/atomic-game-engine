--!strict
-- A small town with NPCs whose perception and movement decisions use separate
-- fixed-tick cadence. Their attack deadlines and animation tracks keep ticking.

local RunService = game:GetService("RunService")
if RunService:IsReplica() then return end

local cadence = game:GetService("UpdateCadenceService")
local data = game:GetService("DataSceneService")
local RIG = "examples.SwingRig"
local SOURCE_REVISION = 1
-- Set the authored attribute before running this script to open one crowded town.
local authoredPopulation = workspace:GetAttribute("CadencePopulation")
if authoredPopulation ~= nil and authoredPopulation ~= 2 and authoredPopulation ~= 64 then
	error("CadencePopulation must be 2 or 64")
end
local crowded = authoredPopulation == 64

local function part(name: string, size: Vector3, position: Vector3, color: Color3): Part
	local object = Instance.new("Part")
	object.Name = name
	object.Size = size
	object.Position = position
	object.Color = color
	object.Anchored = true
	object.Parent = workspace
	return object
end

part(
	"Town Square",
	if crowded then Vector3.new(264, 1, 128) else Vector3.new(52, 1, 52),
	Vector3.new(0, -0.5, 0),
	Color3.new(0.29, 0.31, 0.29)
)
for index = -1, 1 do
	part(
		"House " .. index,
		Vector3.new(7, 7, 7),
		Vector3.new(index * 14, 3.5, if crowded then 40 else -21),
		Color3.new(0.49, 0.36, 0.25)
	)
end

type Practice = { Target: Part, Health: Humanoid, Id: string }
local function practiceTarget(name: string, id: string, position: Vector3): Practice
	local target = part(name, Vector3.new(3, 4, 3), position, Color3.new(0.7, 0.3, 0.2))
	target:SetAttribute("DataFactoryId", id)
	local health = Instance.new("Humanoid")
	health.RootPart = target
	health.MaxHealth = 100
	health.Health = 100
	health.Parent = target
	return { Target = target, Health = health, Id = id }
end
local sharedTarget: Practice? = nil
if not crowded then
	sharedTarget = practiceTarget("Practice Target", "town.practice-target", Vector3.new(0, 2, -8))
end

-- The one-bone authored clip uses the same 36-byte layout and SwingRig as
-- Animation.luau. Cadence never pauses the track's semantic time.
local clip = Instance.new("AnimationBuffer")
clip.Name = "Town Walk"
assert(clip.KeyframeBytes == 36)
local frames = buffer.create(clip.KeyframeBytes * 3)
local function keyframe(index: number, time: number, angle: number)
	local offset = index * clip.KeyframeBytes
	buffer.writeu16(frames, offset, 0)
	buffer.writeu16(frames, offset + 2, 0)
	buffer.writef32(frames, offset + 4, time)
	buffer.writef32(frames, offset + 8, 0)
	buffer.writef32(frames, offset + 12, 0)
	buffer.writef32(frames, offset + 16, 0)
	buffer.writef32(frames, offset + 20, 0)
	buffer.writef32(frames, offset + 24, math.sin(angle * 0.5))
	buffer.writef32(frames, offset + 28, 0)
	buffer.writef32(frames, offset + 32, math.cos(angle * 0.5))
end
keyframe(0, 0, -0.3)
keyframe(1, 0.5, 0.3)
keyframe(2, 1, -0.3)
assert(clip:BakeAnimation(1, frames))
clip.Parent = workspace

type Citizen = {
	Model: Model,
	Rig: SkinnedMeshPart,
	Humanoid: Humanoid,
	Target: Part,
	TargetHumanoid: Humanoid,
	TargetId: string,
}
local citizens: { Citizen } = {}
local function citizen(name: string, x: number, color: Color3, practice: Practice)
	local model = Instance.new("Model")
	model.Name = name
	local rig = Instance.new("SkinnedMeshPart")
	rig.Name = "Body"
	rig.MeshId = "engine.SkinnedWedge"
	rig.RigId = RIG
	rig.JointCount = 1
	rig.Size = Vector3.new(2.5, 3, 2.5)
	rig.Position = Vector3.new(x, 2.5, 8)
	rig.Color = color
	rig.Anchored = false
	rig.Parent = model
	local bone = Instance.new("Bone")
	bone.Name = "Root"
	bone.Parent = rig
	local animator = Instance.new("Animator")
	animator.Rig = rig
	animator.Parent = rig
	local animation = Instance.new("Animation")
	animation.AnimationBuffer = clip
	animation.RigId = RIG
	animation.Parent = animator
	local track = Instance.new("AnimationTrack")
	track.Animation = animation
	track.Looped = true
	track.IsPlaying = true
	track.WeightCurrent = 1
	track.WeightTarget = 1
	track.Parent = animator
	local humanoid = Instance.new("Humanoid")
	humanoid.RootPart = rig
	humanoid.WalkSpeed = 3.5
	humanoid.Parent = model
	model.Parent = workspace
	assert(cadence:Configure(model, "town.citizen." .. name, 8, 12, 4, 8, 3))
	model:SetAttribute("NextAttackTick", 0)
	table.insert(citizens, {
		Model = model,
		Rig = rig,
		Humanoid = humanoid,
		Target = practice.Target,
		TargetHumanoid = practice.Health,
		TargetId = practice.Id,
	})
end

if crowded then
	for index = 1, 64 do
		local x = (index - 32.5) * 4
		local practice = practiceTarget(
			"Practice Target " .. index,
			"town.practice-target." .. index,
			Vector3.new(x, 2.5, -492)
		)
		citizen("Citizen " .. index, x, Color3.new(0.2, 0.62, 0.78), practice)
	end
else
	assert(sharedTarget ~= nil)
	citizen("North Guard", -9, Color3.new(0.2, 0.62, 0.78), sharedTarget)
	citizen("South Guard", 9, Color3.new(0.78, 0.6, 0.24), sharedTarget)
end

local tick = 0
RunService.Heartbeat:Connect(function(_delta: number)
	tick += 1
	for _, npc in citizens do
		local target = npc.Target
		local targetHumanoid = npc.TargetHumanoid
		local difference = target.Position - npc.Rig.Position
		local distance = difference.Magnitude
		local fighting = distance <= 12 and targetHumanoid.Health > 0
		assert(cadence:SetCombat(npc.Model, fighting))

		local seeing = cadence:BeginUpdate(npc.Model, "perception", SOURCE_REVISION)
		if seeing.due then
			local visible = false
			if distance > 3 then
				local direction = difference.Unit
				local hit = data:Raycast({
					origin = npc.Rig.Position + direction * 2,
					direction = direction,
					max_distance_metres = distance,
				})
				visible = hit.status == "ok" and hit.hit and hit.id == npc.TargetId
			end
			assert(cadence:CommitPerception(npc.Model, seeing.token, SOURCE_REVISION, visible, target.Position, true))
		end

		local moving = cadence:BeginUpdate(npc.Model, "behaviour", SOURCE_REVISION)
		if moving.due then
			local observed = cadence:Perception(npc.Model)
			assert(observed ~= nil, "NPC perception is unavailable")
			local direction = Vector3.new(0, 0, 0)
			if observed.visible and distance > 5 then direction = difference.Unit end
			assert(cadence:CommitBehaviour(npc.Model, moving.token, SOURCE_REVISION, direction, true))
		end

		-- The deadline is checked every tick, including ticks with no expensive
		-- perception or behaviour work. Promotion refreshes both before damage.
		local nextAttackValue = npc.Model:GetAttribute("NextAttackTick")
		if nextAttackValue ~= nil and type(nextAttackValue) ~= "number" then
			error("NextAttackTick must be a number")
		end
		local nextAttack = if nextAttackValue == nil then 0 else nextAttackValue
		if fighting and tick >= nextAttack and cadence:CombatReady(npc.Model, SOURCE_REVISION) then
			targetHumanoid.Health = math.max(0, targetHumanoid.Health - 1)
			npc.Model:SetAttribute("NextAttackTick", tick + 30)
		end
	end
end)
