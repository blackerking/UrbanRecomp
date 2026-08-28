# `lib/retcomm-rbengine` has no `url` in .gitmodules, so recursive clone fails

**FILED: https://github.com/mstan/snesrecomp/issues/41**

*Written by an AI (Claude) working on a SimCity SNES recomp host with
@blackerking, who has reviewed it.*

## What happens

`.gitmodules` on `main` declares two submodules. The second has no `url`:

```
[submodule "lib/recomp-net"]
	path = lib/recomp-net
	url = https://github.com/TechnicallyComputers/recomp-net.git
	branch = main

[submodule "lib/retcomm-rbengine"]
	path = lib/retcomm-rbengine
	branch = main
```

The tree does record a gitlink for it:

```
160000 commit ebd94a4729abe2c0615070cef3ffe05b3f9ebf28	lib/retcomm-rbengine
```

so `git` finds something to check out and no way to fetch it:

```
fatal: No url found for submodule path 'snesrecomp/lib/retcomm-rbengine' in .gitmodules
fatal: Failed to recurse into submodule path 'snesrecomp'
```

Anyone cloning recursively — directly, or as a submodule of a host project,
which is how it reached us — stops there.

## Impact, and what still works

For this host the outer checkout does complete and the build is unaffected;
nothing we compile needs `retcomm-rbengine`. So the practical damage is limited
to the recursive clone erroring out, which is enough to break a
`clone --recurse-submodules` in CI or a first-time setup.

It surfaced here when a host repo bumped its `snesrecomp` pointer onto current
`main`: the previous pointer predated the gitlink, so a recursive clone that
worked before now fails.

## Fix

Either supply the `url` for `lib/retcomm-rbengine`, or drop the gitlink and the
`.gitmodules` stanza if the dependency is not actually required to build.

I have deliberately not guessed the URL. `lib/recomp-net` points at
`TechnicallyComputers`, and it would be reasonable to assume the sibling lives
there too — but that is an inference, and committing a wrong URL would turn a
clear error into a confusing one.
