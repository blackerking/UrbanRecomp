# `__attribute__((constructor))` in interp_bridge.c does not compile on MSVC

**FILED: https://github.com/mstan/snesrecomp/issues/40**

*Written by an AI (Claude) working on an SNES city-builder recomp host (Urban Recomp) with
@blackerking, who has reviewed it.*

## What happens

`runner/src/snes/interp_bridge.c` installs the interp-trace ring dump with a
GCC/Clang constructor attribute:

```c
/* Install the ring dump as cpu_state.c's halt-path hook (explicit hook, not
 * a PE weak symbol — see cpu_state.c). Constructor runs at image load. */
__attribute__((constructor))
static void itrace_install_dump_hook(void) {
    g_interp_recent_dump_hook = interp_bridge_dump_recent_steps;
}
```

MSVC has no such attribute and rejects it outright:

```
interp_bridge.c(763,15): error C2143: syntax error: missing ')' before '('
interp_bridge.c(763,27): error C2059: syntax error: ')'
interp_bridge.c(764,1): error C2143: syntax error: missing ')' before 'type'
interp_bridge.c(763,1): error C2091: function returns function
```

Every target that compiles `interp_bridge.c` fails. In this host that is three
of five; the two that survive build the interp816 core without this file, which
is why it took a while to notice.

Toolchain: MSVC 19.x, Visual Studio 2022, x64, via CMake.

## Suggested fix

The portable pair — the GCC attribute where it exists, and on MSVC a function
pointer in `.CRT$XCU`, which the CRT walks before `main()`:

```c
static void itrace_install_dump_hook(void);
#if defined(_MSC_VER)
#  pragma section(".CRT$XCU", read)
__declspec(allocate(".CRT$XCU"))
void (*itrace_install_dump_hook_ctor)(void) = itrace_install_dump_hook;
#  pragma comment(linker, "/include:itrace_install_dump_hook_ctor")
static void itrace_install_dump_hook(void)
#else
__attribute__((constructor))
static void itrace_install_dump_hook(void)
#endif
{
    g_interp_recent_dump_hook = interp_bridge_dump_recent_steps;
}
```

The pointer needs **external** linkage: `/include:` has to be able to name the
symbol, and without it the section entry is discarded and the hook never
installs. Marking it `static` links but silently does nothing, which is the
worse failure of the two.

Verified building all five targets of the Urban Recomp host with MSVC, and the
runtime behaviour is unchanged: qualify PASS at 2000 and 6000 frames, and 30
frames of scrolling gameplay are byte-identical to the pre-change build.

Happy to open a PR if useful — the change is carried on
an earlier host branch of `blackerking/snesrecomp` (since merged).
