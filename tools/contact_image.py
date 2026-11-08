#!/usr/bin/env python3
"""The image the ESP32 relay reflashes its contact board with (POST /contact/flash, README.md): the boot
selector at 0x0 and a slot image at 0x1000 (the OTA layout), as sbl_common.slots_image writes them.

    uv run python tools/contact_image.py BOOT.bin SLOT_A.img contact.bin
    curl -u admin:<pass> -F firmware=@contact.bin 'http://<relay>/contact/flash?mac=777B42DF'
"""

import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import sbl_common  # noqa: E402

if __name__ == '__main__':
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    image = sbl_common.slots_image(sys.argv[1], sys.argv[2])
    pathlib.Path(sys.argv[3]).write_bytes(image)
    print(f'{sys.argv[3]}: {len(image)} bytes (selector + slot A)')
