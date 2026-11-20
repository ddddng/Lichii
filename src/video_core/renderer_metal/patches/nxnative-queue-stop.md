# Native Metal teardown

Apply `nxnative-queue-stop.patch` to nxnative at
`7ebb76eda8ec1c532a4fc52170f2c5e13b18e80f` and compile with
`LICHII_NVN_METAL_BRIDGE`. The native state is single-use in each process.
Pause must not call the permanent close API.

1. The app closes admission to guest NVN dispatch, then calls
   `nvn_mtl_request_stop()` before waiting for guest threads to stop. The stop
   flag and queue admission share `nq_mu`. The worker no longer selects any
   pending node, even if its event is later signalled. Cancellation wakes
   receipt, guest GPU-fence, pacing, and physical present waiters. It does not
   publish fake fence values or interrupt Metal's resource-safety waits.
2. Once all admitted dispatch calls have returned, the app calls
   `nvn_mtl_drain()` from the session owner, not from a native worker or
   callback. Keep guest memory, host callbacks, and the layer valid and do not
   pump a second session. Replay joins first: its active prefix can commit or
   leave the suspend gate, but an unpublished wait cannot keep replay alive.
   The classic present thread joins next, after any active drawable acquisition
   and handoff. Keep `CAMetalLayer.allowsNextDrawableTimeout` enabled and never
   require a synchronous call to the teardown thread from a front-end callback.
3. No replay or present producer remains. `rec_stop()` publishes the last
   encoding block, commits held submissions, and joins the optional encoder.
   The end signal is then appended to the Metal 4 queue; the classic queue gets
   its own terminal command buffer. Waiting for either queue alone would miss
   work submitted on the other. The feedback group separately waits for
   callbacks, including CPU stores to guest memory, before dropping queued
   present pins, abandoned jobs and saved queue bindings. The code terminates
   rather than returning if Metal cannot establish safe completion.
4. Clear the app's callbacks and release its layer and guest backing only
   after drain returns. A drawable already handed to the compositor can still
   be displayed; its presented handler accesses only native static bookkeeping.
   A no-copy Metal allocation retained by a native cache must own a backing
   lease independently of the app. Queue drain does not reset all native caches.

The bridge uses `nextDrawable`, not nxnative's detached display-link thread.
It also disables presentation snapshots, whole-window screenshot callbacks,
the detached uniform copy worker, and the guest SyncWait fast path that cannot
be interrupted. Asynchronous Metal encoding remains available.

## Cloud test

The test includes the patched scheduler and substitutes Metal and Darwin-only
services. It parks a queue on a real mapped-event wait, blocks CPU callers on
sync and present receipts, then verifies that cancellation wakes both while a
different queue's active prefix can still commit. The pending present must
never reach Metal, but its pin must be released at discard.

```sh
nvn=/path/to/patched/nxnative/dm/gfx/nvn
clang -std=gnu17 -DLICHII_NVN_METAL_BRIDGE -I"$nvn" -I"$nvn/include" -pthread \
  src/video_core/renderer_metal/patches/nxnative-queue-stop-test.c -o /tmp/nvn-queue-stop
timeout 10s /tmp/nvn-queue-stop
```

This does not exercise the Metal queues, drawable timeout, or feedback on an
Apple device.
