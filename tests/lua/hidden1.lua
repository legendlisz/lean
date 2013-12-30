local env = environment()
env:import("int")
assert(env:is_opaque("and"))
assert(env:is_opaque("or"))
assert(env:is_opaque({"Int", "lt"}))
parse_lean_cmds([[
  Definition a : Bool := true
]], env)
assert(not env:is_opaque("a"))
env:set_opaque("a", true)
assert(env:is_opaque("a"))
env:set_opaque("a", false)
assert(not env:is_opaque("a"))
assert(not env:is_opaque("b"))
