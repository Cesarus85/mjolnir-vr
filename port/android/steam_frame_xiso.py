#!/usr/bin/env python3
"""
Copies the maps folder out of an Xbox disc image (an "xiso", or a whole
disc's ".iso") on the Steam Frame's SteamOS side, for its installer
(steam_frame_install.sh). It reads the image as the app's own import does
(app/src/main/java/com/halo/decomp/XisoExtractor.java, which this follows;
the desktop games' is port/linux/src/xiso.c).

    steam_frame_xiso.py <image> <destination>

writes <destination>/maps (through maps.partial, so an interrupted copy
never leaves maps/ui.map behind) and prints its progress for zenity
--progress: a percentage on a line of its own, and "# <text>" lines. On
failure it prints "error: <reason the player can act on>" to stderr and
exits 1.

The image's file system is XDVDFS, read as extract-xiso does
(https://github.com/XboxDev/extract-xiso, extract-xiso.c, whose format
handling this follows; its license is below and in
port/third_party/extract-xiso/LICENSE.TXT): 2048-byte sectors; a volume
descriptor at 0x10000 that starts and ends with "MICROSOFT*XBOX*MEDIA" and
gives the root directory's sector and size; and directories whose entries
form a binary tree (each entry: left and right subtree offsets in 4-byte
units, the start sector, the size, attributes, the name's length and the
name). Images made from a whole disc put the game partition further in, at
one of the offsets below.

Some parts of this code are copyright in@fishtank.com. This product
includes software developed by in <in@fishtank.com>.

Copyright (c) 2003 in <in@fishtank.com>
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

1. Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.

3. All advertising materials mentioning features or use of this software
   must display the following acknowledgement:

   This product includes software developed by in <in@fishtank.com>.

4. Neither the name of "in" nor the email address "in@fishtank.com"
   may be used to endorse or promote products derived from this software
   without specific prior written permission.

THIS SOFTWARE IS PROVIDED `AS IS' AND ANY EXPRESS OR IMPLIED WARRANTIES
INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE
AUTHOR OR ANY CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
"""

import os
import struct
import sys

SECTOR_SIZE = 2048
VOLUME_DESCRIPTOR_OFFSET = 0x10000
ENTRY_HEADER_SIZE = 14
ATTRIBUTE_DIRECTORY = 0x10
# directory tables are a few sectors; anything much larger is not one
MAXIMUM_DIRECTORY_SIZE = 4 << 20
MAXIMUM_FILES = 256
COPY_BUFFER_SIZE = 1 << 20
VOLUME_MAGIC = b"MICROSOFT*XBOX*MEDIA"
# where the game partition starts: a plain image, and whole-disc images
# (extract-xiso's GLOBAL, XGD3 and XGD1 offsets)
PARTITION_OFFSETS = (0, 0x0FD90000, 0x02080000, 0x18300000)


class ExtractError(Exception):
    """a reason the player can act on"""


def read_at(image, offset, size):
    image.seek(offset)
    data = image.read(size)
    if len(data) != size:
        raise ExtractError("Could not read the disc image (is it complete?).")
    return data


def find_volume(image):
    """the game partition's offset, and the root directory's sector and size"""
    for offset in PARTITION_OFFSETS:
        try:
            descriptor = read_at(image, offset + VOLUME_DESCRIPTOR_OFFSET, SECTOR_SIZE)
        except ExtractError:
            continue
        if descriptor[:20] != VOLUME_MAGIC or descriptor[0x7EC:0x7EC + 20] != VOLUME_MAGIC:
            continue
        sector, size = struct.unpack_from("<II", descriptor, 20)
        return offset, sector, size
    raise ExtractError("This is not an Xbox disc image.")


def read_directory(image, partition, sector, size, what):
    if size <= 0 or size > MAXIMUM_DIRECTORY_SIZE:
        raise ExtractError(what)
    return read_at(image, partition + sector * SECTOR_SIZE, size)


def walk(table, offset, depth, directories, out, visited):
    """collects the entries (name, sector, size) of the subtree at offset
    (4-byte units), files or directories as asked"""
    at = offset * 4
    # a malformed table must not loop or run off its end
    visited[0] += 1
    if depth > 64 or visited[0] > 4096 or at + ENTRY_HEADER_SIZE > len(table):
        return
    left, right, sector, size, attributes, name_length = struct.unpack_from("<HHIIBB", table, at)
    # 0xFFFF: padding, an empty directory
    if left == 0xFFFF:
        return
    if left:
        walk(table, left, depth + 1, directories, out, visited)
    is_directory = bool(attributes & ATTRIBUTE_DIRECTORY)
    if (at + ENTRY_HEADER_SIZE + name_length <= len(table) and name_length > 0
            and is_directory == directories and len(out) < MAXIMUM_FILES):
        name = table[at + ENTRY_HEADER_SIZE:at + ENTRY_HEADER_SIZE + name_length].decode("latin-1")
        # (as extract-xiso refuses them: no name may leave the folder)
        if name not in (".", "..") and "/" not in name and "\\" not in name:
            out.append((name, sector, size))
    if right:
        walk(table, right, depth + 1, directories, out, visited)


def extract(image_path, destination):
    with open(image_path, "rb") as image:
        partition, root_sector, root_size = find_volume(image)

        # the root's maps folder
        table = read_directory(image, partition, root_sector, root_size,
            "The disc image's file system is damaged.")
        directories = []
        walk(table, 0, 0, True, directories, [0])
        maps = [entry for entry in directories if entry[0].lower() == "maps"]
        if not maps:
            raise ExtractError("The disc image has no maps folder: it is not a Halo disc.")

        # its files
        table = read_directory(image, partition, maps[-1][1], maps[-1][2],
            "The disc image's maps folder is damaged.")
        files = []
        walk(table, 0, 0, False, files, [0])
        if not any(name.lower() == "ui.map" for name, _, _ in files):
            raise ExtractError("The disc image's maps folder has no ui.map: it is not a Halo disc.")
        total = sum(size for _, _, size in files) or 1

        partial = os.path.join(destination, "maps.partial")
        finished = os.path.join(destination, "maps")
        try:
            os.makedirs(partial, exist_ok=True)
        except OSError:
            raise ExtractError("Could not create %s." % partial)
        done = 0
        shown = -1
        for name, sector, size in files:
            path = os.path.join(partial, name)
            offset = partition + sector * SECTOR_SIZE
            remaining = size
            print("# Copying maps/%s" % name, flush=True)
            try:
                with open(path, "wb") as out:
                    while remaining > 0:
                        count = min(remaining, COPY_BUFFER_SIZE)
                        out.write(read_at(image, offset, count))
                        offset += count
                        remaining -= count
                        done += count
                        # (zenity closes at 100: that is left to the caller)
                        percent = min(99, done * 100 // total)
                        if percent != shown:
                            print(percent, flush=True)
                            shown = percent
            except ExtractError:
                raise ExtractError("Could not read %s from the disc image (is it complete?)." % name)
            except OSError:
                raise ExtractError("Could not write %s (is the storage full?)." % path)

    # (the maps folder may be there, empty)
    try:
        os.makedirs(finished, exist_ok=True)
        for name, _, _ in files:
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
