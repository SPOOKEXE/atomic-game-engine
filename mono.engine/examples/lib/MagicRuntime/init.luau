--!strict

-- Turning a compiled spell's presentation into what this engine draws.
--
-- The Roblox project's `MagicRuntime` builds a Part, an Attachment, one
-- `ParticleEmitter` per authored emitter, a `Trail`, a muzzle `Beam` and a
-- `PointLight`. **This engine has all of them**, so this module builds all of
-- them.
--
-- It did not always. This file used to open by saying the engine had exactly
-- one of those classes and that the rest were an honest gap, and it counted the
-- authored emitters into a log line so the gap was visible. That was true when
-- it was written and stopped being true when `mono.engine/effects` landed:
-- `effects/Classes.cpp` registers `ParticleEmitter`, `Beam` and `Trail`, and
-- `scene/Part.cpp` registers `Attachment` and `PointLight`. Nothing told this
-- file, so every spell in the demo went on drawing a bare cube while the data
-- underneath it was complete. `mono.engine/examples/Particles.luau` is the
-- reference for every property set below.
--
-- ## What the authored data says, and what it does not
--
-- An authored emitter carries five things: an id, `enabledSeconds`, a lifetime
-- range and a rate. That is all `Components.Presentation.Particle` takes, so
-- **everything else about how an emitter looks is derived here** - from the
-- projectile's own colour and size, which is what the presets spend their curve
-- keys on. A preset that wanted a specific spread or speed would have to be able
-- to say so, and it cannot; deriving is not a shortcut around authored data,
-- there is none to read.
--
-- The beam, the trail and the light are the same: the Roblox runtime hardcoded
-- them and the schema never carried them, so their widths, lifetimes and ranges
-- are this file's and are scaled off the projectile so a big spell reads bigger.
--
-- ## Bursts, and why they are a window rather than a call
--
-- `Timeline.EmitParticles(id, time, emitterId, count)` is a one-shot burst, and
-- Roblox spells it `emitter:Emit(count)`. **This engine has no such method**, so
-- a burst here is the emitter turned on for exactly as long as its rate takes to
-- deliver `count` particles. The arithmetic is in `emit` below.
--
-- That is why `Rate` is set once, to the largest thing the emitter will ever be
-- asked to do, and `TimeScale` carries the difference. `ParticleSystem.cpp`
-- sizes an emitter's slice of the pool from `Rate` **on the frame the block is
-- claimed** and never resizes it, so an emitter that raised its rate to burst
-- would burst into a block sized for its resting rate and emit almost nothing.
-- `TimeScale` multiplies the emission accumulator and nothing else, so scaling
-- through it leaves the reservation alone. A cue-only emitter therefore sits at
-- `TimeScale = 0`: enabled, holding its rows, emitting nothing until cued.

local MagicRuntime = {}

-- How long a burst window stays open, in seconds.
--
-- Short enough to read as one puff rather than a stream, and long enough that a
-- 60 Hz tick can see it: a window under one frame would be opened and closed
-- between two `advance` calls and deliver whatever a single frame's accumulator
-- happened to owe.
local BURST_SECONDS = 0.08

-- What a burst emitter's rate is when no cue says otherwise.
--
-- Every emitter reserves `Rate * lifetime.max + 1` rows whether it is emitting
-- or not, so this is a floor on what a cue-only emitter costs and not a target.
local MINIMUM_BURST_RATE = 1.0

-- `MagicCore`'s validator still allows exactly four material names, and nothing
-- on this side reads them.
--
-- **No material is set, and `Enum.Material` no longer exists.** v0.10 removed
-- it: seventeen names nothing sampled, replaced by a `Material` instance that
-- names a published asset. A demo that shipped one would only draw right on a
-- machine whose content store happened to hold that material, so the colour
-- carries the look here exactly as it always actually did.

-- One authored emitter, built.
export type Emitter = {
	instance: ParticleEmitter,
	-- What `TimeScale` returns to when a burst window closes. Zero for an
	-- emitter that only ever fires from a cue.
	resting: number,
	-- Seconds left of the open burst window, or zero.
	window: number,
	-- When the authored `enabledSeconds` runs out, measured in projectile age.
	-- Past this the emitter stops emitting and its particles finish.
	until_: number,
}

export type Handle = {
	part: BasePart,
	-- Where everything that is not the body hangs. See `Particles.luau`: an
	-- emitter parented to an attachment emits from the attachment's resolved
	-- world frame, which is what makes a tail follow a moving projectile.
	attachment: Attachment,
	-- Authored size, before any Scale curve. Curves multiply this rather than
	-- the current size, or a shrinking curve compounds every frame.
	baseSize: Vector3,
	baseTransparency: number,
	lifetime: number,
	curves: { any },
	emitters: { [string]: Emitter },
	-- How many emitters this projectile draws.
	emitterCount: number,
	light: Light?,
	trail: Trail?,
	beam: Beam?,
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

-- The largest single burst any cue in `timeline` asks of `emitterId`.
--
-- Read at build time because it decides the emitter's reservation, which is
-- fixed for the life of the block. A cue asking for more than this would emit
-- what its rows allow and no more.
local function largestBurst(timeline: { any }, emitterId: string): number
	local largest = 0
	for _, cue in timeline do
		if cue.kind == "Timeline.EmitParticles" and cue.emitterId == emitterId then
			largest = math.max(largest, cue.count or 0)
		end
	end
	return largest
end

-- A colour ramp from the projectile's own colour to a dimmer, cooler version of
-- it, which is what a spark or a smoke tail does as it ages.
local function trailColours(colour: Color3): ColorSequence
	return ColorSequence.new({
		ColorSequenceKeypoint.new(0, colour),
		ColorSequenceKeypoint.new(1, Color3.new(colour.R * 0.25, colour.G * 0.25, colour.B * 0.35)),
	})
end

-- Builds the body of one projectile from a compiled spell, with everything that
-- hangs off it.
--
-- `compiled` is what `Compiler.compileGraph` produced for this recipe -- the
-- root document or one of its children -- so a burst child is built from its own
-- recipe rather than from its parent's.
--
-- `muzzle` is the attachment a muzzle beam tethers back to, or nil for no beam.
-- One per lane rather than one per projectile: the tether is between the caster
-- and the shot, and the caster does not move.
function MagicRuntime.build(compiled: any, parent: Instance?, name: string?, muzzle: Attachment?): Handle
	local projectile = compiled.clientProgram.projectile
	local presentation = projectile.presentation

	local part = Instance.new("Part")
	part.Name = name or "Projectile"
	part.Anchored = true
	-- A projectile is a visual: it must not shove the terrain it is about to
	-- crater, and nothing in this slice reads its collisions.
	part.CanCollide = false

	local size = Vector3.new(presentation.size.x, presentation.size.y, presentation.size.z)
	local colour = Color3.new(presentation.color.r, presentation.color.g, presentation.color.b)
	part.Size = size
	part.Color = colour
	part.Transparency = presentation.transparency or 0

	if parent ~= nil then
		part.Parent = parent
	else
		part.Parent = workspace
	end

	-- The one hang point, at the body's centre. Every emitter, the trail and the
	-- beam's near end all resolve through it.
	local attachment = Instance.new("Attachment")
	attachment.Parent = part

	local span = math.max(size.X, math.max(size.Y, size.Z))
	local lifetime = projectile.lifetime.seconds
	local timeline = presentation.timeline or {}

	local emitters: { [string]: Emitter } = {}
	local built = 0

	for _, authored in presentation.particles or {} do
		local instance = Instance.new("ParticleEmitter")
		instance.Name = authored.id

		local sustain = authored.rate or 0
		local burst = largestBurst(timeline, authored.id) / BURST_SECONDS

		-- The reservation, and the reason `Rate` is written once. See the
		-- header: the pool sizes a block from this on the frame it is claimed.
		local rate = math.max(sustain, burst, MINIMUM_BURST_RATE)
		instance.Rate = rate
		instance.TimeScale = sustain / rate

		-- **Enabled even at rest**, because an emitter the pool sees as disabled
		-- with nothing alive gives its rows back - `RefreshEmitters` says so -
		-- and a cue-only emitter would then have to claim a block on the frame
		-- it bursts and burst into whatever was left. Holding the rows costs
		-- them; `TimeScale` is what makes it silent.
		instance.Enabled = true

		instance.Lifetime = NumberRange.new(authored.lifetime.min, authored.lifetime.max)

		-- Derived, every one of them: the schema carries none of this. Scaled
		-- off the body so a nine-metre nova reads bigger than a dart.
		instance.Speed = NumberRange.new(span * 1.5, span * 4.0)
		instance.SpreadAngle = Vector2.new(28, 28)
		instance.Acceleration = Vector3.new(0, span * 0.5, 0)
		instance.Drag = 1.2
		instance.Additive = true
		instance.LightEmission = 0.7
		instance.Size = NumberSequence.new({
			NumberSequenceKeypoint.new(0, span * 0.55),
			NumberSequenceKeypoint.new(1, 0),
		})
		instance.Transparency = NumberSequence.new({
			NumberSequenceKeypoint.new(0, 0.1),
			NumberSequenceKeypoint.new(1, 1),
		})
		instance.Color = trailColours(colour)
		instance.Parent = attachment

		emitters[authored.id] = {
			instance = instance,
			resting = sustain / rate,
			window = 0,
			-- `enabledSeconds` is measured from spawn, and the solver clamps it
			-- to the projectile's own life. Doing the same here means a scene
			-- that dropped an event still stops emitting at the authored moment.
			until_ = math.min(authored.enabledSeconds, lifetime),
		}
		built += 1
	end

	-- The ribbon behind the body. Two attachments a body apart, because a trail
	-- is the quad swept between them - one point would have no width.
	local trail: Trail? = nil
	do
		local top = Instance.new("Attachment")
		top.CFrame = CFrame.new(Vector3.new(0, span * 0.5, 0))
		top.Parent = part

		local bottom = Instance.new("Attachment")
		bottom.CFrame = CFrame.new(Vector3.new(0, -span * 0.5, 0))
		bottom.Parent = part

		local ribbon = Instance.new("Trail")
		ribbon.Attachment0 = top
		ribbon.Attachment1 = bottom
		ribbon.Lifetime = 0.35
		ribbon.Additive = true
		ribbon.Color = trailColours(colour)
		ribbon.Transparency = NumberSequence.new(0.15, 1)
		ribbon.Parent = part
		trail = ribbon
	end

	-- The blast light. Range off the body for the reason the emitters are: a
	-- constant would make a dart light a room and a nova light a desk.
	local light: Light? = nil
	do
		local lamp = Instance.new("PointLight")
		lamp.Color = colour
		lamp.Brightness = 2.0
		lamp.Range = span * 6.0
		lamp.Parent = part
		light = lamp
	end

	-- The muzzle tether, when there is a muzzle to tether to.
	local beam: Beam? = nil
	if muzzle ~= nil then
		local tether = Instance.new("Beam")
		tether.Attachment0 = muzzle
		tether.Attachment1 = attachment
		tether.Width0 = span * 0.15
		tether.Width1 = span * 0.6
		tether.Additive = true
		tether.Color = trailColours(colour)
		tether.Transparency = NumberSequence.new(0.4, 0.05)
		tether.Parent = part
		beam = tether
	end

	return {
		part = part,
		attachment = attachment,
		baseSize = size,
		baseTransparency = presentation.transparency or 0,
		lifetime = lifetime,
		curves = presentation.curves or {},
		emitters = emitters,
		emitterCount = built,
		light = light,
		trail = trail,
		beam = beam,
	}
end

-- Applies the authored curves at a projectile's age.
function MagicRuntime.applyCurves(handle: Handle, age: number)
	for _, curve in handle.curves do
		local value = sampleCurve(curve, age, handle.lifetime)

		if curve.property == "Scale" then
			handle.part.Size = handle.baseSize * value

			-- The light follows the body, because a spell that swells as it arms
			-- and lights the ground by a constant amount reads as two effects.
			local light = handle.light
			if light ~= nil then
				light.Range = math.max(handle.baseSize.X, handle.baseSize.Y) * 6.0 * value
			end
		elseif curve.property == "Transparency" then
			handle.part.Transparency = math.clamp(value, 0, 1)
		end
	end

	-- **Authored, and applied whether or not a cue ever arrives.** The solver
	-- schedules its own disable for the same moment, so this is the second of
	-- two agreeing sources rather than a fallback - and it is the one that still
	-- holds if a consumer never wires the events up.
	for _, emitter in handle.emitters do
		if emitter.window <= 0 and age >= emitter.until_ then
			emitter.instance.TimeScale = 0
		end
	end
end

-- Closes any burst window that has run its course.
--
-- Called with the frame delta rather than the projectile's age: a window is a
-- length of wall time and the age is a simulation clock that a cancelled
-- projectile stops advancing.
function MagicRuntime.advance(handle: Handle, delta: number)
	for _, emitter in handle.emitters do
		if emitter.window <= 0 then
			continue
		end

		emitter.window -= delta
		if emitter.window <= 0 then
			emitter.window = 0
			emitter.instance.TimeScale = emitter.resting
		end
	end
end

-- Fires a one-shot burst of `count` particles from one emitter.
--
-- `Timeline.EmitParticles`, and the window is how a burst is spelled here - see
-- the header. A count larger than the emitter reserved for at build time emits
-- what its rows hold, which is the same thing Roblox's own pool does.
function MagicRuntime.emit(handle: Handle, emitterId: string, count: number)
	local emitter = handle.emitters[emitterId]
	if emitter == nil or count <= 0 then
		return
	end

	-- Full speed for exactly as long as the rate needs to deliver the count.
	-- `Rate` already covers the largest burst this emitter was built for, so the
	-- window is `BURST_SECONDS` for that one and shorter for anything smaller.
	emitter.instance.TimeScale = 1
	emitter.window = count / emitter.instance.Rate
end

-- Stops or restarts one emitter's steady emission.
--
-- `Timeline.SetEmitterEnabled`, which the validator restricts to disable-only
-- cues; the enable half is here because the disable half would be the only
-- caller of a one-way function and that reads as an oversight.
--
-- **`TimeScale` and not `Enabled`**, so the emitter keeps its rows and whatever
-- is already in the air finishes. Turning `Enabled` off would be correct too -
-- the pool waits for the last particle before reclaiming - but it would also
-- surrender the reservation, and a later cue would have to win it back.
function MagicRuntime.setEmitterEnabled(handle: Handle, emitterId: string, enabled: boolean)
	local emitter = handle.emitters[emitterId]
	if emitter == nil then
		return
	end

	emitter.window = 0
	emitter.instance.TimeScale = if enabled then emitter.resting else 0
end

function MagicRuntime.setPosition(handle: Handle, position: Vector3)
	handle.part.Position = position
end

-- Hides the body without destroying it, for the moment a projectile detonates.
--
-- **The tails outlive the body, which is the whole reason this is not
-- `destroy`.** The Roblox runtime kept the part alive here for exactly that and
-- this engine needs it for the same reason: an emitter's particles live in the
-- pool for their own lifetime after the last one is spawned, and destroying the
-- part takes the emitter's rows with it. What stops is emission, the beam and
-- the light; what finishes is everything already in the air.
function MagicRuntime.conceal(handle: Handle)
	handle.part.Visible = false

	for _, emitter in handle.emitters do
		emitter.window = 0
		emitter.instance.TimeScale = 0
	end

	local beam = handle.beam
	if beam ~= nil then
		beam.Enabled = false
	end

	local trail = handle.trail
	if trail ~= nil then
		trail.Enabled = false
	end

	local light = handle.light
	if light ~= nil then
		light.Enabled = false
	end
end

-- How long after `conceal` the last thing this projectile put in the air dies.
--
-- **What a caller has to wait before `destroy` is safe.** Destroying the part
-- takes its emitters with it, and an emitter's rows go back to the pool when it
-- goes - so a spell destroyed at impact loses the burst it fired at impact,
-- which is the one the whole preset was built around.
function MagicRuntime.longestTail(handle: Handle): number
	local longest = 0
	for _, emitter in handle.emitters do
		longest = math.max(longest, emitter.instance.Lifetime.Max)
	end

	local trail = handle.trail
	if trail ~= nil then
		longest = math.max(longest, trail.Lifetime)
	end
	return longest
end

function MagicRuntime.destroy(handle: Handle)
	handle.part:Destroy()
end

-- How many emitters a whole compiled graph authors.
--
-- Reported by the demo scene. It used to count what could not be drawn and now
-- counts what is drawn, which is the same number read the other way round.
function MagicRuntime.emitterCount(graph: any): number
	local total = #(graph.root.clientProgram.projectile.presentation.particles or {})
	for _, recipe in graph.recipes do
		total += #(recipe.clientProgram.projectile.presentation.particles or {})
	end
	return total
end

MagicRuntime.sampleCurve = sampleCurve
MagicRuntime.Build = MagicRuntime.build

return table.freeze(MagicRuntime)
