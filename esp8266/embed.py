# PlatformIO pre-build script: embeds ../web/index.html, the gateway image (../gateway_image.py) and the git
# version (#89, as ESP-IDF's PROJECT_VER does for the ESP32) as PROGMEM C headers under include/ so ESP8266 uses the
# exact same single-file web UI and gateway image as ESP32.
import pathlib
import subprocess
import sys

Import('env')  # noqa: F821 (injected by PlatformIO SCons)

project = pathlib.Path(env['PROJECT_DIR'])  # noqa: F821
root = project.parent
html = (root / 'web' / 'index.html').read_bytes()
out = project / 'include' / 'generated_index_html.h'
out.parent.mkdir(parents=True, exist_ok=True)

rows = [', '.join(f'0x{b:02x}' for b in html[i : i + 16]) for i in range(0, len(html), 16)]
body = ',\n    '.join(rows)
header = (
    '#pragma once\n'
    '#include <Arduino.h>\n'
    '#include <pgmspace.h>\n'
    f'static const char INDEX_HTML[{len(html)}] PROGMEM = {{\n    {body}\n}};\n'
)
if not out.exists() or out.read_text() != header:
    out.write_text(header)

subprocess.run(
    [sys.executable, str(root / 'gateway_image.py'), '--header', str(project / 'include' / 'generated_gateway.h')],
    check=True,
)

try:
    ver = subprocess.check_output(['git', 'describe', '--always', '--dirty', '--tags'], cwd=root, text=True).strip()
except (OSError, subprocess.SubprocessError):
    ver = 'unknown'
ver_out = project / 'include' / 'generated_version.h'
ver_header = f'#pragma once\n#define OWS_VERSION "{ver}"\n'
if not ver_out.exists() or ver_out.read_text() != ver_header:
    ver_out.write_text(ver_header)
