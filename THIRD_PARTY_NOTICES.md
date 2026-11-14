# Third-party notices

## PawnIO module (AMDFamily17)

The exe embeds `third_party/pawnio/AMDFamily17.bin`, an unmodified, signed module from
[PawnIO.Modules](https://github.com/namazso/PawnIO.Modules) (release 0.2.11) by namazso. It is loaded into the
separately installed [PawnIO](https://pawnio.eu) driver to read the CPU temperature. The module is licensed under
the GNU Lesser General Public License 2.1; the full text is in `third_party/pawnio/COPYING`, and its source code
is `AMDFamily17.p` in the PawnIO.Modules repository. PawnIO itself is not included.

## Device protocols

The Commander Pro and HXi/RMi PSU protocols were implemented from the documentation and source of
[liquidctl](https://github.com/liquidctl/liquidctl) and the Linux kernel's `corsair-cpro` driver. No code from
either project is included.

The AMD temperature register addresses follow
[LibreHardwareMonitor](https://github.com/LibreHardwareMonitor/LibreHardwareMonitor). No code from it is included.

## NVIDIA NVML

The GPU temperature comes from `nvml.dll`, which ships with the NVIDIA display driver and is loaded at runtime.
Nothing from NVIDIA is included.

## Corsair

Corsair, iCUE, Commander Pro, HXi and RMi are trademarks of Corsair. This project is not affiliated with or
endorsed by Corsair. The tray icon is the iCUE icon with a cross over it, used to show that this replaces iCUE.

## SpacemouseHOTAS

The software renderer and widgets (`src/ui`, `src/win32/font_win32.cpp`) come from
[SpacemouseHOTAS](https://github.com/Spirit532/SpacemouseHOTAS) by the same author, MIT licensed.
