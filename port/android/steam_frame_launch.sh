#!/bin/bash
# Halo VR on the Steam Frame: Lepton runs the APK in a container of its own,
# and installs it anew whenever the APK is newer than the installed one
# (port/android/STEAM_FRAME.md). The game's data: ~/Documents/HaloCE.
# (one container for every start, whatever id Steam gives the shortcut)
export SteamAppId=4290000001
LEPTON="$HOME/.local/share/Steam/steamapps/common/Lepton/lepton"
APK="$HOME/Documents/HaloCE/HaloVR.apk"

started=$SECONDS
"$LEPTON" waitforexitandrun -- "$APK"
# (a new APK is only installed on its first start, not started: again)
if (( SECONDS - started < 30 )); then
    "$LEPTON" waitforexitandrun -- "$APK"
fi
