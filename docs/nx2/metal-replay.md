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

The next milestone is a private NVN2 replay wired to Lichii's boot and teardown, with
local Metal shaders and an actual game frame. The existing Vulkan renderer remains the
fallback. The fixture's GPU time is not a game performance result.
