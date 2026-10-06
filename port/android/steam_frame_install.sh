#!/bin/bash
# Ringfall: Mjolnir VR's installer for the Steam Frame, run in its Desktop
# Mode from the bundle the release's "Install Ringfall: Mjolnir VR" desktop
# file downloads (steam_frame_installer.desktop; the bundle: this script,
# RingfallMjolnirVR.apk, ringfall-mjolnir-vr.sh = steam_frame_launch.sh,
# steam_frame_xiso.py and the library pictures in grid/). Everything is
# answered with dialogs, no keyboard:
# - the game in ~/Documents/RingfallMjolnirVR (port/android/STEAM_FRAME.md),
#   its APK replaced (Lepton installs a newer one at the next start), so
#   running the installer again updates the game; the folder of the builds
#   before the name, ~/Documents/HaloCE, becomes it;
# - the game data: the maps folder copied out of a disc image of the
#   original Xbox game (.iso or .xiso) found in Downloads, Documents, the
#   Desktop or on a memory card, or chosen, unless it is there already;
# - the game in the Steam library, with its pictures, unless it is there
#   already (the shortcut of the builds before the name replaced): Steam is
#   closed for that (it keeps its shortcuts in memory and writes them back
#   when it quits), and started again in Gaming Mode.
# RINGFALL_UNATTENDED=1 answers every question with its default and prints
# instead of showing dialogs (RINGFALL_IMAGE: the disc image to use), for
# testing over adb; it never closes Steam.

BUNDLE="$(cd "$(dirname "$0")" && pwd)"
GAME_DIR="$HOME/Documents/RingfallMjolnirVR"
STEAM_DIR="$HOME/.local/share/Steam"
LEPTON="$STEAM_DIR/steamapps/common/Lepton/lepton"
TITLE="Ringfall: Mjolnir VR"
NAME="Ringfall: Mjolnir VR"
APK_NAME="RingfallMjolnirVR.apk"
LAUNCHER="ringfall-mjolnir-vr.sh"
EXE="\"$GAME_DIR/$LAUNCHER\""
# the builds before the name: their folder and Steam shortcut
OLD_DIR="$HOME/Documents/HaloCE"
OLD_NAME="Halo VR"
OLD_EXE="\"$OLD_DIR/halo-vr.sh\""

unattended() { [ "${RINGFALL_UNATTENDED:-0}" = 1 ] || ! command -v zenity >/dev/null; }

info()
{
    if unattended; then printf "info: %b\n" "$1"; else zenity --info --title="$TITLE" --width=460 --text="$1"; fi
}

fail()
{
    if unattended; then printf "error: %b\n" "$1" >&2; else zenity --error --title="$TITLE" --width=460 --text="$1"; fi
    exit 1
}

# yes/no; unattended: the default given ($2, "yes" or "no")
ask()
{
    if unattended; then [ "$2" = yes ]; return; fi
    zenity --question --title="$TITLE" --width=460 --text="$1"
}

# the disc image the player chooses
choose_image()
{
    if unattended; then return 1; fi
    zenity --file-selection --title="Choose a disc image of Halo: Combat Evolved for the Xbox (.iso or .xiso)" \
        --filename="$HOME/Downloads/" --file-filter="Disc images | *.iso *.ISO *.xiso *.XISO" \
        --file-filter="All files | *"
}

# the disc images near at hand, largest first (the game's image is a few GB;
# anything under 500 MB is not one)
find_images()
{
    find "$HOME/Downloads" "$HOME/Documents" "$HOME/Desktop" /run/media -maxdepth 3 -type f \
        \( -iname '*.iso' -o -iname '*.xiso' \) -size +500M -printf '%s\t%p\n' 2>/dev/null |
        sort -rn | cut -f2-
}

extract_maps()
{
    local image="$1" log
    log="$(mktemp)"
    if unattended; then
        python3 "$BUNDLE/steam_frame_xiso.py" "$image" "$GAME_DIR" 2>"$log" | grep '^#' | tail -n 1
        status=${PIPESTATUS[0]}
    else
        python3 "$BUNDLE/steam_frame_xiso.py" "$image" "$GAME_DIR" 2>"$log" |
            zenity --progress --title="$TITLE" --width=460 --auto-close --no-cancel \
                --text="Copying the game data out of the disc image..."
        status=${PIPESTATUS[0]}
    fi
    if [ "$status" != 0 ]; then
        reason="$(sed -n 's/^error: //p' "$log" | head -n 1)"
        rm -f "$log"
        fail "The game data could not be copied out of\n$image\n\n${reason:-Unknown error.}"
    fi
    rm -f "$log"
}

# the shortcut's id as Steam makes it for a non-Steam game: its pictures in
# config/grid are named after it
shortcut_id()
{
    python3 -c 'import sys, zlib; print(zlib.crc32((sys.argv[1] + sys.argv[2]).encode()) | 0x80000000)' "$1" "$2"
}

# whether every Steam user's shortcuts.vdf already holds the game
have_shortcut()
{
    local found=1 config
    for config in "$STEAM_DIR"/userdata/*/config; do
        [ -d "$config" ] || continue
        case "$config" in */userdata/0/*) continue ;; esac
        grep -qaF "$GAME_DIR/$LAUNCHER" "$config/shortcuts.vdf" 2>/dev/null || return 1
        found=0
    done
    return $found
}

# the game added to every Steam user's shortcuts.vdf (Steam closed), as a VR
# title, with its pictures; the builds' before the name gone
add_shortcut()
{
    local id old_id config
    id="$(shortcut_id "$EXE" "$NAME")"
    old_id="$(shortcut_id "$OLD_EXE" "$OLD_NAME")"
    for config in "$STEAM_DIR"/userdata/*/config; do
        [ -d "$config" ] || continue
        case "$config" in */userdata/0/*) continue ;; esac
        python3 - "$config/shortcuts.vdf" "$id" "$NAME" "$EXE" "\"$GAME_DIR/\"" "$OLD_EXE" <<'PYTHON' || return 1
import os, struct, sys

path, appid, name, exe, start, old_exe = (sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4], sys.argv[5],
    sys.argv[6])

# Steam's binary VDF: 0x00 a map, 0x01 a string, 0x02 an int32, 0x08 a map's end
def read_map(data, at):
    items = []
    while True:
        kind = data[at]; at += 1
        if kind == 0x08:
            return items, at
        end = data.index(b"\0", at); key = data[at:end]; at = end + 1
        if kind == 0x00:
            value, at = read_map(data, at)
        elif kind == 0x01:
            end = data.index(b"\0", at); value = data[at:end]; at = end + 1
        elif kind == 0x02:
            value = struct.unpack_from("<I", data, at)[0]; at += 4
        else:
            raise ValueError("unknown VDF type %d" % kind)
        items.append((kind, key, value))

def write_map(items):
    out = b""
    for kind, key, value in items:
        out += bytes([kind]) + key + b"\0"
        if kind == 0x00:
            out += write_map(value)
        elif kind == 0x01:
            out += value + b"\0"
        else:
            out += struct.pack("<I", value)
    return out + b"\x08"

shortcuts = []
if os.path.exists(path):
    with open(path, "rb") as file:
        data = file.read()
    if data:
        top, _ = read_map(data, 0)
        for kind, key, value in top:
            if key.lower() == b"shortcuts" and kind == 0x00:
                shortcuts = [entry for entry in value if entry[0] == 0x00]
# (an older entry of the game's replaced, and the builds' before the name)
shortcuts = [entry for entry in shortcuts
    if not any(k.lower() == b"exe" and v in (exe.encode(), old_exe.encode()) for _, k, v in entry[2])]

def text(key, value):
    return (0x01, key.encode(), value.encode())

def number(key, value):
    return (0x02, key.encode(), value)

entry = [
    number("appid", appid), text("AppName", name), text("Exe", exe), text("StartDir", start),
    text("icon", ""), text("ShortcutPath", ""), text("LaunchOptions", ""), number("IsHidden", 0),
    number("AllowDesktopConfig", 1), number("AllowOverlay", 1), number("OpenVR", 1), number("Devkit", 0),
    text("DevkitGameID", ""), number("DevkitOverrideAppID", 0), number("LastPlayTime", 0),
    text("FlatpakAppID", ""), text("sortas", ""), (0x00, b"tags", []),
]
shortcuts.append((0x00, b"", entry))
shortcuts = [(0x00, str(index).encode(), value) for index, (_, _, value) in enumerate(shortcuts)]
data = write_map([(0x00, b"shortcuts", shortcuts)])
with open(path + ".tmp", "wb") as file:
    file.write(data)
os.replace(path + ".tmp", path)
PYTHON
        rm -f "$config/grid/${old_id}.png" "$config/grid/${old_id}p.png" "$config/grid/${old_id}_hero.png" \
            "$config/grid/${old_id}_logo.png"
        mkdir -p "$config/grid"
        cp "$BUNDLE/grid/library.png" "$config/grid/${id}.png"
        cp "$BUNDLE/grid/libraryp.png" "$config/grid/${id}p.png"
        cp "$BUNDLE/grid/library_hero.png" "$config/grid/${id}_hero.png"
        cp "$BUNDLE/grid/library_logo.png" "$config/grid/${id}_logo.png"
    done
}

# --- the game

[ -x "$LEPTON" ] || fail "Lepton, which runs Android games on the Steam Frame, was not found.\n\nStart any Android game from the Steam store once, then run this installer again."
for file in "$APK_NAME" "$LAUNCHER" steam_frame_xiso.py grid/library.png; do
    [ -f "$BUNDLE/$file" ] || fail "The installer is incomplete ($file is missing). Download it again."
done

# the builds before the name: their folder becomes the game's (settings,
# saves and game data kept), without their APK and launcher
if [ -d "$OLD_DIR" ] && [ ! -e "$GAME_DIR" ]; then
    mv "$OLD_DIR" "$GAME_DIR" || fail "Could not rename $OLD_DIR to $GAME_DIR."
    rm -f "$GAME_DIR/HaloVR.apk" "$GAME_DIR/halo-vr.sh"
fi

if [ -f "$GAME_DIR/$APK_NAME" ] || [ -f "$GAME_DIR/maps/ui.map" ]; then
    ask "$NAME is installed already. Update it to this version?\n\n(Your settings, saves and game data stay.)" yes || exit 0
else
    ask "This installs $NAME on your Steam Frame.\n\nYou need your own disc image of Halo: Combat Evolved for the original Xbox (an .iso or .xiso file, any version), for example in Downloads.\n\nContinue?" yes || exit 0
fi

mkdir -p "$GAME_DIR/maps" || fail "Could not create $GAME_DIR."
# (the game runs in Lepton's container as a user of its own)
chmod 777 "$GAME_DIR" "$GAME_DIR/maps"
cp "$BUNDLE/$LAUNCHER" "$GAME_DIR/$LAUNCHER.tmp" && chmod 755 "$GAME_DIR/$LAUNCHER.tmp" &&
    mv -f "$GAME_DIR/$LAUNCHER.tmp" "$GAME_DIR/$LAUNCHER" || fail "Could not write $GAME_DIR/$LAUNCHER."
# (newer than the installed one: Lepton installs it at the next start)
cp "$BUNDLE/$APK_NAME" "$GAME_DIR/$APK_NAME.tmp" && chmod 666 "$GAME_DIR/$APK_NAME.tmp" &&
    mv -f "$GAME_DIR/$APK_NAME.tmp" "$GAME_DIR/$APK_NAME" && touch "$GAME_DIR/$APK_NAME" ||
    fail "Could not write $GAME_DIR/$APK_NAME (is the storage full?)."

# --- the game data

if [ ! -f "$GAME_DIR/maps/ui.map" ]; then
    image="${RINGFALL_IMAGE:-}"
    if [ -z "$image" ]; then
        mapfile -t images < <(find_images)
        for candidate in "${images[@]}"; do
            if ask "Use this disc image?\n\n$candidate" yes; then
                image="$candidate"
                break
            fi
        done
    fi
    if [ -z "$image" ]; then
        if [ ${#images[@]} = 0 ]; then
            info "No disc image was found in Downloads, Documents, on the Desktop or a memory card.\n\nChoose it in the next window."
        fi
        image="$(choose_image)" || fail "No disc image was chosen. Run the installer again once it is on the Frame (for example in Downloads)."
    fi
    extract_maps "$image"
    chmod -R a+rwX "$GAME_DIR/maps"
    image_note="\n\nThe disc image itself is no longer needed:\n$image"
fi

# --- Steam

steam_closed=0
if ! have_shortcut; then
    if unattended; then
        echo "info: the Steam shortcut is missing (not added unattended)"
    else
        info "$NAME is now added to your Steam library. Steam closes for a moment for that."
        if pgrep -x steam >/dev/null; then
            steam -shutdown >/dev/null 2>&1 &
            for _ in $(seq 60); do pgrep -x steam >/dev/null || break; sleep 1; done
            pgrep -x steam >/dev/null && fail "Steam did not close. Close it (Steam menu > Exit) and run the installer again."
        fi
        steam_closed=1
        add_shortcut || fail "Could not add $NAME to the Steam library."
    fi
fi

if unattended; then
    echo "info: installed in $GAME_DIR"
    exit 0
fi
if ask "$NAME is installed.${image_note:-}\n\nSwitch to Gaming Mode now? Start $NAME from your library there." yes; then
    steamos-session-select gamescope >/dev/null 2>&1 ||
        info "Switch to Gaming Mode yourself (the \"Return to Gaming Mode\" icon or the power menu)."
elif [ "$steam_closed" = 1 ]; then
    setsid steam >/dev/null 2>&1 < /dev/null &
fi
