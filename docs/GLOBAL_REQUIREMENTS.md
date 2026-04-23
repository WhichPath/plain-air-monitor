# Global Requirements Record

No new global packages were installed for this workspace setup.

Existing host tools used:
- `/usr/bin/git`
- `/usr/bin/cmake`
- `/usr/bin/python3`
- `/usr/bin/pip3`

Workspace-local installs:
- ESP-IDF source: `.tools/esp-idf`
- ESP-IDF toolchains and Python environment: `.tools/espressif`

Potential global setup that may be needed later for flashing:
- Serial device permission, for example adding the user to the appropriate dialout/uucp group.
- udev rules for USB serial/JTAG devices, depending on the Linux distribution and how the T-Display-S3 enumerates.

Do not install those globally unless flashing fails with a permission or device access error.
