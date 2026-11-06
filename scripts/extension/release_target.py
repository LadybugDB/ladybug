"""Prints the releases/ directory a Build and Deploy Extensions run may write, or fails.

Nightly runs write vdev. Every other run writes v<LBUG_EXTENSION_VERSION>. Once a
release of that minor version is tagged, its patch releases download from that
directory, so only a run on one of its vX.Y.Z tags may replace it. Before the
first release any ref may write it, since dev builds of main read it.
"""

import argparse
import pathlib
import re
import subprocess
import sys


def release_target(nightly, ref_type, ref_name, extension_version, tags):
    if not re.fullmatch(r"\d+\.\d+\.\d+", extension_version):
        raise SystemExit(f"Unexpected LBUG_EXTENSION_VERSION {extension_version!r}")
    if nightly:
        return "vdev"
    minor = ".".join(extension_version.split(".")[:2])
    tag_pattern = re.compile(rf"v{re.escape(minor)}\.\d+")
    released = sorted(t for t in tags if tag_pattern.fullmatch(t))
    if not released or (ref_type == "tag" and tag_pattern.fullmatch(ref_name)):
        return f"v{extension_version}"
    raise SystemExit(
        f"Refusing to write releases/v{extension_version} from {ref_type} {ref_name}: "
        f"{minor} is released ({', '.join(released)}), so only a v{minor}.N release tag may rebuild it. "
        f"Nightly runs write vdev."
    )


def extension_version(cmake_lists):
    return re.search(r'LBUG_EXTENSION_VERSION="([^"]+)"', cmake_lists.read_text()).group(1)


def remote_tags(repo_url):
    output = subprocess.run(["git", "ls-remote", "--tags", "--refs", repo_url], check=True,
                            capture_output=True, text=True).stdout
    return [line.split("refs/tags/", 1)[1] for line in output.splitlines()]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--nightly", choices=["true", "false"], required=True)
    parser.add_argument("--ref-type", required=True, help="github.ref_type: branch or tag")
    parser.add_argument("--ref-name", required=True, help="github.ref_name")
    parser.add_argument("--repo-url", required=True, help="repository to read release tags from")
    parser.add_argument("--cmake-lists", type=pathlib.Path, default=pathlib.Path("CMakeLists.txt"))
    args = parser.parse_args()
    print(release_target(args.nightly == "true", args.ref_type, args.ref_name,
                         extension_version(args.cmake_lists), remote_tags(args.repo_url)))


if __name__ == "__main__":
    sys.exit(main())
