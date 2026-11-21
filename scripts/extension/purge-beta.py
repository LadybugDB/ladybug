import pathlib
import os
from packaging.version import InvalidVersion, Version

CURRENT_DIR = pathlib.Path(__file__).parent.resolve()

RELEASES_PATH = CURRENT_DIR.joinpath('releases').resolve()

production_releases = open(CURRENT_DIR.joinpath('PRODUCTION_RELEASES')).read().splitlines()

releases_to_purge = [r for r in os.listdir(RELEASES_PATH) if r.startswith('v')]

releases_to_purge = [r for r in releases_to_purge if r not in production_releases]
releases_to_purge = [r[1:] for r in releases_to_purge]
releases_to_purge.sort(key=Version)

# Spare the newest non-production release only if it is newer than every
# production release, i.e. it is the in-development beta. Stale betas older
# than the supported production releases (e.g. v0.18.1) are purged as well.
production_versions = []
for r in production_releases:
    try:
        production_versions.append(Version(r[1:] if r.startswith('v') else r))
    except InvalidVersion:
        continue
keep_newest = True
if production_versions and releases_to_purge:
    keep_newest = Version(releases_to_purge[-1]) > max(production_versions)
if keep_newest and releases_to_purge:
    releases_to_purge.pop()

releases_to_purge = ['v' + r for r in releases_to_purge]

if len(releases_to_purge) == 0:
    print('No releases to purge.')
    exit(0)

print('Releases to purge:')
for r in releases_to_purge:
    print('  ' + r)

for r in releases_to_purge:
    path_to_purge = RELEASES_PATH.joinpath(r)
    print('Deleting ' + str(path_to_purge))
    os.system('rm -rf ' + str(path_to_purge))

print('Done.')
