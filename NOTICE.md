# Third-party assets

## sounds/ — EdgeTX voice pack

Voice clips are taken from the **EdgeTX sdcard sounds** pack, English voice,
release **v2.12.0**:

  https://github.com/EdgeTX/edgetx-sdcard-sounds

Included: 16 word clips (`armed`, `disarm`, `lowbat`, `battry`, `batalert`,
`warnng`, `volt0/1`, `percent0/1`, `rssi_org`, `rssi_red`, `telemko`, `telemok`,
`lost`, `lowbatt`) and `sounds/num/0000.wav`..`0100.wav`.

They are used the way EdgeTX/OpenTX uses them — announcements are CONCATENATED
from a word plus a number plus a unit ("battery" + "fifteen" + "volts"), which is
why the numbers are one clip per value rather than per digit.

EdgeTX is licensed under the **GPL-2.0**; these assets are redistributed under
that licence. See the upstream repository for the full text and for the other
languages/voices available (the pack ships cn, cz, da, de, en, en_gb variants).

## fonts/ — OSD glyph atlases

Betaflight OSD font atlases from:

  https://github.com/xNuclearSquirrel/o3-multipage-osd

Only the `BTFL_*_HD` (4-page, 96x9216) variants are usable with this player's
atlas loader; `_SD`/`INAV`/`ARDU` variants in that repo are 1- or 2-page and
would be sliced incorrectly. See the loader comment in `apfpv_player.cpp`.

Refer to the upstream repository for licence terms.
