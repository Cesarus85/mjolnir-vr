#!/bin/bash
# Mjolnir VR's installer for the Steam Frame, run in its Desktop
# Mode from the bundle the release's "Install Mjolnir VR" desktop
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
#   already: added by Steam itself (steam://addnonsteamgame, as SteamOS's
#   "Add to Steam" does), from a desktop file beside the game. Steam is not
#   closed: it keeps its shortcuts in memory and writes them back, and on the
#   Steam Frame Desktop Mode runs inside Gaming Mode's session, which ends
#   with Steam. The shortcut of the builds before the name ("Halo VR") is
#   left for the player to remove (Steam would write it back).
# MJOLNIR_UNATTENDED=1 answers every question with its default and prints
# instead of showing dialogs (MJOLNIR_IMAGE: the disc image to use), for
# testing over adb; it never closes Steam.

BUNDLE="$(cd "$(dirname "$0")" && pwd)"
GAME_DIR="$HOME/Documents/RingfallMjolnirVR"
STEAM_DIR="$HOME/.local/share/Steam"
LEPTON="$STEAM_DIR/steamapps/common/Lepton/lepton"
TITLE="Mjolnir VR"
NAME="Mjolnir VR"
APK_NAME="RingfallMjolnirVR.apk"
LAUNCHER="ringfall-mjolnir-vr.sh"
EXE="\"$GAME_DIR/$LAUNCHER\""
# (the desktop file Steam adds the game from)
NAME_FILE="Mjolnir VR"
# the builds before the name: their folder and Steam shortcut
OLD_DIR="$HOME/Documents/HaloCE"
OLD_NAME="Halo VR"
# the name of builds 302 to 313, which their shortcut keeps (Steam writes
# its shortcuts back from memory: the player renames it)
FORMER_NAME="Ringfall: Mjolnir VR"

# what the installer did, for looking into problems: written to /tmp while
# it runs, then kept beside the game as install.log
LOG="/tmp/mjolnir-vr-install.log"
: > "$LOG"
log() { echo "$(date '+%F %T') $*" >> "$LOG"; }
keep_log() { [ -d "$GAME_DIR" ] && cp -f "$LOG" "$GAME_DIR/install.log" 2>/dev/null; }
trap keep_log EXIT
log "installer from $BUNDLE"

# whether Steam runs: its pid file (~/.steam/steam.pid), or its client's
# process (ubuntu12_32/steam: the process is not always named "steam")
steam_running()
{
    local pid
    pid="$(cat "$HOME/.steam/steam.pid" 2>/dev/null)"
    if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
        return 0
    fi
    pgrep -f 'ubuntu12_32/steam( |$)' >/dev/null || pgrep -x steam >/dev/null
}

unattended() { [ "${MJOLNIR_UNATTENDED:-0}" = 1 ] || ! command -v zenity >/dev/null; }

info()
{
    if unattended; then printf "info: %b\n" "$1"; else zenity --info --title="$TITLE" --width=460 --text="$1"; fi
}

fail()
{
    log "error: $1"
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

# the Steam users' shortcut files (userdata/0 is no user)
shortcut_files()
{
    local config
    for config in "$STEAM_DIR"/userdata/*/config; do
        case "$config" in */userdata/0/*) continue ;; esac
        [ -f "$config/shortcuts.vdf" ] && echo "$config/shortcuts.vdf"
    done
}

# whether a Steam user's library holds the launcher given
have_shortcut_to()
{
    local file
    while read -r file; do
        grep -qaF "$1" "$file" && return 0
    done < <(shortcut_files)
    return 1
}

have_shortcut() { have_shortcut_to "$GAME_DIR/$LAUNCHER"; }

# the id Steam gave the shortcut whose Exe is $2, in shortcuts.vdf $1 (Steam
# picks it; its library pictures in config/grid are named after it)
shortcut_id()
{
    python3 - "$1" "$2" <<'PYTHON'
import struct, sys

path, exe = sys.argv[1], sys.argv[2].encode()

# Steam's binary VDF: 0x00 a map, 0x01 a string, 0x02 an int32, 0x08 a map's end
def read_map(data, at):
    items = []
    while True:
        kind = data[at]; at += 1
        if kind == 0x08:
            return items, at
        end = data.index(b"\0", at); key = data[at:end].lower(); at = end + 1
        if kind == 0x00:
            value, at = read_map(data, at)
        elif kind == 0x01:
            end = data.index(b"\0", at); value = data[at:end]; at = end + 1
        elif kind == 0x02:
            value = struct.unpack_from("<I", data, at)[0]; at += 4
        else:
            raise ValueError("unknown VDF type %d" % kind)
        items.append((key, value))

with open(path, "rb") as file:
    top, _ = read_map(file.read(), 0)
for key, shortcuts in top:
    if key == b"shortcuts":
        for _, entry in shortcuts:
            fields = dict(entry)
            if fields.get(b"exe") == exe and b"appid" in fields:
                print(fields[b"appid"])
                sys.exit(0)
sys.exit(1)
PYTHON
}

# the game added to the library of the Steam user logged in, by Steam (it
# must run), from a desktop file beside the game; then its pictures
add_shortcut()
{
    local desktop="$GAME_DIR/$NAME_FILE.desktop" url
    printf '[Desktop Entry]\nType=Application\nName=%s\nExec=%s\nPath=%s\nIcon=steam\nTerminal=false\n' \
        "$NAME" "$GAME_DIR/$LAUNCHER" "$GAME_DIR" > "$desktop" && chmod 755 "$desktop" || return 1
    url="steam://addnonsteamgame/$(python3 -c 'import sys, urllib.parse; print(urllib.parse.quote(sys.argv[1], safe=""))' "$desktop")"
    log "asking Steam: $url"
    # (Steam's own "Add to Steam" touches this first: steamos-add-to-steam)
    touch /tmp/addnonsteamgamefile
    steam "$url" >/dev/null 2>&1 &
    for _ in $(seq 30); do have_shortcut && break; sleep 1; done
    have_shortcut || return 1
    add_pictures
    return 0
}

# the library pictures of the game's shortcut, for every Steam user who has
# it (again on an update: the builds before the name "Mjolnir VR" had
# Ringfall's)
add_pictures()
{
    local file id
    while read -r file; do
        id="$(shortcut_id "$file" "$EXE")" || continue
        log "shortcut $id in $file"
        mkdir -p "$(dirname "$file")/grid"
        cp "$BUNDLE/grid/library.png" "$(dirname "$file")/grid/${id}.png"
        cp "$BUNDLE/grid/libraryp.png" "$(dirname "$file")/grid/${id}p.png"
        cp "$BUNDLE/grid/library_hero.png" "$(dirname "$file")/grid/${id}_hero.png"
        cp "$BUNDLE/grid/library_logo.png" "$(dirname "$file")/grid/${id}_logo.png"
    done < <(shortcut_files)
}

# --- the game

[ -x "$LEPTON" ] || fail "Lepton, which runs Android games on the Steam Frame, was not found.\n\nStart any Android game from the Steam store once, then run this installer again."
for file in "$APK_NAME" "$LAUNCHER" steam_frame_xiso.py grid/library.png; do
    [ -f "$BUNDLE/$file" ] || fail "The installer is incomplete ($file is missing). Download it again."
done

# the builds before the name: their folder becomes the game's (settings,
# saves and game data kept), without their APK and launcher
if [ -d "$OLD_DIR" ] && [ ! -e "$GAME_DIR" ]; then
    log "renaming $OLD_DIR to $GAME_DIR"
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
# (the build's number: the launcher updates from it, steam_frame_launch.sh;
# an update fetched for an older build is dropped)
cp "$BUNDLE/build.txt" "$GAME_DIR/build.txt" 2>/dev/null || rm -f "$GAME_DIR/build.txt"
rm -rf "$GAME_DIR/update"

# --- the game data

if [ ! -f "$GAME_DIR/maps/ui.map" ]; then
    image="${MJOLNIR_IMAGE:-}"
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
    log "extracting $image"
    extract_maps "$image"
    chmod -R a+rwX "$GAME_DIR/maps"
    image_note="\n\nThe disc image itself is no longer needed:\n$image"
fi

# --- Steam

if have_shortcut; then
    log "the Steam shortcut is there"
    add_pictures
elif unattended; then
    echo "info: the Steam shortcut is missing (not added unattended)"
elif ! steam_running; then
    info "Steam is not running, so $NAME could not be added to your library.\n\nStart Steam and run the installer again."
else
    add_shortcut || fail "Steam did not add $NAME to your library. Run the installer again; if it happens again, add\n$GAME_DIR/$NAME_FILE.desktop\nwith Steam's \"Add a Non-Steam Game\"."
fi

if unattended; then
    echo "info: installed in $GAME_DIR"
    exit 0
fi
if have_shortcut_to "$OLD_DIR/halo-vr.sh"; then
    info "The library still shows the old \"$OLD_NAME\" from before the game's new name. It no longer starts: remove it in Steam (its gear or Manage > Remove non-Steam game)."
fi
if have_shortcut_to "$FORMER_NAME"; then
    info "The library shows the game by its former name, \"$FORMER_NAME\". To call it $NAME there: its gear > Properties, then its name at the top."
fi
if ask "$NAME is installed.${image_note:-}\n\nSwitch to Gaming Mode now? Start $NAME from your library there (its pictures may show once Steam starts again)." yes; then
    steamos-session-select gamescope >/dev/null 2>&1 ||
        info "Switch to Gaming Mode yourself (the \"Return to Gaming Mode\" icon or the power menu)."
fi
