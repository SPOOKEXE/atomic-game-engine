--!strict
-- A self-contained skeletal animation. The script writes procedural keyframes
-- into a buffer, bakes them, and plays the resulting world-owned clip.

local RIG = "examples.SwingRig"

local clip = Instance.new("AnimationBuffer")
clip.Name = "ProceduralSwing"
assert(clip.KeyframeBytes == 36, "unexpected animation keyframe layout")

local records = buffer.create(clip.KeyframeBytes * 3)
local function writeKeyframe(index: number, time: number, angle: number)
	local offset = index * clip.KeyframeBytes
	buffer.writeu16(records, offset, 0)
	buffer.writeu16(records, offset + 2, 0)
	buffer.writef32(records, offset + 4, time)
	buffer.writef32(records, offset + 8, 0)
	buffer.writef32(records, offset + 12, 0)
	buffer.writef32(records, offset + 16, 0)
	buffer.writef32(records, offset + 20, 0)
	buffer.writef32(records, offset + 24, math.sin(angle * 0.5))
	buffer.writef32(records, offset + 28, 0)
	buffer.writef32(records, offset + 32, math.cos(angle * 0.5))
end

writeKeyframe(0, 0, -1.1)
writeKeyframe(1, 1, 1.1)
writeKeyframe(2, 2, -1.1)
assert(clip:BakeAnimation(2, records), "procedural animation did not bake")
assert(clip.DataSize > 0, "baked animation buffer stayed empty")
clip.Parent = workspace

local floor = Instance.new("Part")
floor.Name = "Floor"
floor.Anchored = true
floor.Size = Vector3.new(22, 0.5, 14)
floor.CFrame = CFrame.new(Vector3.new(0, -0.25, 0))
floor.Color = Color3.new(0.08, 0.10, 0.14)
floor.Parent = workspace

local function makeRig(name: string, x: number, color: Color3, phase: number)
	local pedestal = Instance.new("Part")
	pedestal.Name = name .. "Pedestal"
	pedestal.Anchored = true
	pedestal.Size = Vector3.new(1.2, 2, 1.2)
	pedestal.CFrame = CFrame.new(Vector3.new(x, 1, 0))
	pedestal.Color = Color3.new(0.22, 0.25, 0.32)
	pedestal.Parent = workspace

	local rig = Instance.new("SkinnedMeshPart")
	rig.Name = name
	rig.Anchored = true
	rig.MeshId = "engine.SkinnedWedge"
	rig.RigId = RIG
	rig.JointCount = 1
	rig.Size = Vector3.new(3.5, 3.5, 3.5)
	rig.CFrame = CFrame.new(Vector3.new(x, 4, 0))
	rig.Color = color
	rig.Parent = workspace

	local root = Instance.new("Bone")
	root.Name = "Root"
	root.Parent = rig

	local animator = Instance.new("Animator")
	animator.Name = "Animator"
	animator.Rig = rig
	animator.EvaluationThrottled = false
	animator.Parent = rig

	local animation = Instance.new("Animation")
	animation.Name = "Swing"
	animation.AnimationBuffer = clip
	animation.RigId = RIG
	animation.Parent = animator

	local track = Instance.new("AnimationTrack")
	track.Name = "SwingTrack"
	track.Animation = animation
	track.FadeTime = 0
	track.WeightCurrent = 1
	track.WeightTarget = 1
	track.TimePosition = phase
	track.Looped = true
	track.IsPlaying = true
	track.Parent = animator
end

-- The phase offsets keep all three real tracks moving while making their
-- different sampled poses readable in a still capture of the demo.
makeRig("AnimatedRig", -4, Color3.new(1.0, 0.28, 0.08), 0)
makeRig("AnimatedRigMiddle", 0, Color3.new(1.0, 0.68, 0.08), 0.67)
makeRig("AnimatedRigRight", 4, Color3.new(0.25, 0.72, 1.0), 1.33)

local camera = Instance.new("Camera")
camera.Name = "Camera"
camera.CFrame = CFrame.lookAt(Vector3.new(13, 7, 17), Vector3.new(0, 3, 0))
camera.Parent = workspace
workspace.CurrentCamera = camera
