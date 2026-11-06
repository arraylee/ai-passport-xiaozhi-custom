# JPEG display lifecycle regression

The target uses the repository's real LVGL screens, images, descriptors and
deletion events. `extract_production.py` extracts the production state and the
complete unchanged `build_dsc`/`show_image`/`teardown`/`tick`/request functions from
`jpeg_view.c` at build time. The test does not reimplement their algorithms.
Only hardware decoding, Flash mapping and application callback boundaries are
replaced. Tests also enforce deleting image references before unmapping a frame.

```sh
cmake -S tests/host/jpeg_view_lifecycle -B /tmp/passport-jpeg-view-lifecycle
cmake --build /tmp/passport-jpeg-view-lifecycle --target test_jpeg_view_lifecycle test_jpeg_view_decode -j 8
ctest --test-dir /tmp/passport-jpeg-view-lifecycle --output-on-failure
python3 tests/host/jpeg_view_lifecycle/check_regressions.py /tmp/passport-jpeg-view-lifecycle
```

Fourteen independent lifecycle scenarios cover a blocked photo retaining its pending request
without decoding or capturing a temporary return screen; one display after
unblocking; replacing a photo while retaining the original home and deleting the
old photo; replacement decode/mapping failure cleanup and silent avatar recovery;
updating an avatar while a blocked full photo remains queued; delaying named and
uploaded avatars while a photo is active and preserving their notification
ownership; first-photo decode/mapping failure cleanup of the home avatar; and
the first actual home refresh after returning from a photo, which must already
contain the restored avatar without an intermediate placeholder frame. They also
cover exit requests during queued/ongoing decoding and 50 photo/native-avatar
return cycles.

`test_jpeg_view_decode` runs those same scenarios with the complete unchanged
production `jpeg_view.c`, real JPEG preflight and real LVGL. Only the ESP JPEG
codec, capability heap, Flash and PNG decoder boundaries are controlled. Nine
additional scenarios verify complete RGB565 stripe bytes/order; stripe allocation
failure before frame erase; allocation cleanup after frame-begin, frame-write and
codec-process failures; rejecting zero/oversized output lengths and invalid
headers before allocation; and keeping PNG/native avatars free of JPEG stripe
allocations. The capability-heap boundary checks 16-byte alignment, the 7,680-byte
maximum, decoder-close/free/source-unmap order and no outstanding allocation.
The real-decode native return scenario requires exactly one stripe allocation
and release per photo, with none for native avatar restoration over 50 cycles.

The optional regression checker disables nine protections in the generated
build copy, confirms each corresponding scenario fails for the expected reason,
then restores the exact production slice and reruns the complete suite. It never
changes production files. The ESP JPEG codec implementation, device heap capacity
and physical display output are outside these host tests.
