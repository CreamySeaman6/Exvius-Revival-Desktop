"""Is this APK the game this program runs: FINAL FANTASY BRAVE EXVIUS 9.1.0, Global, 64-bit ARM?

Two looks, both without running anything:

* `apk_version`: the version the APK declares (`android:versionName`, `android:versionCode` in the binary
  AndroidManifest.xml). The game's own `manifest.json` in an .xapk says the same; the APK's manifest is read
  because every kind of package has one.
* `library_problem`: the game library itself. The 9.1.0 library carries the C++ runtime inside it (it defines
  `_Znwm`, C++'s `operator new`, and needs only Android's C, GL, sound and log libraries). Older builds of the game
  take the C++ runtime from a separate Android library (`libc++_shared.so`) that this program does not have, so
  their first allocation returns nothing and the game crashes before its first frame ("guest stopped: invalid memory
  write at 0x0", with "[stub] first call: _Znwm" just before it in client\\runner.log). A 32-bit library or one for
  another processor is caught here too.
"""
from __future__ import annotations

import struct

GAME_VERSION = '9.1.0'
NEEDED_ALLOWED = {'libOpenSLES.so', 'libGLESv2.so', 'libEGL.so', 'liblog.so', 'libandroid.so', 'libdl.so', 'libc.so',
                  'libm.so', 'libz.so', 'libstdc++.so', 'libGLESv1_CM.so', 'libGLESv3.so', 'libjnigraphics.so'}

ATTR_VERSION_CODE = 0x0101021B
ATTR_VERSION_NAME = 0x0101021C


# ----------------------------------------------------------------------------------------------------
# The APK's manifest (Android's binary XML)
# ----------------------------------------------------------------------------------------------------
def _strings(chunk: bytes) -> list[str]:
    count, _styles, flags, start, _ = struct.unpack_from('<IIIII', chunk, 8)
    utf8 = bool(flags & 0x100)
    out = []
    for i in range(count):
        at = start + struct.unpack_from('<I', chunk, 28 + 4 * i)[0]
        try:
            if utf8:
                n = chunk[at]
                at += 2 if n & 0x80 else 1                       # length in UTF-16 units, then in bytes
                n = chunk[at]
                if n & 0x80:
                    n = ((n & 0x7F) << 8) | chunk[at + 1]
                    at += 1
                at += 1
                out.append(chunk[at:at + n].decode('utf-8', 'replace'))
            else:
                n = struct.unpack_from('<H', chunk, at)[0]
                at += 2
                if n & 0x8000:
                    n = ((n & 0x7FFF) << 16) | struct.unpack_from('<H', chunk, at)[0]
                    at += 2
                out.append(chunk[at:at + 2 * n].decode('utf-16-le', 'replace'))
        except (IndexError, struct.error):
            out.append('')
    return out


def manifest_version(data: bytes) -> tuple[str | None, int | None]:
    """(versionName, versionCode) from a binary AndroidManifest.xml; None for what is not there."""
    if len(data) < 8 or struct.unpack_from('<H', data, 0)[0] != 0x0003:
        return None, None
    strings: list[str] = []
    resource_ids: list[int] = []
    at = struct.unpack_from('<H', data, 2)[0]
    while at + 8 <= len(data):
        kind, head, size = struct.unpack_from('<HHI', data, at)
        if size < 8:
            break
        chunk = data[at:at + size]
        if kind == 0x0001:
            strings = _strings(chunk)
        elif kind == 0x0180:
            resource_ids = list(struct.unpack_from(f'<{(size - head) // 4}I', chunk, head))
        elif kind == 0x0102:                                    # the first element is <manifest>
            name = struct.unpack_from('<I', chunk, 20)[0]
            if name < len(strings) and strings[name] != 'manifest':
                return None, None
            attr_start, attr_size, attr_count = struct.unpack_from('<HHH', chunk, 24)
            version_name = version_code = None
            for i in range(attr_count):
                a = 16 + attr_start + i * attr_size
                _ns, key, raw, _sz, _res0, dtype, value = struct.unpack_from('<IIIHBBI', chunk, a)
                rid = resource_ids[key] if key < len(resource_ids) else 0
                label = strings[key] if key < len(strings) else ''
                if rid == ATTR_VERSION_NAME or label == 'versionName':
                    index = raw if raw != 0xFFFFFFFF else (value if dtype == 0x03 else None)
                    if index is not None and index < len(strings):
                        version_name = strings[index]
                elif rid == ATTR_VERSION_CODE or label == 'versionCode':
                    if dtype in (0x10, 0x11):
                        version_code = value
            return version_name, version_code
        at += size
    return None, None


def apk_version(packages) -> tuple[str | None, int | None]:
    """The version the APK (or the .xapk's base APK) declares. `packages`: open zipfile.ZipFile objects."""
    name = code = None
    for package in packages:
        try:
            data = package.read('AndroidManifest.xml')
        except (KeyError, OSError, ValueError):
            continue
        try:
            n, c = manifest_version(data)
        except (struct.error, IndexError, ValueError):
            continue
        name = name or n
        code = code or c
    return name, code


# ----------------------------------------------------------------------------------------------------
# The game library (ELF)
# ----------------------------------------------------------------------------------------------------
def library_problem(path) -> str | None:
    """Why this game library cannot run here, in plain words, or None when nothing was found against it."""
    try:
        with open(path, 'rb') as fh:
            head = fh.read(64)
            if head[:4] != b'\x7fELF':
                return 'The game library in this APK is not a program library at all.'
            if head[4] != 2 or struct.unpack_from('<H', head, 18)[0] != 183:
                return 'The game library in this APK is not the 64-bit ARM one.'
            shoff, = struct.unpack_from('<Q', head, 40)
            shentsize, shnum = struct.unpack_from('<HH', head, 58)
            fh.seek(shoff)
            table = fh.read(shentsize * shnum)
            sections = [struct.unpack_from('<IIQQQQIIQQ', table, i * shentsize) for i in range(shnum)]

            def read(index):
                s = sections[index]
                fh.seek(s[4])
                return fh.read(s[5])
            needed, defines_new, imports = set(), False, 0
            for s in sections:
                if s[1] == 11:                                    # SHT_DYNSYM
                    symbols, names = read(sections.index(s)), read(s[6])
                    for i in range(1, len(symbols) // 24):
                        st_name, info, _other, shndx = struct.unpack_from('<IBBH', symbols, i * 24)
                        if shndx == 0:
                            imports += 1
                        if names[st_name:st_name + 6] == b'_Znwm\0':
                            defines_new = shndx != 0
                elif s[1] == 6:                                   # SHT_DYNAMIC
                    dynamic, names = read(sections.index(s)), read(s[6])
                    for i in range(len(dynamic) // 16):
                        tag, value = struct.unpack_from('<qQ', dynamic, i * 16)
                        if tag == 0:
                            break
                        if tag == 1:                              # DT_NEEDED
                            needed.add(names[value:names.index(b'\0', value)].decode('ascii', 'replace'))
    except (OSError, struct.error, ValueError, IndexError) as exc:
        return f'The game library in this APK could not be read ({exc}).'
    other = sorted(needed - NEEDED_ALLOWED)
    if other or not defines_new:
        what = f'it needs {", ".join(other)}' if other else 'it takes the C++ runtime from a separate library'
        return (f'The game library in this APK is a different build from the {GAME_VERSION} one ({what}), so it '
                f'crashes as soon as it starts. It usually means an older version of the game, or one labelled '
                f'{GAME_VERSION} that is not. Use the FINAL FANTASY BRAVE EXVIUS {GAME_VERSION} APK, Global version: in it, '
                f'lib/arm64-v8a/libgame.so is 60,410,728 bytes.')
    return None
