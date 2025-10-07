import sys
from pathlib import Path

LBUG_ROOT = Path(__file__).parent.parent.parent.parent

if sys.platform == "win32":
    # \ in paths is not supported by lbug's parser
    LBUG_ROOT = str(LBUG_ROOT).replace("\\", "/")
