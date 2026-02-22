#!/usr/bin/env python3
"""
Patch script for zstd
Applies necessary transformations to the zstd source code
for integration into the ladybug codebase.
"""

import sys
import os
from pathlib import Path

def rename_c_to_cpp(srcdir):
    """Rename all .c files to .cpp for C++ compilation."""
    print("  Renaming .c files to .cpp...")
    lib_dir = srcdir / "lib"
    for c_file in lib_dir.rglob("*.c"):
        cpp_file = c_file.with_suffix(".cpp")
        c_file.rename(cpp_file)
        print(f"    Renamed: {c_file.relative_to(srcdir)} -> {cpp_file.relative_to(srcdir)}")

def add_namespace_wrapper(srcdir):
    """Add lbug_zstd namespace wrapper to zstd.h."""
    print("  Skipping namespace wrapper (incompatible with zstd 1.5.7 internal headers)")
    # Note: zstd 1.5.7 uses extern "C" blocks which are incompatible with namespace wrappers
    # when internal headers include the main header. The extern "C" ensures C linkage, and
    # adding a namespace inside causes type visibility issues for internal code.
    return True

def remove_assembly_file(srcdir):
    """Remove assembly file to avoid hidden symbol issues with shared libraries."""
    print("  Removing assembly file (will use C fallback)...")
    asm_file = srcdir / "lib" / "decompress" / "huf_decompress_amd64.S"
    
    if asm_file.exists():
        asm_file.unlink()
        print(f"    Removed: {asm_file.relative_to(srcdir)}")
    else:
        print(f"    Assembly file not found (already removed or doesn't exist)")

def main():
    if len(sys.argv) < 2:
        print("Usage: do-patch.py <source-directory>")
        sys.exit(1)
    
    srcdir = Path(sys.argv[1]).resolve()
    
    if not srcdir.exists():
        print(f"Error: Source directory {srcdir} does not exist")
        sys.exit(1)
    
    print("Applying zstd patches...")
    
    try:
        rename_c_to_cpp(srcdir)
        add_namespace_wrapper(srcdir)
        remove_assembly_file(srcdir)
        print("Patches applied successfully!")
        return 0
    except Exception as e:
        print(f"Error applying patches: {e}")
        import traceback
        traceback.print_exc()
        return 1

if __name__ == "__main__":
    sys.exit(main())
