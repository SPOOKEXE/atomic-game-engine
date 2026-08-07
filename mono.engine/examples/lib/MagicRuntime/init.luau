--!strict

-- Turning a compiled spell's presentation into what this engine can draw.
--
-- The Roblox project's `MagicRuntime` builds a Part, an Attachment, one
-- `ParticleEmitter` per authored emitter, an optional `Trail`, an optional
-- muzzle `Beam`, and a `PointLight`. **This engine has exactly one of those
-- classes.** So this module is deliberately not a port of that one -- it is the
-- honest subset, and the gap is written down rather than faked.
--
-- ## What is missing, and what it costs
--
-- | Roblox class | here | what is lost |
-- |---|---|---|
-- | `Part` | yes | nothing |
-- | `ParticleEmitter` | **no** | every tail, trail, smoke plume and impact burst |
-- | `Beam` | **no** | the muzzle tether and `AtomicNova`'s radial shafts |
-- | `Trail` | **no** | the ribbon behind a moving projectile |
-- | `PointLight` | **no** | the blast light |
-- | `Attachment` | **no** | nothing on its own; it exists to hang the above off |
--
-- **`cueEmitterIds` is the load-bearing loss.** Five presets author emitters with
-- rate zero and no timeline cue, which is this schema's way of saying "an effect
-- cue fires this" -- it is how `AtomicNova` grows a mushroom out of one part.
-- Every one of those is invisible here. The *data* is intact and every solver
-- still routes it; there is simply nothing to emit.
--
-- **No stand-in is drawn for them.** Spawning little cubes where particles would
-- be would make a screenshot look like the pipeline works, and it does not.
-- `emitterCount` reports how many an authored spell wanted so a caller can say
-- so out loud.
--
-- ## What does survive
--
-- The body, which is most of what a projectile reads as in motion: shape, size,
-- colour, material, transparency, and the authored `Scale` and `Transparency`
-- curves sampled against the projectile's own age. That is enough for a bolt to
-- swell as it arms and fade as it dies, which is the animation the presets spend
-- most of their curve keys on.

local MagicRuntime = {}

-- `MagicCore`'s validator still allows exactly four material names, and nothing
-- on this side reads them.
--
-- **No material is set, and `Enum.Material` no longer exists.** v0.10 removed
-- it: seventeen names nothing sampled, replaced by a `Material` instance that
-- names a published asset. A demo that shipped one would only draw right on a
-- machine whose content store happened to hold that material, so the colour
-- carries the look here exactly as it always actually did.

export type Handle = {
	part: BasePart,
	-- Authored size, before any Scale curve. Curves multiply this rather than
	-- the current size, or a shrinking curve compounds every frame.
	baseSize: Vector3,
	baseTransparency: number,
	lifetime: number,
	curves: { any },
	-- How many emitters the spell authored that this engine cannot draw.
	emitterCount: number,
}

-- Linear interpolation between a curve's keys.
--
-- The Roblox runtime samples the same keys the same way; the sampling is in the
-- data, not in the engine. `NormalizedLifetime` is the only time mode the
-- presets use, and it is the only one honoured here -- an unknown mode is
-- treated as normalised rather than refused, because a curve that silently stops
-- animating is easier to see than a scene that will not load.
local function sampleCurve(curve: any, age: number, lifetime: number): number
	local keys = curve.keys
	if #keys == 0 then
		return 0
	end

	local time = if lifetime > 0 then math.clamp(age / lifetime, 0, 1) else 0

	if time <= keys[1].time then
		return keys[1].value
	end
	if time >= keys[#keys].time then
		return keys[#keys].value
	end

	for index = 1, #keys - 1 do
		local from = keys[index]
		local to = keys[index + 1]
		if time >= from.time and time <= to.time then
			local span = to.time - from.time
			if span <= 0 then
				return to.value
			end
			return from.value + (to.value - from.value) * ((time - from.time) / span)
		end
	end

	return keys[#keys].value
end

-- Builds the body of one projectile from a compiled spell.
--
-- `compiled` is what `Compiler.compileGraph` produced for this recipe -- the
-- root document or one of its children -- so a burst child is built from its own
-- recipe rather than from its parent's.
function MagicRuntime.build(compiled: any, parent: Instance?, name: string?): Handle
	local presentation = compiled.clientProgram.projectile.presentation

	local part = Instance.new("Part")
	part.Name = name or "Projectile"
	part.Anchored = true
	-- A projectile is a visual: it must not shove the terrain it is about to
	-- crater, and nothing in this slice reads its collisions.
	part.CanCollide = false

	local size = Vector3.new(presentation.size.x, presentation.size.y, presentation.size.z)
	part.Size = size
	part.Color = Color3.new(presentation.color.r, presentation.color.g, presentation.color.b)
	part.Transparency = presentation.transparency or 0

	if parent ~= nil then
		part.Parent = parent
	else
		part.Parent = workspace
	end

	return {
		part = part,
		baseSize = size,
		baseTransparency = presentation.transparency or 0,
		lifetime = compiled.clientProgram.projectile.lifetime.seconds,
		curves = presentation.curves or {},
		emitterCount = #(presentation.particles or {}),
	}
end

-- Applies the authored curves at a projectile's age.
function MagicRuntime.applyCurves(handle: Handle, age: number)
	for _, curve in handle.curves do
		local value = sampleCurve(curve, age, handle.lifetime)

		if curve.property == "Scale" then
			handle.part.Size = handle.baseSize * value
		elseif curve.property == "Transparency" then
			handle.part.Transparency = math.clamp(value, 0, 1)
		end
	end
end

function MagicRuntime.setPosition(handle: Handle, position: Vector3)
	handle.part.Position = position
end

-- Hides the body without destroying it, for the moment a projectile detonates.
--
-- The Roblox runtime keeps the part alive here so its particle tails can finish;
-- there are no tails, so this is only about the body disappearing at the instant
-- it should rather than one frame later.
function MagicRuntime.conceal(handle: Handle)
	handle.part.Visible = false
end

function MagicRuntime.destroy(handle: Handle)
	handle.part:Destroy()
end

-- How many authored emitters across a whole compiled graph have nothing to draw
-- them. Reported by the demo scene so the gap is visible in the log rather than
-- only in this comment.
function MagicRuntime.missingEmitters(graph: any): number
	local total = #(graph.root.clientProgram.projectile.presentation.particles or {})
	for _, recipe in graph.recipes do
		total += #(recipe.clientProgram.projectile.presentation.particles or {})
	end
	return total
end

MagicRuntime.sampleCurve = sampleCurve
MagicRuntime.Build = MagicRuntime.build

return table.freeze(MagicRuntime)
