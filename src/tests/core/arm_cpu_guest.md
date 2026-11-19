# Native ARM64 guest tests

Build and run on Linux ARM64 with 4 KiB host pages. Enable `BUILD_TESTING` and
`LICHII_TESTS` when configuring Lichii, then run:

```sh
cmake --build build --target arm_cpu_guest_tests
ctest --test-dir build -L '^arm_cpu_guest$' --output-on-failure --no-tests=error
```

The runner writes an assembly-built, keyless NRO to a private temporary directory
and boots it through the normal application loader, with null graphics and audio.
The NCE and Dynarmic cases use the same guest code to check SVC arguments and
results, TLS across thread switches, the guest counter and sleep, exclusive
reservations, and involuntary preemption. The NCE-only case exercises the current
unmapped data-load recovery path. A backend check combines the loader's
trampoline state with a TLS marker execution wrote into the NCE thread state.

The benchmark is opt-in. Run the following both ways on the same native ARM64
host and with the same affinity, alternating the order between pairs:

```sh
build/src/tests/arm_cpu_guest_tests nce --benchmark
build/src/tests/arm_cpu_guest_tests dynarmic --benchmark
```

Each loop runs once before its measured sample. The reported times use the
guest counter backed by the host's ARM virtual counter, and exclude emulator
startup. Report the spread along with the result. Do not use cross-architecture
emulation for timing, or treat these loops as game performance estimates.
