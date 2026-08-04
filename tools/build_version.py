# CUSTOM (TeTeHacko): real version and build timestamp for the `ver` command.
#
# WIRE THIS INTO EVERY ENV WE FLASH. Without it the node reports the fallback
# literal from examples/*/MyMesh.h -- "v1.16.0", "6 Jun 2026" -- which is the
# same in every build ever made, so `ver` cannot tell one flash from another.
# That is not a cosmetic problem: after a 14-minute BLE DFU onto the T1000-E,
# every field the node reported back was byte-for-byte what it said beforehand,
# and the only evidence the image had landed was a line in a scrollback buffer.
#
#   extra_scripts = ${nrf52_base.extra_scripts}
#     pre:tools/build_version.py
#
# Injects FIRMWARE_VERSION (<upstream tag>-tth-<git sha>, '+' = dirty tree) and
# FIRMWARE_BUILD_DATE (compile time). NOTE: the define changes every build =
# full rebuild each time (~10-30 s) -- the price for an always-fresh timestamp.
Import("env")
import subprocess
from datetime import datetime


def git(cwd, *args):
    return subprocess.check_output(("git",) + args, cwd=cwd,
                                   stderr=subprocess.DEVNULL).decode().strip()


cwd = env.subst("$PROJECT_DIR")

# The base version follows the newest upstream release tag that HEAD descends
# from, rather than being written here as a literal -- a literal would keep
# claiming v1.16.0 long after upstream moved on, which is exactly the kind of
# stale-by-construction string this script exists to get rid of.
# Upstream tags them per-example ("companion-v1.16.0", "repeater-v1.16.0"), so
# match anything carrying a v<digit> and strip the prefix. Our own release tags
# (`...+tth.<timestamp>`) are excluded so they cannot shadow the upstream base.
base = "nover"
try:
    tag = git(cwd, "describe", "--tags", "--abbrev=0",
              "--match", "*v[0-9]*", "--exclude", "*+tth*")
    i = tag.find("v")
    base = tag[i:] if i >= 0 else tag
except Exception:
    pass

sha, dirty = "nogit", ""
try:
    sha = git(cwd, "rev-parse", "--short=7", "HEAD")
    if subprocess.call(["git", "diff", "--quiet"], cwd=cwd) != 0:
        dirty = "+"
except Exception:
    pass

# Both strings have to survive the companion protocol's fixed-width fields
# (RESP_CODE_DEVICE_INFO in companion_radio/MyMesh.cpp): 20 B for the version
# and 12 B for the build date, NUL included, so 19 and 11 usable characters.
# Anything longer is silently truncated on the wire -- which first showed up as
# a node reporting "v1.16.0-tth-956238c" for SHA 956238c3, and a build date of
# "04 Aug 2026 " with the time cut clean off. The repeater's text console has no
# such limit, but one format everywhere beats two.
VER_LIMIT, DATE_LIMIT = 19, 11

# Budget: base and the dirty marker are never sacrificed -- a lost '+' would
# turn an uncommitted build into something that looks reproducible. The SHA
# gives up characters instead; git needs 7 for uniqueness in a repo this size,
# so this only bites if upstream's version string grows unusually long.
marker = "-tth"
room = VER_LIMIT - len(base) - len(marker) - len(dirty)
version = "%s%s%s%s" % (base, marker, sha[:max(4, room)], dirty)
if len(version) > VER_LIMIT:
    print("build_version.py: WARNING: %r is %d chars, will truncate to %d on "
          "the companion protocol" % (version, len(version), VER_LIMIT))

# yymmdd hhmm = exactly 11. Not pretty, but complete and sortable; the readable
# "%d %b %Y %H:%M" is 17 and lost its time to the 12-byte field.
build_date = datetime.now().strftime("%y%m%d %H%M")[:DATE_LIMIT]
env.Append(CPPDEFINES=[
    ("FIRMWARE_VERSION", env.StringifyMacro(version)),
    ("FIRMWARE_BUILD_DATE", env.StringifyMacro(build_date)),
])
print("build_version.py: %s (Build: %s)" % (version, build_date))
