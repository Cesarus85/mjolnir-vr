#!/bin/bash
# Mjolnir VR on the Steam Frame: Lepton runs the APK in a container of its own,
# and installs it anew whenever the APK is newer than the installed one
# (port/android/STEAM_FRAME.md). The game's data: ~/Documents/RingfallMjolnirVR.
# (a container of its own, not the shortcut's id: with that, SteamVR's
# client in the container waited on xrCreateInstance)
export SteamAppId="${HALO_LEPTON_APP_ID:-4290000001}"
# (what Steam gives an Android title of its own, and a non-Steam game not:
# the app's data in the container links to the compat data path's
# internal/, a dangling link without it; and the shader paths: Lepton reads
# the dump path against the other)
export STEAM_COMPAT_DATA_PATH="$HOME/.local/share/Steam/steamapps/compatdata/$SteamAppId"
export STEAM_COMPAT_SHADER_PATH="$HOME/.local/share/Steam/steamapps/shadercache/$SteamAppId"
mkdir -p "$STEAM_COMPAT_DATA_PATH/internal" "$STEAM_COMPAT_SHADER_PATH"
unset STEAM_FOSSILIZE_DUMP_PATH LD_PRELOAD
LEPTON="$HOME/.local/share/Steam/steamapps/common/Lepton/lepton"
DATA="$HOME/Documents/RingfallMjolnirVR"
APK="$DATA/RingfallMjolnirVR.apk"
LOG="$DATA/launcher.log"
# (written by the game when the player quits it: no start again)
QUIT="$DATA/quit_requested.txt"
rm -f "$QUIT"
CONTEXT="steamlaunch-$SteamAppId"

log() { echo "$(date '+%F %T') $*" >> "$LOG"; }
log "start (pid $$, parent $PPID)"

# Steam's "Exit game" ends the game's processes as Steam knows them (this
# script, under Steam's reaper), not the container, which podman runs: a
# signal here is passed on to Lepton, and a watcher of its own takes the
# container down once this script is gone (killed)
stopping=0
child=0
stop()
{
    log "signal $1"
    stopping=1
    [ "$child" -gt 0 ] && kill -TERM "$child" 2>/dev/null
}
trap 'stop TERM' TERM
trap 'stop INT' INT
trap 'stop HUP' HUP

script=$$
setsid bash -c '
    while kill -0 '"$script"' 2>/dev/null; do sleep 1; done
    echo "$(date "+%F %T") watcher: the game was ended, so its container" >> "'"$LOG"'"
    "'"$LEPTON"'" kill "'"$CONTEXT"'" >/dev/null 2>&1
' </dev/null >/dev/null 2>&1 &

# the game's log while it runs (Lepton's logcat of its container), for
# looking into problems: logcat.txt, anew each start
LOGCAT="$DATA/logcat.txt"
: > "$LOGCAT"
setsid bash -c '
    while kill -0 '"$script"' 2>/dev/null; do
        "'"$LEPTON"'" logcat "'"$CONTEXT"'" 2>/dev/null | grep -v "^ERROR: \|^steamlaunch-\|^$" >> "'"$LOGCAT"'"
        sleep 3
    done
' </dev/null >/dev/null 2>&1 &

run()
{
    "$LEPTON" waitforexitandrun -- "$APK" &
    child=$!
    # (wait returns early when a signal comes: until Lepton is gone)
    while kill -0 "$child" 2>/dev/null; do
        wait "$child"
    done
    child=0
}

# Updates (update.auto in config.toml; the game's own updater, the Quest's,
# does nothing here: Lepton installs the APK in this folder each start).
# While the game runs, the latest release is looked for; a newer one's
# bundle is fetched into update/, checked, and put in place as the next
# start begins, before Lepton runs the APK (it installs the newer one).
# A build that knows no number of its own (build.txt: a local one) never
# looks. Nothing changes without the internet, or when anything fails.
REPOSITORY="@REPOSITORY@"
UPDATE="$DATA/update"
BUILD_FILE="$DATA/build.txt"
SELF="$DATA/ringfall-mjolnir-vr.sh"

update_apply()
{
    local new="$UPDATE/bundle" build
    [ -f "$UPDATE/ready" ] || { rm -rf "$UPDATE"; return 0; }
    build=$(cat "$new/build.txt" 2>/dev/null)
    # (the launcher renamed over this one: bash goes on reading its own copy)
    if cp "$new/ringfall-mjolnir-vr.sh" "$SELF.tmp" && chmod 755 "$SELF.tmp" &&
        cp "$new/RingfallMjolnirVR.apk" "$APK.tmp" && chmod 666 "$APK.tmp" &&
        mv -f "$SELF.tmp" "$SELF" && mv -f "$APK.tmp" "$APK" && touch "$APK" &&
        cp "$new/build.txt" "$BUILD_FILE"
    then
        log "update: build $build put in place"
    else
        log "update: build $build could not be put in place"
        rm -f "$SELF.tmp" "$APK.tmp"
    fi
    rm -rf "$UPDATE"
}

update_wanted()
{
    # (update.auto = false in config.toml's [update]: never)
    ! awk '/^\[/ { section = $0 } section == "[update]" && /^[ \t]*auto[ \t]*=/ { print $3 }' \
        "$DATA/config.toml" 2>/dev/null | grep -q false
}

update_check()
{
    local installed latest
    installed=$(cat "$BUILD_FILE" 2>/dev/null)
    [[ "$installed" =~ ^[0-9]+$ ]] && (( installed > 0 )) || return 0
    update_wanted || return 0
    latest=$(curl -fsSL --max-time 20 -H "Accept: application/vnd.github+json" \
        "https://api.github.com/repos/$REPOSITORY/releases/latest" |
        python3 -I -c 'import json, sys; print(json.load(sys.stdin)["tag_name"].rpartition("-")[2])' 2>/dev/null)
    [[ "$latest" =~ ^[0-9]+$ ]] && (( latest > installed )) || return 0
    log "update: build $latest found (this is $installed), fetching it"
    rm -rf "$UPDATE" && mkdir -p "$UPDATE" || return 0
    if curl -fsSL --retry 2 --max-time 900 -o "$UPDATE/bundle.zip" \
            "https://github.com/$REPOSITORY/releases/download/build-$latest/mjolnir-vr-steam-frame.zip" &&
        python3 -I -m zipfile -e "$UPDATE/bundle.zip" "$UPDATE/bundle" &&
        [ "$(cat "$UPDATE/bundle/build.txt" 2>/dev/null)" = "$latest" ] &&
        [ -s "$UPDATE/bundle/RingfallMjolnirVR.apk" ] && [ -s "$UPDATE/bundle/ringfall-mjolnir-vr.sh" ]
    then
        rm -f "$UPDATE/bundle.zip"
        touch "$UPDATE/ready"
        log "update: build $latest fetched, put in place at the next start"
    else
        log "update: build $latest could not be fetched"
        rm -rf "$UPDATE"
    fi
}

update_apply
update_check </dev/null >/dev/null 2>&1 &
updater=$!

# (a new APK is only installed on its first start, not started; and a start
# right after a game that was ended hard can fail in SteamVR, which then
# needs a moment: again, up to three times, while it ends within 30 s)
for attempt in 1 2 3; do
    started=$SECONDS
    run
    if (( stopping || SECONDS - started >= 30 )) || [ -e "$QUIT" ]; then
        break
    fi
    log "ended within $((SECONDS - started)) s: again"
    sleep 5
done
rm -f "$QUIT"
# (a fetch still under way is given up: the next start looks again)
pkill -P "$updater" 2>/dev/null
kill "$updater" 2>/dev/null
log "end"
