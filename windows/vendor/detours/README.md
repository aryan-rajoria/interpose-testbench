# Vendored Microsoft Detours subset

Only what depcheck links against is vendored (~1.2 MB of the full repo):

- `include/` — public headers
- `lib.X64/detours.lib` — the x64 static import library

Source: <https://github.com/microsoft/Detours>, shallow clone pinned at
commit `adb0760` ("Support hooking ARM64EC target functions (#388)").

To reproduce the lib from a fresh clone (see the Detours gotcha in
`windows/README.md`): enter a VS dev environment (`windows\msvc-env.cmd`)
and run `nmake` from the clone root — stop once `lib.X64\detours.lib` is
built; the samples stage needs `sn.exe` (strong-name signing) and is not
required.

Everything else upstream (samples, `bin.X64` tools, tests) is deliberately
not vendored.
