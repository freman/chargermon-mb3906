Import("env")
import os
import datetime

# Write a build stamp into a generated header every build. PlatformIO/SCons keys
# recompilation off file *content* (not mtime), so touching main.c did nothing and
# its __DATE__/__TIME__ went stale. A header whose content changes each build forces
# main.c to recompile, keeping the /api/info + boot-log build stamp truthful.
stamp = datetime.datetime.now().strftime("%b %d %Y %H:%M:%S")
path = os.path.join(env["PROJECT_DIR"], "src", "build_info.h")
with open(path, "w") as f:
    f.write('#pragma once\n#define FW_BUILD_STAMP "%s"\n' % stamp)

