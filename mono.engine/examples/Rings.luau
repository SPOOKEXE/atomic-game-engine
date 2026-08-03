--!strict
-- Rings of orbiting, spinning parts.
--
-- Written the way a Roblox script is written: `game:GetService`, `Instance.new`,
-- properties by name, `part.Parent = workspace`, and behaviour on
-- `RunService.Heartbeat`. The motion is **CFrame composition** — an orbit is a
-- rotation times an offset, and a spin is a CFrame multiplied into itself each
-- beat — rather than trigonometry and Euler angles done by hand.
--
-- **The script is the simulation.** There is no `Orbit` component and no engine
-- system reading one; the engine does not know what an orbit is. What it knows
-- is `Part`, `CFrame` and `Size`, and this file does the rest.
--
-- What each property is underneath, since it is worth knowing once:
--
--     part.CFrame     -> scene::Transform.Frame      (a whole component)
--     part.Position   -> the translation of that frame, rotation kept
--     part.Size       -> scene::Bounds.HalfExtent    (doubled on the way out)
--     part.Color      -> scene::Visual.Tint
--     part.Anchored   -> whether RigidBody and Motion exist at all
--
-- An instance is an entity, a class is a set of components, and a property is a
-- projection of one or more of them. Nothing else exists.
--
-- **Determinism.** `deltaTime` is the world's fixed tick delta, never wall
-- time, and there is no `os`, no clock and no `math.random` here. A recording
-- of this scene replays byte for byte.

local RunService = game:GetService("RunService")

local COUNT = 512
local PER_RING = 64
local INNER_RADIUS = 3
local OUTER_RADIUS = 12

local rings = math.max(1, math.ceil(COUNT / PER_RING))
local ringStep = if rings > 1 then (OUTER_RADIUS - INNER_RADIUS) / (rings - 1) else 0

-- A deterministic value in [0, 1) from an index and a salt.
--
-- Not `math.random`: a shared generator makes the scene depend on the order
-- things were created in, and this has to be the same world every run.
local function hashed(index: number, salt: number): number
	local value = bit32.bxor(index * 0x9E3779B1, salt * 0x85EBCA77)
	value = bit32.bxor(value, bit32.rshift(value, 15))
	value = (value * 0x2545F491) % 0x100000000
	value = bit32.bxor(value, bit32.rshift(value, 13))
	return (value % 100000) / 100000
end

local function range(index: number, salt: number, low: number, high: number): number
	return low + hashed(index, salt) * (high - low)
end

-- Orbit state lives in the script, because how a part moves is the game's
-- business. The engine holds the part.
type Orbiter = {
	Part: Part,
	Radius: number,
	Height: number,
	Speed: number,
	Angle: number,
	Spin: CFrame,
	SpinRate: Vector3,
}

local orbiters: { Orbiter } = {}

for index = 0, COUNT - 1 do
	local ring = math.floor(index / PER_RING)
	local withinRing = index % PER_RING

	local part = Instance.new("Part")
	part.Name = "Orbiter"

	-- The whole edge, because `Size` is a full extent. The engine stores half
	-- of one and the conversion halves it, which is why a script never sees a
	-- half-extent anywhere.
	local edge = range(index, 31, 0.6, 1.4)
	part.Size = Vector3.new(edge, edge, edge)
	part.Color = Color3.fromRGB(
		range(index, 19, 40, 230),
		range(index, 23, 50, 205),
		range(index, 29, 90, 240)
	)

	-- Nothing here is simulated by physics; the script places every part every
	-- beat. Anchoring says so, and it is a real archetype move rather than a
	-- flag — an anchored part carries no RigidBody and no Motion, so the
	-- dynamic queries never visit it.
	part.Anchored = true
	part.Parent = workspace

	table.insert(orbiters, {
		Part = part,
		Radius = INNER_RADIUS + ring * ringStep,
		Height = range(index, 7, -5, 5),
		-- Outer rings turn more slowly, which reads as depth without any depth
		-- cue in the shading.
		Speed = 0.45 / (1 + ring * 0.35),
		Angle = withinRing / PER_RING * math.pi * 2,
		Spin = CFrame.new(),
		SpinRate = Vector3.new(
			range(index, 11, -1.2, 1.2),
			range(index, 13, -1.2, 1.2),
			range(index, 17, -1.2, 1.2)
		),
	})
end

RunService.Heartbeat:Connect(function(deltaTime: number)
	for _, orbiter in orbiters do
		orbiter.Angle += orbiter.Speed * deltaTime

		-- The part's own turn, accumulated by composition. `*=` on a CFrame is
		-- the idiom every rotation script is built out of, and it stays a unit
		-- rotation without anybody normalising anything.
		orbiter.Spin *= CFrame.Angles(
			orbiter.SpinRate.X * deltaTime,
			orbiter.SpinRate.Y * deltaTime,
			orbiter.SpinRate.Z * deltaTime
		)

		-- Turn about the origin, step out along the ring, then apply the spin.
		-- Reading left to right is reading the transform: this is what a CFrame
		-- chain is for, and it is why the orbit needs no sine and no cosine.
		orbiter.Part.CFrame = CFrame.Angles(0, orbiter.Angle, 0)
			* CFrame.new(orbiter.Radius, orbiter.Height, 0)
			* orbiter.Spin
	end
end)

print(`rings: {COUNT} parts across {rings} ring(s), animated from script`)
