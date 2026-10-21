#!/usr/bin/env python3
"""
Copies the maps folder out of the player's Xbox disc image of Halo (an
"xiso", or a whole disc's ".iso") on the Steam Frame's SteamOS side, for
its installer (steam_frame_install.sh), as the app's own import does
(app/src/main/java/com/halo/decomp/XisoExtractor.java, which this follows;
the desktop games' is port/linux/src/xiso.c).

    steam_frame_xiso.py <image> <destination>

writes <destination>/maps (through maps.partial, ui.map moved in last, so
an interrupted copy never leaves maps/ui.map behind) and prints its
progress for zenity --progress: a percentage on a line of its own, and
"# <text>" lines. On failure it prints "error: <reason the player can act
on>" to stderr and exits 1.

Written from a description of the Xbox's disc file system, XDVDFS
(xboxdevwiki's XDVDFS page, https://xboxdevwiki.net/XDVDFS), as
XisoExtractor.java is:

- Sectors are 2048 bytes and integers little-endian. Sector numbers count
  from the start of the game partition: an image of the partition alone has
  it at 0, an image of a whole disc further in (PARTITION_STARTS, tried in
  that order).
- The volume descriptor is the partition's sector 32 (at 0x10000). It holds
  "MICROSOFT*XBOX*MEDIA" at its start and again at 0x7EC, and the root
  directory table's start sector (u32 at 0x14) and size in bytes (u32 at
  0x18).
- A directory table is a binary tree of entries whose root is the entry at
  offset 0. An entry is a left and a right subtree link (u16 each, in 4-byte
  units from the table's start, 0 for none), the start sector and the size
  of the file or of the subdirectory's table (u32 each), the attributes (u8,
  0x10 for a directory), the name's length (u8) and the name. Unused bytes
  are 0xFF, so a left link of 0xFFFF is padding, not an entry.
- A file is one contiguous run of bytes from the start of its sector.

The image is the player's file and nothing read from it is trusted: only
the root's table and the maps folder's are read, each whole and at most
4 MB; a table's walk visits each offset once, goes at most 64 links deep
and looks at most 4096 entries; and a file is written only under a plain
name that no file before it had (ignoring case).

A European disc in two languages (Spanish and Italian) has no maps folder
but one for each, maps_es and maps_it: the one for the language asked for
(MJOLNIR_LANGUAGE, else the system's) is taken, else English's, else the
first, and its language written to maps/language.txt, which the game takes
as the Xbox's language while game.language is unset.
"""

import os
import struct
import sys

SECTOR_SIZE = 2048
DESCRIPTOR_OFFSET = 0x10000
MAGIC = b"MICROSOFT*XBOX*MEDIA"
SECOND_MAGIC_OFFSET = 0x7EC
ENTRY_FIXED_SIZE = 14
ATTRIBUTE_DIRECTORY = 0x10
PADDING_LINK = 0xFFFF
# where the game partition may start: an image of the partition alone, a
# whole XGD2, XGD3 and XGD1 (the original Xbox's) disc
PARTITION_STARTS = (0x00000000, 0x0FD90000, 0x02080000, 0x18300000)
# directory tables are a few sectors; anything much larger is not one
MAXIMUM_TABLE_SIZE = 4 << 20
MAXIMUM_DEPTH = 64
MAXIMUM_VISITS = 4096
MAXIMUM_ENTRIES = 256
COPY_CHUNK = 1 << 20
PARTIAL_DIRECTORY = "maps.partial"


class ExtractError(Exception):
    """a reason the player can act on"""


def read_exactly(image, position, size):
    """size bytes of the image from position; None if it ends first"""
    image.seek(position)
    data = image.read(size)
    return data if len(data) == size else None


def find_partition(image, image_size):
    """the game partition's start, and the root table's sector and size"""
    for start in PARTITION_STARTS:
        if start + DESCRIPTOR_OFFSET + SECTOR_SIZE > image_size:
            continue
        descriptor = read_exactly(image, start + DESCRIPTOR_OFFSET, SECTOR_SIZE)
        if (descriptor is None or descriptor[:len(MAGIC)] != MAGIC or
                descriptor[SECOND_MAGIC_OFFSET:SECOND_MAGIC_OFFSET + len(MAGIC)] != MAGIC):
            continue
        sector, size = struct.unpack_from("<II", descriptor, 0x14)
        return start, sector, size
    raise ExtractError("This is not an Xbox disc image.")


def read_table(image, partition, sector, size, damaged):
    if size == 0 or size > MAXIMUM_TABLE_SIZE:
        raise ExtractError(damaged)
    table = read_exactly(image, partition + sector * SECTOR_SIZE, size)
    if table is None:
        raise ExtractError("Could not read the disc image (is it complete?).")
    return table


def is_plain_name(name):
    """no path, no . or .., and only printable ASCII that every file system
    takes as a plain name"""
    if not name or name in (".", ".."):
        return False
    return all(0x20 <= ord(character) <= 0x7E and character not in '/\\:*?"<>|' for character in name)


def walk_table(table, directories):
    """the folders (or the files) of a table with plain names, as (name,
    sector, size), in the tree's order: each entry's left subtree, the entry,
    its right subtree; a link to padding, past the table or back to an entry
    already visited ends that branch"""
    entries, visited = [], set()
    visits = [0]

    def walk(offset, depth):
        if depth > MAXIMUM_DEPTH or visits[0] >= MAXIMUM_VISITS:
            return
        if offset >= len(table) or len(table) - offset < ENTRY_FIXED_SIZE:
            return
        left, right = struct.unpack_from("<HH", table, offset)
        if left == PADDING_LINK or offset // 4 in visited:
            return
        visited.add(offset // 4)
        visits[0] += 1
        if left:
            walk(left * 4, depth + 1)
        name_length = table[offset + 13]
        if name_length and name_length <= len(table) - offset - ENTRY_FIXED_SIZE:
            is_directory = bool(table[offset + 12] & ATTRIBUTE_DIRECTORY)
            name = table[offset + ENTRY_FIXED_SIZE:offset + ENTRY_FIXED_SIZE + name_length].decode("latin-1")
            # (the destination's file system may ignore case)
            if (is_directory == directories and len(entries) < MAXIMUM_ENTRIES and is_plain_name(name) and
                    all(entry[0].lower() != name.lower() for entry in entries)):
                sector, size = struct.unpack_from("<II", table, offset + 4)
                entries.append((name, sector, size))
        if right:
            walk(right * 4, depth + 1)

    walk(0, 0)
    return entries


def maps_language(name):
    """a language's maps folder's language ("maps_it": "it"), else None"""
    if len(name) == 7 and name[:5].lower() == "maps_" and name[5:].isalpha():
        return name[5:].lower()
    return None


def choose_maps(folders):
    """the maps folder, and its language (None for a plain "maps")"""
    for folder in folders:
        if folder[0].lower() == "maps":
            return folder, None
    languages = [(maps_language(folder[0]), folder) for folder in folders if maps_language(folder[0])]
    wanted = (os.environ.get("MJOLNIR_LANGUAGE") or os.environ.get("LC_ALL") or
        os.environ.get("LC_MESSAGES") or os.environ.get("LANG") or "")[:2].lower()
    for preferred in (wanted, "en"):
        for language, folder in languages:
            if language == preferred:
                return folder, language
    if languages:
        return languages[0][1], languages[0][0]
    raise ExtractError("The disc image has no maps folder: it is not a Halo disc.")


def extract(image_path, destination):
    with open(image_path, "rb") as image:
        image_size = os.fstat(image.fileno()).st_size
        partition, root_sector, root_size = find_partition(image, image_size)

        # the root's maps folder
        root = read_table(image, partition, root_sector, root_size, "The disc image's file system is damaged.")
        (_, maps_sector, maps_size), language = choose_maps(walk_table(root, True))

        # its files
        table = read_table(image, partition, maps_sector, maps_size, "The disc image's maps folder is damaged.")
        files = walk_table(table, False)
        user_interface = next((file for file in files if file[0].lower() == "ui.map"), None)
        if user_interface is None:
            raise ExtractError("The disc image's maps folder has no ui.map: it is not a Halo disc.")
        # (an image that ends before a file does is refused before anything
        # is written; an empty file's sector need not be anywhere)
        for name, sector, size in files:
            if size and partition + sector * SECTOR_SIZE + size > image_size:
                raise ExtractError("Could not read %s from the disc image (is it complete?)." % name)
        total = sum(size for _, _, size in files) or 1

        # the copy, to maps.partial (emptied of what an earlier attempt left)
        partial = os.path.join(destination, PARTIAL_DIRECTORY)
        try:
            os.makedirs(partial, exist_ok=True)
            for leftover in os.listdir(partial):
                os.remove(os.path.join(partial, leftover))
        except OSError:
            raise ExtractError("Could not create %s." % partial)
        done, shown = 0, -1
        for name, sector, size in files:
            path = os.path.join(partial, name)
            position, remaining = partition + sector * SECTOR_SIZE, size
            print("# Copying maps/%s" % name, flush=True)
            try:
                with open(path, "wb") as out:
                    while remaining > 0:
                        count = min(remaining, COPY_CHUNK)
                        data = read_exactly(image, position, count)
                        if data is None:
                            raise ExtractError("Could not read %s from the disc image (is it complete?)." % name)
                        out.write(data)
                        position += count
                        remaining -= count
                        done += count
                        # (zenity closes at 100: that is left to the caller)
                        percent = min(99, done * 100 // total)
                        if percent != shown:
                            print(percent, flush=True)
                            shown = percent
            except OSError:
                raise ExtractError("Could not write %s (is the storage full?)." % path)

    # then into maps (which may be there, empty), the language before and
    # ui.map (the sign that the data is there) last
    finished = os.path.join(destination, "maps")
    try:
        os.makedirs(finished, exist_ok=True)
        language_file = os.path.join(finished, "language.txt")
        if os.path.exists(language_file):
            os.remove(language_file)
        if language:
            with open(language_file, "w") as out:
                out.write(language + "\n")
        order = [file for file in files if file is not user_interface] + [user_interface]
        for name, _, _ in order:
            os.replace(os.path.join(partial, name), os.path.join(finished, name))
        os.rmdir(partial)
    except OSError as error:
        raise ExtractError("Could not move the maps into %s (%s)." % (finished, error.strerror))


def main():
    if len(sys.argv) != 3:
        print("usage: steam_frame_xiso.py <image> <destination>", file=sys.stderr)
        return 2
    try:
        extract(sys.argv[1], sys.argv[2])
    except ExtractError as error:
        print("error: %s" % error, file=sys.stderr)
        return 1
    except OSError as error:
        print("error: Could not open the disc image (%s)." % error.strerror, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
