import os
import shutil
import subprocess
import sys

LBUG_ROOT = os.path.dirname(os.path.dirname(os.path.realpath(__file__)))
# Datasets can only be copied from the root since copy.schema contains relative paths
os.chdir(LBUG_ROOT)

# Define the build type from input
if len(sys.argv) > 1 and sys.argv[1].lower() == "release":
    build_type = "release"
else:
    build_type = "relwithdebinfo"

# Change the current working directory
if os.path.exists(f"{LBUG_ROOT}/dataset/databases/tinysnb"):
    shutil.rmtree(f"{LBUG_ROOT}/dataset/databases/tinysnb")
if sys.platform == "win32":
    lbug_shell_path = f"{LBUG_ROOT}/build/{build_type}/src/lbug_shell"
else:
    lbug_shell_path = f"{LBUG_ROOT}/build/{build_type}/tools/shell/lbug"
subprocess.check_call(
    [
        "python3",
        f"{LBUG_ROOT}/benchmark/serializer.py",
        "TinySNB",
        f"{LBUG_ROOT}/dataset/tinysnb",
        f"{LBUG_ROOT}/dataset/databases/tinysnb",
        "--single-thread",
        "--lbug-shell",
        lbug_shell_path,
    ]
)
