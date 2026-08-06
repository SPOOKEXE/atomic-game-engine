--!strict

local TableUtil = require(script.Parent.TableUtil)
local Enums = require(script.Parent.Enums)

local Components = {}

local function copyConfig(config: any?): { [any]: any }
	if config == nil then
		return {}
	end
	return TableUtil.deepCopy(config)
end

local function lifetimeTime(seconds: number, config: any?): { [any]: any }
	local result = copyConfig(config)
	result.kind = "Lifetime.Time"
	result.seconds = seconds
	return result
end

local function motionLinear(velocity: any, config: any?): { [any]: any }
	local result = copyConfig(config)
	result.kind = "Motion.Linear"
	result.velocity = TableUtil.deepCopy(velocity)
	return result
end

local function motionBallistic(velocity: any, acceleration: any, config: any?): { [any]: any }
	local result = copyConfig(config)
	result.kind = "Motion.Ballistic"
	result.velocity = TableUtil.deepCopy(velocity)
	result.acceleration = TableUtil.deepCopy(acceleration)
	return result
end

local function motionBezier(duration: number, points: { any }, space: string?, config: any?): { [any]: any }
	local result = copyConfig(config)
	result.kind = "Motion.Bezier"
	result.duration = duration
	result.space = space or "LaunchLocal"
	result.points = TableUtil.deepCopy(points)
	return result
end

local function particleEmitter(
	id: string,
	enabledSeconds: number,
	minimumLifetime: number,
	maximumLifetime: number,
	config: any?
): { [any]: any }
	local result = copyConfig(config)
	result.kind = "Presentation.Particle"
	result.id = id
	result.enabledSeconds = enabledSeconds
	result.lifetime = {
		min = minimumLifetime,
		max = maximumLifetime,
	}
	if result.rate == nil then
		result.rate = 0
	end
	return result
end

local function presentationCurve(
	id: string,
	property: string,
	timeMode: string,
	keys: { any },
	config: any?
): { [any]: any }
	local result = copyConfig(config)
	result.kind = "Presentation.Curve"
	result.id = id
	result.property = property
	result.timeMode = timeMode
	result.keys = TableUtil.deepCopy(keys)
	return result
end

local function presentationPart(config: any?): { [any]: any }
	local result = copyConfig(config)
	result.kind = "Presentation.Part"
	if result.shape == nil then
		result.shape = "Ball"
	end
	if result.size == nil then
		result.size = { x = 1, y = 1, z = 1 }
	end
	if result.color == nil then
		result.color = { r = 1, g = 1, b = 1 }
	end
	if result.transparency == nil then
		result.transparency = 0
	end
	if result.material == nil then
		result.material = "Neon"
	end
	if result.particles == nil then
		result.particles = {}
	end
	if result.curves == nil then
		result.curves = {}
	end
	if result.timeline == nil then
		result.timeline = {}
	end
	return result
end

local function timelineEmitParticles(
	id: string,
	time: number,
	emitterId: string,
	count: number,
	config: any?
): { [any]: any }
	local result = copyConfig(config)
	result.kind = "Timeline.EmitParticles"
	result.id = id
	result.time = time
	result.emitterId = emitterId
	result.count = count
	return result
end

local function timelineSetEmitterEnabled(
	id: string,
	time: number,
	emitterId: string,
	enabled: boolean,
	config: any?
): { [any]: any }
	local result = copyConfig(config)
	result.kind = "Timeline.SetEmitterEnabled"
	result.id = id
	result.time = time
	result.emitterId = emitterId
	result.enabled = enabled
	return result
end

local function eventRecord(
	id: string,
	kind: string,
	order: number,
	action: string?,
	config: any?
): { [any]: any }
	local result = copyConfig(config)
	result.kind = kind
	result.id = id
	result.order = order
	result.action = action or Enums.EventAction.None
	return result
end

local function eventSpawn(id: string, order: number, action: string?): { [any]: any }
	return eventRecord(id, "Event.Spawn", order, action)
end

local function eventImpact(id: string, order: number, action: string?): { [any]: any }
	return eventRecord(id, "Event.Impact", order, action)
end

local function eventExpire(id: string, order: number, action: string?): { [any]: any }
	return eventRecord(id, "Event.Expire", order, action)
end

local function burstChild(id: string, recipeId: string, count: number, config: any?): { [any]: any }
	local result = copyConfig(config)
	result.id = id
	result.recipeId = recipeId
	result.count = count
	return result
end

local function deterministicBurst(children: { any }, config: any?): { [any]: any }
	local result = copyConfig(config)
	result.kind = "Burst.Deterministic"
	result.trigger = result.trigger or Enums.BurstTrigger.Expire
	result.distribution = result.distribution or Enums.BurstDistribution.Sphere
	result.children = TableUtil.deepCopy(children)
	result.maxDepth = result.maxDepth or 1
	if result.random == nil then
		result.random = {
			kind = "Random.Derived",
			seedSource = "Server",
			purpose = "burst.direction",
		}
	end
	return result
end

local function effectDamage(amount: number, damageType: any?, sourceTags: { string }?, config: any?): { [any]: any }
	local suppliedConfig = if type(damageType) == "table" and sourceTags == nil and config == nil then damageType else config
	local result = copyConfig(suppliedConfig)
	result.kind = Enums.EffectKind.Damage
	result.amount = amount
	if type(damageType) == "string" then
		result.damageType = damageType
	else
		result.damageType = result.damageType or Enums.DamageType.Magic
	end
	result.sourceTags = TableUtil.deepCopy(sourceTags or result.sourceTags or {})
	return result
end

local function effectHeal(amount: number, config: any?): { [any]: any }
	local result = copyConfig(config)
	result.kind = Enums.EffectKind.Heal
	result.amount = amount
	return result
end

local function effectArea(
	center: any,
	radius: number?,
	effects: { any }?,
	config: any?
): { [any]: any }
	local result = if type(center) == "table" and radius == nil and effects == nil and config == nil
		then copyConfig(center)
		else copyConfig(config)
	result.kind = Enums.EffectKind.Area
	if type(center) == "string" then
		result.center = center
		result.radius = radius
	end
	result.falloff = result.falloff or Enums.AreaFalloff.Constant
	result.relationToSource = result.relationToSource or Enums.Relation.Any
	result.includeAnyTags = TableUtil.deepCopy(result.includeAnyTags or {})
	result.includeAllTags = TableUtil.deepCopy(result.includeAllTags or {})
	result.excludeAnyTags = TableUtil.deepCopy(result.excludeAnyTags or {})
	result.maxTargets = result.maxTargets or 16
	result.effects = TableUtil.deepCopy(effects or result.effects or {})
	return result
end

local function effectPull(
	center: any,
	deltaSpeed: number?,
	maxDeltaSpeed: number?,
	config: any?
): { [any]: any }
	local result = if type(center) == "table" and deltaSpeed == nil and maxDeltaSpeed == nil and config == nil
		then copyConfig(center)
		else copyConfig(config)
	result.kind = Enums.EffectKind.Pull
	if type(center) == "string" then
		result.center = center
		result.deltaSpeed = deltaSpeed
		result.maxDeltaSpeed = maxDeltaSpeed
	end
	return result
end

local function effectApplyStatus(
	definitionId: string,
	definitionVersion: number,
	stacks: number?,
	potency: number?,
	config: any?
): { [any]: any }
	local result = copyConfig(config)
	result.kind = Enums.EffectKind.ApplyStatus
	result.definitionId = definitionId
	result.definitionVersion = definitionVersion
	result.stacks = stacks or 1
	result.potency = potency or 1
	return result
end

local function effectCleanse(config: any?): { [any]: any }
	local result = copyConfig(config)
	result.kind = Enums.EffectKind.Cleanse
	result.includeAnyTags = TableUtil.deepCopy(result.includeAnyTags or {})
	result.includeAllTags = TableUtil.deepCopy(result.includeAllTags or {})
	result.excludeAnyTags = TableUtil.deepCopy(result.excludeAnyTags or {})
	result.maxRemovals = result.maxRemovals or 1
	return result
end

local function effectTrigger(
	id: string,
	trigger: string,
	order: number,
	effect: any,
	cueIds: { string }?
): { [any]: any }
	return {
		id = id,
		trigger = trigger,
		order = order,
		effect = TableUtil.deepCopy(effect),
		cueIds = TableUtil.deepCopy(cueIds or {}),
	}
end

local function statusModifier(
	id: string,
	stat: string,
	operation: string,
	value: number,
	config: any?
): { [any]: any }
	local result = copyConfig(config)
	result.id = id
	result.stat = stat
	result.operation = operation
	result.value = value
	return result
end

local function statusDefinition(id: string, version: number, config: any?): { [any]: any }
	local result = copyConfig(config)
	result.id = id
	result.version = version
	result.tags = TableUtil.deepCopy(result.tags or {})
	result.duration = result.duration or 5
	result.tickOnApply = result.tickOnApply or false
	result.tickAtExpiry = result.tickAtExpiry or false
	result.stackPolicy = result.stackPolicy or "RefreshDuration"
	result.maxStacks = result.maxStacks or 1
	result.maxIndependentInstances = result.maxIndependentInstances or 1
	result.periodicEffects = TableUtil.deepCopy(result.periodicEffects or {})
	result.modifiers = TableUtil.deepCopy(result.modifiers or {})
	return result
end

Components.Lifetime = table.freeze({
	Time = lifetimeTime,
})

Components.Motion = table.freeze({
	Linear = motionLinear,
	Ballistic = motionBallistic,
	Bezier = motionBezier,
})

Components.Presentation = table.freeze({
	Part = presentationPart,
	Particle = particleEmitter,
	Curve = presentationCurve,
})

Components.Timeline = table.freeze({
	EmitParticles = timelineEmitParticles,
	SetEmitterEnabled = timelineSetEmitterEnabled,
})

Components.Event = table.freeze({
	Record = eventRecord,
	Spawn = eventSpawn,
	Impact = eventImpact,
	Expire = eventExpire,
})

Components.Burst = table.freeze({
	Child = burstChild,
	Deterministic = deterministicBurst,
})

Components.Effect = table.freeze({
	Damage = effectDamage,
	Heal = effectHeal,
	Area = effectArea,
	Pull = effectPull,
	ApplyStatus = effectApplyStatus,
	Cleanse = effectCleanse,
	Trigger = effectTrigger,
})

Components.Status = table.freeze({
	Definition = statusDefinition,
	Modifier = statusModifier,
})

Components.LifetimeTime = lifetimeTime
Components.MotionLinear = motionLinear
Components.MotionBallistic = motionBallistic
Components.MotionBezier = motionBezier
Components.PresentationPart = presentationPart
Components.ParticleEmitter = particleEmitter
Components.PresentationCurve = presentationCurve
Components.TimelineEmitParticles = timelineEmitParticles
Components.TimelineSetEmitterEnabled = timelineSetEmitterEnabled
Components.EventRecord = eventRecord
Components.BurstChild = burstChild
Components.DeterministicBurst = deterministicBurst
Components.EffectDamage = effectDamage
Components.EffectHeal = effectHeal
Components.EffectArea = effectArea
Components.EffectPull = effectPull
Components.EffectApplyStatus = effectApplyStatus
Components.EffectCleanse = effectCleanse
Components.EffectTrigger = effectTrigger
Components.StatusDefinition = statusDefinition
Components.StatusModifier = statusModifier

return table.freeze(Components)
