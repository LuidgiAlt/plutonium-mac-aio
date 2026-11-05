# x87sidecar (Plutonium AIO build)

Speeds up x87 floating-point code of 32-bit Windows games under Rosetta 2.
A separate arm64 process (the sidecar) translates x87 instructions for
Rosetta: Rosetta's `translate_insn` is patched to ask the sidecar first, and
whatever the sidecar declines, stock Rosetta translates as usual.

Derived from x87sidecar 1.9.1 (MIT, see LICENSE), reduced to what the AIO
uses and hardened:

- Only the cooperative attach: Wine hands over the task port at start
  (`--cooperative`, patch 0023 in the AIO). No ptrace attach, no in-process
  hook, no decoder hook, no AOT tooling, profilers or benchmarks.
- If the sidecar dies, the target keeps running on stock Rosetta (the
  translation state is restored from a snapshot) instead of exiting.
- A failed assert declines that one request instead of ending the sidecar.
- Opcode ids are mapped by name from Rosetta's own table; without that table
  nothing is hooked.
- The stub is placed after the last `__TEXT` section of libRosettaRuntime.
- The configuration is fixed; the only environment variable is `X87_LOGS=1`
  (verbose log).

## Build

    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64
    cmake --build build --target x87sidecar

`cmake --build build` also builds the x86_64 test programs in `build/bin/tests`
(Apple clang). Each runs as `build/bin/x87sidecar --cooperative <test>` and
prints PASS or FAIL. `--probe` checks the installed Rosetta without launching
anything.
