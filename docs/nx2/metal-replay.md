# Experimental Metal replay

`ENABLE_EXPERIMENTAL_METAL_REPLAY=ON` builds the off-by-default library on Apple Silicon.
It does not change the game's renderer selection. The offscreen test uses Metal 4 on
macOS 26 or later, and does not open a game or load Vulkan.

With `BUILD_TESTING=ON` and `LICHII_TESTS=ON`, run:

```sh
cmake --build build --target metal_replay_test
MTL_DEBUG_LAYER=1 ctest --test-dir build --output-on-failure -R '^metal_replay_test$'
```

The test draws from an aligned Lichii guest backing view, reads the pixels back, and
checks that Metal keeps the view alive after the caller drops its reference. The
view's owner must keep its mapping stable until the Metal deallocator runs. Imports
crossing a VM region boundary are rejected; they need a different mapping or copy path.

## Run the NVN bridge test

Check out nxnative at `7ebb76eda8ec1c532a4fc52170f2c5e13b18e80f`. Use an Xcode SDK with
Metal 4 support and its Metal Toolchain. Configure the Lichii build with
`-DLICHII_NXNATIVE_SOURCE_DIR=/path/to/nxnative` in addition to the test options above.

```sh
cmake --build build --target nvn_metal_bridge_test
ctest --test-dir build --output-on-failure -R '^nvn_metal_bridge_test$'
```

The executable uses a synthetic profile. It generates a restricted dispatcher from
the pinned NVN ABI and runs the calls through Lichii's ARM64 Dynarmic block-entry hook.
The test checks a fenced GPU copy into the original backing and reads all pixels back
from a cleared guest render target. It runs with Metal API and GPU validation enabled.
To also test asynchronous Metal encoding, set `DM_NVN_ENC_THREAD=1` on the CTest command.

The bridge test is separate from Lichii's renderer selection. It doesn't boot a game
or present to a window. Keep the existing Vulkan renderer until game boot, drawable
presentation, and teardown work on the native path.
