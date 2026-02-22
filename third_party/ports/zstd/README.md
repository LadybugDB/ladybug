# Zstd Update - Fix for Hidden Symbol Linker Error

## Problem

When updating zstd to 1.5.7, the new version includes x86-64 assembly optimizations (`huf_decompress_amd64.S`) that can cause linker errors when building shared libraries:

```
relocation R_X86_64_PC32 against undefined hidden symbol 
'HUF_decompress4X1_usingDTable_internal_fast_asm_loop' can not be used when making a shared object
```

## Root Cause

The assembly functions in `huf_decompress_amd64.S` are marked with `.hidden` directive, making them have hidden visibility. When:
1. The CMakeLists.txt sets `ZSTDLIB_VISIBILITY=` (empty) to hide all symbols
2. Position-independent code (`-fPIC`) is required for shared libraries
3. PC-relative relocations (R_X86_64_PC32) reference hidden symbols

The linker cannot resolve these references in a shared library context.

## Solution

Disable the x86-64 assembly optimizations entirely by:

1. **Adding `ZSTD_DISABLE_ASM` compile definition** in `CMakeLists.txt`
   - This prevents the C code from referencing the assembly functions
   - Falls back to portable C implementations

2. **Removing the assembly file** via the patch script
   - Ensures the assembly file isn't accidentally compiled
   - Keeps the source tree clean

3. **Automated via ports system**
   - Created `third_party/ports/zstd/do-patch.py` script
   - Handles all transformations automatically
   - Integrated into `BUILD_COMMANDS` in the port Makefile

## Files Modified

### Port Definition
- `third_party/ports/zstd/Makefile` - Added `BUILD_COMMANDS` to run patch script
- `third_party/ports/zstd/do-patch.py` - Automated patch script that:
  1. Renames `.c` files to `.cpp` for C++ compilation
  2. Adds `lbug_zstd` namespace wrapper to `zstd.h`
  3. Removes `huf_decompress_amd64.S` assembly file

### Build Configuration
- `third_party/zstd/CMakeLists.txt` - Added:
  ```cmake
  # Disable x86-64 ASM optimizations to avoid hidden symbol issues with shared libraries
  target_compile_definitions(zstd PRIVATE ZSTD_DISABLE_ASM)
  ```

### Package Management
- `third_party/pkg` - Enhanced to handle `.c` to `.cpp` file mapping:
  - Added `lib/` directory to search paths
  - Maps `.c` extensions to `.cpp` when syncing files

## Usage

To update zstd in the future:

```bash
cd third_party
./pkg zstd fetch     # Download new version (update VERSION in Makefile first)
./pkg zstd extract   # Extract source
./pkg zstd build     # Apply patches (rename .c, add namespace, remove asm)
./pkg zstd install   # Sync files to third_party/zstd/
```

The patch script is idempotent and can be run multiple times safely.

## Performance Impact

The assembly optimizations provide ~10-20% performance improvement for Huffman decompression on x86-64 with BMI2 instructions. By disabling them:
- Compression/decompression falls back to optimized C implementations
- Performance impact is minimal for most workloads
- Ensures compatibility across all platforms (x86-64, ARM64, etc.)
- Avoids complex visibility and relocation issues

## Alternative Solutions Considered

1. **Make assembly functions non-hidden** - Would require modifying assembly file and coordinating with upstream
2. **Compile assembly separately with different flags** - Complex build system changes
3. **Use dynamic BMI2 detection** - Still requires handling hidden symbols
4. **Keep current approach** - ✅ Simplest, most maintainable, cross-platform

## References

- Zstd assembly optimizations: `lib/decompress/huf_decompress_amd64.S`
- Symbol visibility: `lib/common/portability_macros.h` (`ZSTD_HIDE_ASM_FUNCTION`)
- Assembly control: `lib/common/portability_macros.h` (`ZSTD_ENABLE_ASM_X86_64_BMI2`)
