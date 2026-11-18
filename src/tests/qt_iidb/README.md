# Testing the iiDB artwork picker

The tests use synthetic responses and images. They do not contact iiDB or need an API key.
Install Qt 6 with Widgets, Network, Test and the **Qt Image Formats** (`qtimageformats`) module.
The WebP image plugin must be discoverable by Qt.

To build just the picker tests from the repository root:

```sh
cmake -S src/tests/qt_iidb -B build-iidb -G Ninja -DCMAKE_PREFIX_PATH=/path/to/Qt
cmake --build build-iidb
ctest --test-dir build-iidb --output-on-failure
```

The production build with `LICHII_TESTS=ON` also runs the exact-ID file test against Lichii's
artwork writer, using a temporary profile:

```sh
cmake --build build --target iidb_artwork_tests
ctest --test-dir build -R '^iidb_artwork$' --output-on-failure
```
