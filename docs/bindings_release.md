# Prebuilt C API libraries

Ladybug ships precompiled C API libraries for Linux, macOS, and Windows. Use them to build language bindings (Go, Rust, etc.) without compiling Ladybug.

---

## Supported platforms

| Platform   | Download | Contents |
| ---------- | -------- | -------- |
| Linux x86_64 | liblbug-linux-x86_64.tar.gz | lbug.h, lbug.hpp, liblbug.so |
| Linux arm64  | liblbug-linux-aarch64.tar.gz | lbug.h, lbug.hpp, liblbug.so |
| macOS (universal) | liblbug-osx-universal.tar.gz | lbug.h, lbug.hpp, liblbug.dylib |
| Windows x86_64 | liblbug-windows-x86_64.zip | lbug.h, lbug.hpp, lbug_shared.dll, lbug_shared.lib |

Artifacts are available on [GitHub Releases](https://github.com/LadybugDB/ladybug/releases). They are produced by the project’s CI; how to build and publish: see [Building and publishing](#building-and-publishing-maintainers) below.

---

## Building and publishing (maintainers)

**Automatic (recommended):** Push a version tag (e.g. `v0.14.1`). The workflow **Build Precompiled Binaries** runs on `push: tags: v*`, builds all platforms, and uploads artifacts to the GitHub Release for that tag. No manual steps.

**Manual:** If you need a build without a tag (e.g. nightly), run **Actions → Build Precompiled Binaries → Run workflow**. To attach those artifacts to an existing release, run **Actions → Release → Run workflow** and enter the tag and the run ID of the completed build.

---

## Getting the libraries

### From GitHub Releases

1. Open [Releases](https://github.com/LadybugDB/ladybug/releases) and pick a version.
2. Download the archive for your platform (see table above).
3. Extract. Files are at the archive root. Put the library in your binding’s search path (e.g. `lib/dynamic/<platform>/`) and `lbug.h` where your build expects headers.

### Using the download script

From a clone of Ladybug (or with the script copied into your binding repo):

```bash
scripts/download_liblbug.sh <version> <platform> [output_dir]
```

- **version** — Release tag (e.g. `v0.14.1`) or `latest`
- **platform** — `linux-amd64`, `linux-arm64`, `darwin`, `windows-amd64`
- **output_dir** — Where to unpack (default: current directory). Creates `lib/dynamic/<platform>/` and `include/`.

Examples:

```bash
./scripts/download_liblbug.sh v0.14.1 linux-amd64 .
./scripts/download_liblbug.sh latest darwin ./vendor/ladybug
```

Requires `curl`, `tar` (for .tar.gz), and `unzip` (for Windows). To use a different repo: `LADYBUG_REPO=owner/repo ./scripts/download_liblbug.sh ...`.

---

## Using in Go (CGO)

1. Get the library and header for your target (see above). Typical layout: `lib/dynamic/<platform>/` for the shared library, `include/` for `lbug.h`.
2. In the Go file that imports `"C"`, set CGO flags for each platform. Example for Linux amd64:

```go
// #cgo linux,amd64 LDFLAGS: -L${SRCDIR}/lib/dynamic/linux-amd64 -llbug -Wl,-rpath,${SRCDIR}/lib/dynamic/linux-amd64
// #cgo linux,amd64 CFLAGS: -I${SRCDIR}/include
import "C"
```

Use the same pattern for other `GOOS/GOARCH` (e.g. darwin → liblbug.dylib, windows → lbug_shared). Then call the C API via CGO as usual.

3. In your binding’s README, state which Ladybug version you tested with (e.g. “Compatible with Ladybug v0.14.x”). You can call `lbug_get_version()` at runtime to check the loaded library.

---

## Using in other languages

The same archives provide the C API header and shared library. Link your language’s FFI to the library and include `lbug.h`. The public API is in [src/include/c_api/lbug.h](../src/include/c_api/lbug.h). No C++ is required at the binding boundary.

---

## Version compatibility

C API compatibility follows the project’s versioning. Prebuilt libs match a specific release; bindings should document the tested Ladybug version. Runtime version strings are available via `lbug_get_version()` and `lbug_get_storage_version()`.
