--!strict

return function(harness: any, core: any)
	local Components = core.Components
	local Presets = core.Presets
	local StatusDefinitions = core.StatusDefinitions
	local Validator = core.Validator

	local function diagnosticsFor(document: any): { any }
		local valid, diagnostics = Validator.validate(document)
		harness:assertEqual(valid, false)
		return diagnostics
	end

	harness:suite("validation diagnostics", function(h: any)
		h:test("metatable aborts before hostile index or iteration can run", function()
			local invoked = false
			local document = Presets.get("ArcaneBolt")
			document.projectile = setmetatable(document.projectile, {
				__index = function()
					invoked = true
					error("hostile __index must not run")
				end,
				__iter = function()
					invoked = true
					error("hostile __iter must not run")
				end,
			})
			local callOk, valid, diagnostics = pcall(Validator.validate, document)
			h:assertTrue(callOk)
			h:assertEqual(valid, false)
			h:assertDiagnostic(diagnostics, "UNSAFE_METATABLE", "$.projectile")
			h:assertEqual(invoked, false)
		end)

		h:test("unsafe function has exact code and path", function()
			local document = Presets.get("ArcaneBolt")
			document.projectile.presentation.color.r = function() end
			h:assertDiagnostic(diagnosticsFor(document), "UNSAFE_VALUE", "$.projectile.presentation.color.r")
		end)

		h:test("nonfinite number has exact code and path", function()
			local document = Presets.get("ArcaneBolt")
			document.projectile.lifetime.seconds = 0 / 0
			h:assertDiagnostic(diagnosticsFor(document), "NONFINITE_NUMBER", "$.projectile.lifetime.seconds")
		end)

		h:test("shared tables are rejected at the second path", function()
			local document = Presets.get("ArcaneBolt")
			local shared = { x = 1, y = 1, z = 1 }
			document.projectile.motion.velocity = shared
			document.projectile.presentation.size = shared
			h:assertDiagnostic(diagnosticsFor(document), "SHARED_OR_CYCLIC_TABLE", "$.projectile.presentation.size")
		end)

		h:test("sparse arrays are rejected exactly", function()
			local document = Presets.get("ArcaneBolt")
			local emitter = document.projectile.presentation.particles[1]
			document.projectile.presentation.particles = { [1] = emitter, [3] = emitter }
			h:assertDiagnostic(diagnosticsFor(document), "SPARSE_ARRAY", "$.projectile.presentation.particles")
		end)

		h:test("duplicate emitter IDs identify the second emitter", function()
			local document = Presets.get("ArcaneBolt")
			document.projectile.presentation.particles[2].id = document.projectile.presentation.particles[1].id
			h:assertDiagnostic(diagnosticsFor(document), "DUPLICATE_EMITTER_ID", "$.projectile.presentation.particles[2].id")
		end)

		h:test("duplicate curve IDs identify the second curve", function()
			local document = Presets.get("PurifyingPulse")
			document.projectile.presentation.curves[2].id = document.projectile.presentation.curves[1].id
			h:assertDiagnostic(diagnosticsFor(document), "DUPLICATE_CURVE_ID", "$.projectile.presentation.curves[2].id")
		end)

		h:test("timeline order reports the exact late cue", function()
			local document = Presets.get("ArcaneBolt")
			document.projectile.presentation.timeline = {
				Components.Timeline.EmitParticles("first", 1, "arcane_impact_burst", 1),
				Components.Timeline.EmitParticles("second", 0.5, "arcane_impact_burst", 1),
			}
			h:assertDiagnostic(diagnosticsFor(document), "INVALID_TIMELINE_ORDER", "$.projectile.presentation.timeline[2].time")
		end)

		h:test("emitter re-enable reports the exact enabled field", function()
			local document = Presets.get("ArcaneBolt")
			document.projectile.presentation.timeline = {
				Components.Timeline.SetEmitterEnabled("reenable", 0.5, "arcane_tail", true),
			}
			h:assertDiagnostic(
				diagnosticsFor(document),
				"EMITTER_REENABLE_UNSUPPORTED",
				"$.projectile.presentation.timeline[1].enabled"
			)
		end)

		h:test("emitter timing cannot exceed owner lifetime", function()
			local document = Presets.get("ArcaneBolt")
			document.projectile.presentation.particles[1].enabledSeconds = 2
			h:assertDiagnostic(diagnosticsFor(document), "OUT_OF_BOUNDS", "$.projectile.presentation.particles[1].enabledSeconds")
		end)

		h:test("duplicate event IDs and bad order are exact", function()
			local document = Presets.get("ArcaneBolt")
			document.events[2].id = document.events[1].id
			document.events[2].order = 1
			local diagnostics = diagnosticsFor(document)
			h:assertDiagnostic(diagnostics, "DUPLICATE_EVENT_ID", "$.events[2].id")
			h:assertDiagnostic(diagnostics, "INVALID_EVENT_ORDER", "$.events[2].order")
		end)

		h:test("duplicate burst actions for one trigger kind are rejected", function()
			local document = Presets.get("MeteorCluster")
			document.events[3].action = "Burst"
			h:assertDiagnostic(diagnosticsFor(document), "DUPLICATE_BURST_EVENT", "$.events[3].action")
		end)

		h:test("unsafe authored random seed is rejected", function()
			local document = Presets.get("MeteorCluster")
			document.burst.random.seed = 123
			h:assertDiagnostic(diagnosticsFor(document), "UNSAFE_RANDOM_CONFIG", "$.burst.random")
		end)

		h:test("burst fanout over budget is rejected", function()
			local document = Presets.get("MeteorCluster")
			document.budgets.maxProjectiles = 5
			h:assertDiagnostic(diagnosticsFor(document), "BURST_CAP_EXCEEDED", "$.burst.children")
		end)

		h:test("particle work over budget is rejected", function()
			local document = Presets.get("ArcaneBolt")
			document.budgets.maxParticles = 1
			h:assertDiagnostic(diagnosticsFor(document), "PARTICLE_CAP_EXCEEDED", "$.budgets.maxParticles")
		end)

		h:test("duplicate effect trigger IDs are rejected", function()
			local document = Presets.get("ArcaneBolt")
			document.effectTriggers[2] = Components.Effect.Trigger(
				document.effectTriggers[1].id,
				"Expire",
				2,
				Components.Effect.Heal(1)
			)
			h:assertDiagnostic(diagnosticsFor(document), "DUPLICATE_EFFECT_TRIGGER_ID", "$.effectTriggers[2].id")
		end)

		h:test("nested area effects are rejected at the child", function()
			local document = Presets.get("ArcaneBolt")
			local nested = Components.Effect.Area("Impact", 2, { Components.Effect.Damage(1) })
			local outer = Components.Effect.Area("Impact", 4, { nested })
			document.effectTriggers = { Components.Effect.Trigger("nested", "Impact", 1, outer) }
			document.budgets.maxAreaTargets = 32
			document.budgets.maxEffectCommands = 32
			h:assertDiagnostic(diagnosticsFor(document), "NESTED_AREA", "$.effectTriggers[1].effect.effects[1].kind")
		end)

		h:test("missing status version is rejected at the reference", function()
			local document = Presets.get("ArcaneBolt")
			document.effectTriggers = {
				Components.Effect.Trigger("missing", "Impact", 1, Components.Effect.ApplyStatus("missing", 7)),
			}
			h:assertDiagnostic(diagnosticsFor(document), "MISSING_STATUS_DEFINITION", "$.effectTriggers[1].effect.definitionVersion")
		end)

		h:test("duplicate status ID and version is rejected", function()
			local document = Presets.get("Fireball")
			document.statusDefinitions[2] = StatusDefinitions.burn()
			h:assertDiagnostic(diagnosticsFor(document), "DUPLICATE_STATUS_DEFINITION", "$.statusDefinitions[2].version")
		end)

		h:test("periodic status cannot recursively apply status", function()
			local document = Presets.get("ArcaneBolt")
			document.statusDefinitions = {
				Components.Status.Definition("loop", 1, {
					duration = 1,
					tickInterval = 1,
					periodicEffects = { Components.Effect.ApplyStatus("loop", 1) },
				}),
			}
			h:assertDiagnostic(diagnosticsFor(document), "PERIODIC_EFFECT_NOT_ALLOWED", "$.statusDefinitions[1].periodicEffects[1].kind")
		end)

		h:test("status tick cap reports the interval path", function()
			local document = Presets.get("ArcaneBolt")
			document.statusDefinitions = {
				Components.Status.Definition("too_fast", 1, {
					duration = 60,
					tickInterval = 0.01,
					periodicEffects = { Components.Effect.Damage(1) },
				}),
			}
			h:assertDiagnostic(diagnosticsFor(document), "STATUS_TICK_CAP_EXCEEDED", "$.statusDefinitions[1].tickInterval")
		end)

		h:test("unsupported motion kind reports the kind path", function()
			local document = Presets.get("ArcaneBolt")
			document.projectile.motion = { kind = "Motion.Teleport" }
			h:assertDiagnostic(diagnosticsFor(document), "UNSUPPORTED_KIND", "$.projectile.motion.kind")
		end)
	end)
end
