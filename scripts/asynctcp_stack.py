"""Cap the AsyncTCP service task stack on non-PSRAM MQTT observers.

AsyncTCP creates its service task from CONFIG_ASYNC_TCP_STACK_SIZE, 16 KiB by
default, and never deletes it. On a board without PSRAM that is a permanent slice
of internal DRAM, while the task's measured high-water mark stays under 2.2 KiB
even through a firmware upload, so 8 KiB leaves several times that as headroom.

The condition is read from the resolved defines rather than from the environment
name, so a new observer on a PSRAM-less board is covered without an ini edit.
"""
Import("env")

STACK_SIZE = 8192
DEFINE = "CONFIG_ASYNC_TCP_STACK_SIZE"


def _define_names():
    # Pre scripts run before ProcessProgramDeps populates CPPDEFINES.
    flags = [env.BoardConfig().get("build.extra_flags", []),
             env.GetProjectOption("build_flags", [])]
    parsed = env.ParseFlags(flags).get("CPPDEFINES", [])
    names = set()
    for item in parsed:
        name = item[0] if isinstance(item, (tuple, list)) else item
        names.add(str(name).split("=", 1)[0])
    return names


names = _define_names()
if "WITH_MQTT_BRIDGE" not in names or "BOARD_HAS_PSRAM" in names:
    Return()
if DEFINE in names:
    Return()  # an env that sizes the stack itself keeps its value

env.Append(CPPDEFINES=[(DEFINE, STACK_SIZE)])
print("asynctcp: %s=%d" % (DEFINE, STACK_SIZE))
