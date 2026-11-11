"""Installs and loads an extension line with every Ladybug release that reads it.

Every check runs in its own process with an empty home directory, so a cached
extension cannot hide what the repository serves, and a crash at exit fails it.
Releases come from PyPI; the line a release reads comes from LBUG_EXTENSION_VERSION
in its tag's CMakeLists.txt.

With --baseline, a check fails only when the extension also loads from the
baseline, so an extension that already fails there, for example because it
needs a library that is not shipped, does not fail the run. An extension the
baseline serves but the tested tree lacks fails, and so does a baseline that
cannot be reached, so an outage cannot hide a regression.
"""

import argparse
import functools
import http.server
import json
import os
import pathlib
import platform
import re
import subprocess
import sys
import tempfile
import threading
import urllib.error
import urllib.request
from concurrent.futures import ThreadPoolExecutor

OFFICIAL_REPO = "https://extension.ladybugdb.com/"
CMAKE_URL = "https://raw.githubusercontent.com/LadybugDB/ladybug/v{}/CMakeLists.txt"
PYPI_URL = "https://pypi.org/pypi/ladybug/json"
LINE_PATTERN = re.compile(r"v(\d+)\.(\d+)\.(\d+)")
EXTENSION_NAME = re.compile(r"[A-Za-z0-9_]+")
CHECK_TIMEOUT_SECONDS = 300

CHILD = """
import sys
import ladybug

repo, extension = sys.argv[1:]
conn = ladybug.Connection(ladybug.Database(":memory:"))
conn.execute(f"INSTALL {extension} FROM '{repo}'")
conn.execute(f"LOAD EXTENSION {extension}")
print("LOADED")
"""


def get_platform():
    system = {"Linux": "linux", "Darwin": "osx", "Windows": "win"}[platform.system()]
    machine = platform.machine().lower()
    arch = "arm64" if machine in ("arm64", "aarch64") else "amd64"
    return f"{system}_{arch}"


def fetch(url):
    # The extension CDN rejects urllib's default User-Agent from CI runners.
    request = urllib.request.Request(url, headers={"User-Agent": "ladybug-extension-smoke-test"})
    with urllib.request.urlopen(request, timeout=60) as response:
        return response.read().decode()


def list_dirs(url):
    """Directory names in an nginx or http.server index page."""
    return sorted(set(re.findall(r'href="([^"/?.][^"/]*)/"', fetch(url))))


def list_extensions(url):
    """Extensions in a line's platform directory, or [] when the repository has none for it."""
    try:
        names = [name for name in list_dirs(url) if name != "common"]
    except urllib.error.HTTPError as error:
        if error.code == 404:
            return []
        raise
    return validate_names(names)


def missing_from(tested, published):
    """Extensions the published tree serves that the tested tree would drop."""
    return sorted(set(published) - set(tested))


def validate_names(names):
    for name in names:
        if not EXTENSION_NAME.fullmatch(name):
            raise SystemExit(f"Unexpected extension name {name!r}")
    return names


def version_key(version):
    return tuple(int(part) for part in version.split("."))


def released_versions():
    releases = json.loads(fetch(PYPI_URL))["releases"]
    return sorted((v for v, files in releases.items() if files and re.fullmatch(r"\d+\.\d+\.\d+", v)),
                  key=version_key)


@functools.cache
def extension_line(version):
    match = re.search(r'LBUG_EXTENSION_VERSION="([^"]+)"', fetch(CMAKE_URL.format(version)))
    return "v" + match.group(1)


def engines_for_line(line, versions):
    match = LINE_PATTERN.fullmatch(line)
    if not match:
        return []
    candidates = [v for v in versions if tuple(v.split(".")[:2]) == match.groups()[:2]]
    return [v for v in candidates if extension_line(v) == line]


def make_venv(work_dir, python, version):
    venv = work_dir / f"ladybug-{version}"
    subprocess.run(["uv", "venv", "-q", "--python", python, str(venv)], check=True)
    bin_dir = venv / ("Scripts" if platform.system() == "Windows" else "bin")
    interpreter = bin_dir / ("python.exe" if platform.system() == "Windows" else "python")
    subprocess.run(["uv", "pip", "install", "-q", "--python", str(interpreter), f"ladybug=={version}"],
                   check=True)
    return interpreter


def check(interpreter, repo, extension, work_dir):
    home = tempfile.mkdtemp(prefix="home-", dir=work_dir)
    env = dict(os.environ, HOME=home, USERPROFILE=home)
    try:
        result = subprocess.run([str(interpreter), "-I", "-c", CHILD, repo, extension], env=env,
                                capture_output=True, text=True, timeout=CHECK_TIMEOUT_SECONDS)
    except subprocess.TimeoutExpired:
        return f"timed out after {CHECK_TIMEOUT_SECONDS} s"
    if result.returncode == 0 and "LOADED" in result.stdout:
        return None
    lines = [line.strip() for line in (result.stdout + result.stderr).replace(home, "~").splitlines()]
    # dyld follows the missing library's name with long "Referenced from" and "Reason" lines.
    lines = [line for line in lines
             if line.strip("^~ ") and not line.startswith(("Referenced from:", "Reason:"))]
    return f"exit code {result.returncode}: " + " | ".join(lines[-2:])[-500:]


def baseline_unreachable(error):
    """True when a baseline check could not download the extension for a reason other than a 404."""
    return error.startswith("timed out") or (
        "Failed to download extension" in error and "HTTP Returns: 404" not in error)


def counts_as_failure(error, baseline_error):
    """Whether a failed check fails the run.

    baseline_error is None when there is no baseline or the extension loads from it. A failure
    is forgiven when the baseline does not serve the extension (404) or it fails to load there
    too, but not when the baseline could not be reached.
    """
    if error is None:
        return False
    return baseline_error is None or baseline_unreachable(baseline_error)


def baseline_check(interpreter, baseline, extension, work_dir):
    error = check(interpreter, baseline, extension, work_dir)
    if error and baseline_unreachable(error):
        error = check(interpreter, baseline, extension, work_dir)
    return error


class QuietHandler(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *args):
        pass


def serve(directory):
    handler = functools.partial(QuietHandler, directory=directory)
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return f"http://127.0.0.1:{server.server_port}/"


def resolve(source):
    """A repository URL, or a local releases/ directory to serve over HTTP."""
    if os.path.isdir(source):
        return serve(source)
    # The URL goes into a quoted INSTALL ... FROM '<url>' statement.
    if not re.fullmatch(r"https?://[^'\\\s]+", source):
        raise SystemExit(f"Unexpected repository URL {source!r}")
    return source.rstrip("/") + "/"


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--line", nargs="+", required=True, help="lines to test, e.g. v0.21.0")
    parser.add_argument("--repo", default=OFFICIAL_REPO, help="repository URL or local releases/ directory")
    parser.add_argument("--baseline", help="repository URL or directory to compare against")
    parser.add_argument("--extension", nargs="*", default=[], help="extensions to test (default: all in the line)")
    parser.add_argument("--python", default="3.12", help="Python version for the release venvs")
    parser.add_argument("--jobs", type=int, default=4, help="checks to run at once")
    args = parser.parse_args()

    lines = [line for line in args.line if LINE_PATTERN.fullmatch(line)]
    for line in sorted(set(args.line) - set(lines)):
        print(f"{line}: skipped, not a release line")
    if not lines:
        return 0
    validate_names(args.extension)
    repo = resolve(args.repo)
    baseline = resolve(args.baseline) if args.baseline else None
    plat = get_platform()
    versions = released_versions()
    work_dir = pathlib.Path(tempfile.mkdtemp(prefix="extension-smoke-"))

    checks = []
    failures = 0
    for line in lines:
        engines = engines_for_line(line, versions)
        if not engines:
            print(f"{line}: skipped, no release on PyPI reads it")
            continue
        extensions = args.extension or list_extensions(f"{repo}{line}/{plat}/")
        print(f"{line}: {', '.join(engines)} x {len(extensions)} extensions on {plat}")
        if baseline and not args.extension:
            for name in missing_from(extensions, list_extensions(f"{baseline}{line}/{plat}/")):
                print(f"{line:9} {'-':8} {name:14} FAILED: the baseline serves it, the tested tree does not")
                failures += 1
        for engine in engines:
            interpreter = make_venv(work_dir, args.python, engine)
            checks += [(line, engine, extension, interpreter) for extension in extensions]

    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        errors = list(pool.map(lambda c: check(c[3], repo, c[2], work_dir), checks))
        retest = [i for i, error in enumerate(errors) if error and baseline]
        retest_errors = pool.map(lambda i: baseline_check(checks[i][3], baseline, checks[i][2], work_dir), retest)
        baseline_errors = dict(zip(retest, retest_errors))

    for i, ((line, engine, extension, _), error) in enumerate(zip(checks, errors)):
        baseline_error = baseline_errors.get(i)
        if error is None:
            status = "ok"
        elif counts_as_failure(error, baseline_error):
            unreachable = f"; baseline unreachable: {baseline_error}" if baseline_error else ""
            status = f"FAILED: {error}{unreachable}"
            failures += 1
        else:
            status = f"fails with the baseline too: {error}"
        print(f"{line:9} {engine:8} {extension:14} {status}")
    print(f"{len(checks)} checks, {failures} failures")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
