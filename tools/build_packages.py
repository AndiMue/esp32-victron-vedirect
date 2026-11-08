# -----------------------------------------------------------------------------
# build_packages.py  (PlatformIO extra_script)
#
# Nach jedem Firmware-Build werden im Build-Ordner (.pio/build/esp32dev/)
# die Update-Dateien für die WebUI erzeugt:
#
#   firmware_<Version>.bin   nur Firmware      -> WebUI: "Firmware"
#   webui_<Version>.tar      nur WebUI (web/)  -> WebUI: "WebUI"
#   package_<Version>.tar    firmware.bin+web/ -> WebUI: "Paket"
#
# Version aus data/version.json (erzeugt von build_version.py).
# TAR im USTAR-Format (vom ESP32 direkt beim Hochladen entpackbar).
# -----------------------------------------------------------------------------

Import("env")

import json
import os
import shutil
import tarfile


def make_packages(source, target, env):
    project_dir = env["PROJECT_DIR"]
    build_dir = env.subst("$BUILD_DIR")
    data_dir = os.path.join(project_dir, "data")
    firmware = target[0].get_abspath()

    version = "unbekannt"
    try:
        with open(os.path.join(data_dir, "version.json"), encoding="utf-8") as f:
            version = json.load(f)["version"]
    except Exception:
        pass

    # Alte Pakete entfernen, damit nur die aktuellen herumliegen
    for name in os.listdir(build_dir):
        if name.startswith(("firmware_", "webui_", "package_")):
            os.remove(os.path.join(build_dir, name))

    fw_copy = os.path.join(build_dir, f"firmware_{version}.bin")
    shutil.copyfile(firmware, fw_copy)

    def add_web(tar):
        for name in sorted(os.listdir(data_dir)):
            path = os.path.join(data_dir, name)
            if os.path.isfile(path):
                tar.add(path, arcname="web/" + name)

    webui_tar = os.path.join(build_dir, f"webui_{version}.tar")
    with tarfile.open(webui_tar, "w", format=tarfile.USTAR_FORMAT) as tar:
        add_web(tar)

    package_tar = os.path.join(build_dir, f"package_{version}.tar")
    with tarfile.open(package_tar, "w", format=tarfile.USTAR_FORMAT) as tar:
        tar.add(firmware, arcname="firmware.bin")
        add_web(tar)

    print(f"[PAKETE] {os.path.basename(fw_copy)}, {os.path.basename(webui_tar)}, "
          f"{os.path.basename(package_tar)} in {build_dir}")


env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", make_packages)
