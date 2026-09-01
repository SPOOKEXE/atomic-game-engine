--!strict

local Harness = {}

export type Test = {
	Name: string,
	Run: () -> (),
}

function Harness.run(tests: { Test })
	for _, test in tests do
		test.Run()
	end
	print(`bladeborne tests: {#tests} passed`)
end

return Harness
