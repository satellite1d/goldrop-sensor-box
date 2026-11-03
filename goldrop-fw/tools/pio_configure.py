"""Invalidate PlatformIO's CMake cache when release/key/component inputs change."""
from pathlib import Path
import hashlib

Import("env")

project = Path(env.subst("$PROJECT_DIR"))
build = Path(env.subst("$BUILD_DIR"))
inputs = [project / name for name in ("CMakeLists.txt", "main/CMakeLists.txt", "version.txt",
                                     "sdkconfig.defaults", "partitions.csv", "platformio.ini", "config/ota_public_key.pem",
                                     "tools/pio_configure.py")]
inputs += list((project / "components").glob("*/CMakeLists.txt"))
inputs += list((project / "components").glob("*/idf_component.yml"))
inputs += list((project / "components").glob("*/*.in"))
digest = hashlib.sha256()
for path in sorted(set(inputs)):
    digest.update(str(path.relative_to(project)).encode())
    digest.update(b"\0")
    digest.update(path.read_bytes() if path.exists() else b"<missing>")
    digest.update(b"\0")
fingerprint = digest.hexdigest()
stamp = build / "goldrop-configure.sha256"
if not stamp.exists() or stamp.read_text().strip() != fingerprint:
    # Only generated configuration is removed; source, keys and firmware files are preserved.
    (build / "CMakeCache.txt").unlink(missing_ok=True)
    print("Goldrop configuration inputs changed; reconfiguring ESP-IDF")
board = env.BoardConfig()
arguments = board.get("build.cmake_extra_args", "")
board.update("build.cmake_extra_args", arguments + " -DGOLDROP_PIO_CONFIG_FINGERPRINT=" + fingerprint)
