# -----------------------------------------------------------------------------
# build_version.py  (PlatformIO extra_script, "pre:")
#
# Erzeugt bei jedem Build eine Versionsnummer im Format JJ.MM.TT-HHMM,
# z. B. 26.11.08-1612, und schreibt sie in
#   - src/BuildVersion.h   -> Firmware-Version (FW_VERSION)
#   - data/version.json    -> WebUI-Version (wird mit hochgeladen/gepackt)
#
# Beide Dateien sind generiert und stehen in .gitignore.
# Die Dateien werden nur neu geschrieben, wenn sich die Version ändert,
# damit nicht unnötig neu übersetzt wird.
# -----------------------------------------------------------------------------

Import("env")

import json
import os
from datetime import datetime

project_dir = env["PROJECT_DIR"]
version = datetime.now().strftime("%y.%m.%d-%H%M")


def write_if_changed(path, content):
    if os.path.exists(path):
        with open(path, "r", encoding="utf-8") as f:
            if f.read() == content:
                return
    with open(path, "w", encoding="utf-8") as f:
        f.write(content)


header = (
    "// Automatisch erzeugt von tools/build_version.py - nicht von Hand ändern.\n"
    "#pragma once\n"
    f"#define FW_VERSION \"{version}\"\n"
)
write_if_changed(os.path.join(project_dir, "src", "BuildVersion.h"), header)

data_dir = os.path.join(project_dir, "data")
if os.path.isdir(data_dir):
    write_if_changed(os.path.join(data_dir, "version.json"),
                     json.dumps({"version": version}) + "\n")

print(f"[VERSION] {version}")
