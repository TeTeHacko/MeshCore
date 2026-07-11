# CUSTOM (TeTeHacko): custom version and build date/time for the `ver` command.
# Injects FIRMWARE_VERSION (v1.16.0-tth-<git sha>, '+' = dirty tree) and
# FIRMWARE_BUILD_DATE (compile time). NOTE: the define changes every build =
# full rebuild each time (~10-30 s) -- the price for an always-fresh timestamp.
Import("env")
import subprocess
from datetime import datetime

sha = "nogit"
dirty = ""
try:
    cwd = env.subst("$PROJECT_DIR")
    sha = subprocess.check_output(["git", "rev-parse", "--short", "HEAD"], cwd=cwd).decode().strip()
    if subprocess.call(["git", "diff", "--quiet"], cwd=cwd) != 0:
        dirty = "+"
except Exception:
    pass

version = "v1.16.0-tth-%s%s" % (sha, dirty)
build_date = datetime.now().strftime("%d %b %Y %H:%M")
env.Append(CPPDEFINES=[
    ("FIRMWARE_VERSION", env.StringifyMacro(version)),
    ("FIRMWARE_BUILD_DATE", env.StringifyMacro(build_date)),
])
print("build_version.py: %s (Build: %s)" % (version, build_date))
