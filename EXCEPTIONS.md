# Additional permissions

Skyrim Loading Percent is licensed under **GPL-3.0-or-later** (see [COPYING.txt](COPYING.txt))
**WITH** the two additional permissions below, granted under section 7 of that license.

These are the same exceptions carried by [CommonLibSSE-NG](https://github.com/alandtse/CommonLibSSE-NG/blob/ng/EXCEPTIONS.md),
the library this plugin is built on, and they exist for the same reason: an SKSE plugin is by
definition code that links against a proprietary game and a set of community libraries whose
licenses nobody involved has the right to change.

This Program is intended to be used with and modify existing code (the "Modded Code") and to build
a robust modding community with open source principles. The purpose of this exception is to address
issues when an open source modding community interacts with potentially proprietary code. In
addition, the modding community often uses libraries (the "Modding Libraries") under licenses that
may be incompatible with the GPL ("Modding Library Licenses").

For this Program, the **Modded Code** is:

- The Elder Scrolls V: Skyrim, and its variants (Special Edition, Anniversary Edition, VR)

and the **Modding Libraries** are:

- [SKSE / SKSEVR](https://skse.silverlock.org/)
- Microsoft Windows and its SDK components (including Direct3D, DXGI and XInput)

===

## Modding Exception

In addition, as a special exception, the authors give You the additional right to link the code of
this Program with the existing code that this Program is intended to be used with or modify and to
distribute linked combinations including the two, subject to the limitations in this paragraph.
Modded Code permitted under this exception may link to the code of this Program without causing the
Modded Code and portion of the combined work corresponding to the Modded Code to be covered by the
GNU General Public License. You must obey the GNU General Public License in all respects for all of
the Program code and other code used in conjunction with the Program except the Modded Code covered
by this exception. If you modify this file, you may extend this exception to your version of the
file, but you are not obligated to do so. If you do not wish to provide this exception without
modification, you must delete this exception statement from your version and license this file
solely under the GPL without exception.

===

## GPL-3.0 Linking Exception (with Corresponding Source)

Additional permission under GNU GPL version 3 section 7

If you modify this Program, or any covered work, by linking or combining it with Modding Libraries
(or a modified version thereof), containing parts covered by the terms of Modding Library Licenses,
the licensors of this Program grant you additional permission to convey the resulting work.
Corresponding Source for a non-source form of such a combination shall include the source code for
the parts of Modding Libraries used as well as that of the covered work.

===

## Note on the other bundled dependencies

The remaining libraries this plugin links — CommonLibSSE-NG (GPL-3.0-or-later with these same
exceptions), MinHook (BSD-2-Clause), spdlog (MIT), CSimpleIni (MIT) and DirectXTK (MIT) — are
already GPL-3.0-compatible and do not rely on the exceptions above. They are listed in
[CREDITS.md](CREDITS.md).
