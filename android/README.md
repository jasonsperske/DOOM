# DOOM for the Peloton Bike (gen 1)

An Android port of `linuxdoom-1.10` for the original Peloton Bike's tablet: Android 7.0 (API 24), 1920x1080 landscape, MediaTek MT8173. It runs on any Android 7.0+ device.

You ride through each level: pedalling moves you along a route to the exit, and you fight by touching the monsters. Touch-only controls are there too.

The build setup follows [soloton](https://github.com/jasonsperske/soloton): the same toolchain, `minSdk 24`, and an APK for sideloading.

## Build

You need JDK 17+, the Android SDK, **NDK 29.0.14206865** and **CMake 3.31.6**. With soloton's toolchain:

```bash
. ~/.local/share/soloton-toolchain/env.sh
sdkmanager "ndk;29.0.14206865" "cmake;3.31.6"     # once
cd android
echo "sdk.dir=$ANDROID_HOME" > local.properties   # once, if ANDROID_HOME is not set
./gradlew :app:assembleDebug
```

The output is `app/build/outputs/apk/debug/app-debug.apk`, with `arm64-v8a` and `armeabi-v7a` for the bike and `x86_64` for the emulator.

## Install and add a WAD

DOOM needs game data (an IWAD), which is not included. Use one of the names the 1997 engine looks for, in lower case: `doom1.wad` (shareware), `doom.wad`, `doomu.wad`, `doom2.wad`, `plutonia.wad`, `tnt.wad`. Put it in either folder:

- `/sdcard/Android/data/dev.soloton.doom/files/` (no permission needed), or
- `/sdcard/Doom/` (the app asks for storage access).

```bash
adb install -r app/build/outputs/apk/debug/app-debug.apk
adb shell mkdir -p /sdcard/Android/data/dev.soloton.doom/files
adb push doom1.wad /sdcard/Android/data/dev.soloton.doom/files/
```

[Freedoom](https://freedoom.github.io/)'s `freedoom1.wad`, renamed to `doom.wad`, also works.

**Building the shareware WAD into the APK.** If `DOOM1.WAD` (or `doom1.wad`) is at the repository root, the build packs it into the APK. To use a different file, pass `-PembedWad=/path/to/doom1.wad`. On first run, if no other IWAD is installed, the app copies it to private storage and plays it, so the APK works with no setup. WADs stay out of git. Give such an APK only to people allowed to have the WAD: the shareware licence covers passing on the shareware version unmodified and for free.

With no WAD, the app shows these instructions instead. To pass DOOM command-line arguments while testing:

```bash
adb shell am start -n dev.soloton.doom/.DoomActivity --es args "-warp 1 3 -skill 4"
```

To test the higher weapons without a bike, set what GO stands in for (pace in m/s, resistance 0..1):

```bash
adb shell am start -n dev.soloton.doom/.DoomActivity --ef go_pace 4.5 --ef go_resistance 1
```

To test on soloton's gen 1 emulator, run `scripts/gen1-emulator.sh` in the soloton repo, then install as above.

## Ride mode

When a level starts, the game plans a route from the player to the exit, then the bike drives:

- **Pedal to move.** Cadence sets the speed along the route: stop pedalling and you stop. Without a bike, hold **GO** instead (about 70 rpm).
- **Doors open as you reach them,** locked or not. Lifts lower, and stairs and floors that a switch or trigger line would move are moved for you. The exit switch or line triggers when you arrive.
- **You face the nearest monster you can see** and strafe, so you keep following the route while it is in your sights. With nothing in sight, you face along the route.
- **Hold a finger on a monster to fire at it.** That also makes it your target until it dies or you let go.
- **Pedal power picks your weapon.** Power runs from 0 to 100%: `speed x (0.4 + 0.6 x resistance)`, where speed is pace over 4.5 m/s (about 105 rpm), capped at 1.
  - Power is split evenly across the game's weapons, weakest first: pistol, shotgun, super shotgun (DOOM II), chaingun, rocket launcher, then plasma rifle and BFG (not in shareware). In shareware, under 25% is the pistol and 75% or more is rockets.
  - You get the weapon whether or not you have picked it up. Stop pedalling and you're down to the pistol.
  - Without a bike, GO counts as 3 m/s at half resistance: 47%, the shotgun.
  - The left edge shows cadence, resistance, power and the weapon.
- **Rockets only when they can't hurt you.** The launcher is used, and fires, only at a target at least 256 units away with no monster within 192 units of you. Otherwise the next weapon down is used, and rockets come back once there is 64 units more room.
- **Ammo is unlimited** for every weapon, in ride and free mode.
- **Boss levels** end when the bosses die, not at an exit: E1M8 (two Barons of Hell) and the other episode finales. These play differently:
  - The route leads to the bosses.
  - Once you can see one, you circle the nearest at 320–640 units, in whichever direction is clear. You sidestep any shot that would pass within 80 units in the next 40 tics.
  - You keep the boss in front of you, so hold a finger on it as you pass.
  - When the bosses are dead and their walls (tag 666) have lowered, a new route takes you on. On E1M8 that is up the stairs to the teleporter, into the room that ends the episode.
- **FREE** switches to the touch controls below; **RIDE** switches back. If no route to the exit can be found, the touch controls are used.
- When you die, touch the screen to restart.

How the route is planned (`app/src/main/cpp/ride.c`):

1. The map is rasterised into 8-unit cells marking where the player's 30x30 box fits. Cells whose box touches a wall, a ledge too high to climb, a too-low ceiling or solid scenery are blocked.
2. Doors are planned as open. Lifts and floors moved by switches or trigger lines (including staircases, built as `EV_BuildStairs` would) are planned anywhere in the range of heights they can reach.
3. A* finds the route to the normal exit, falling back to the secret exit, or to a sector that ends the level (special 11). Teleporters count as links.
4. Each tic, the player steers at the farthest route cell in a straight line. When blocked (usually by a monster), they sidestep, alternating sides each second. They re-plan when knocked off the route, teleported or stuck for 4 seconds; waiting for a lift or door doesn't count as stuck.

Pace and resistance come from soloton's bike library ([below](#bike-library)) 20 times a second. To check the bike's sensors on a real bike:

```bash
adb shell am start -n dev.soloton.doom/dev.soloton.bike.BikeDiagnosticsActivity
```

## Controls

All controls sit in the black strips beside the 4:3 game (240 px each on the bike), so they barely cover it. In free mode (or ride mode's MENU and MAP; weapons are picked by pedal power there):

| Control | In the game | In menus |
|---|---|---|
| Stick (bottom of the left strip; start anywhere in the strip) | Up/down walks, left/right strafes, push to the rim to run | Arrow keys, repeating while held |
| Drag on the game | Turn | |
| FIRE (bottom of the right strip) | Fire; drag on it to turn while firing | OK (Enter); also answers "yes" in prompts |
| USE (above FIRE) | Open doors, press switches | BACK |
| MENU / MAP (top of the left strip) | Esc / automap | Esc |
| The face in the status bar | Opens the menu (in ride mode too) | |
| 1-7 (top of the right strip) | Weapons | hidden |

Hardware keyboards and the Back key also work. Back opens and closes the menu.

Leaving the app pauses the game and opens the menu, so you never come back mid-fight.

## How it works

`app/src/main/cpp/` replaces the Linux/X11 platform layer (`i_main.c`, `i_video.c`, `i_sound.c`, `i_system.c`, `i_net.c`). The rest of `linuxdoom-1.10/` compiles unchanged except for portability fixes.

| File | Role |
|---|---|
| `android_main.c` | JNI bridge: runs `D_DoomMain` on its own thread, queues input for `I_StartTic`, hands over the surface, handles pause/resume, sends stdout to logcat (tag `DOOM`) |
| `i_video_android.c` | Converts the 320x200 paletted frame and scales it (nearest neighbour) into the `SurfaceView` buffer. The view is 4:3 and centred, so 1440x1080 on the bike |
| `i_sound_android.c` | Sound effects mixed in an OpenSL ES callback (44.1 kHz stereo) |
| `i_music_android.c` | Music: converts each song's MUS lump to a standard MIDI file (MIDI lumps from PWADs pass through) in the app's files folder; `MusicPlayer.kt` plays it, looping, with Android's built-in MIDI synthesiser (`MediaPlayer`). It follows the menu's music volume and pauses with the game and when the app is in the background |
| `i_system_android.c` | Timer that ignores time spent paused, 32 MB zone heap, `I_Error`/`I_Quit` call back to Kotlin |
| `i_net_android.c` | Single player only |
| `ride.c` | Ride mode: route planning, steering, targeting, doors and lifts, weapon choice, unlimited ammo |

Changes to the engine sources in `linuxdoom-1.10/`:

- **64-bit:** pointer arrays allocated at 4 bytes per entry (`r_data.c`, `p_setup.c`); a pointer field in the on-disk `maptexture_t`; pointer-to-`int` casts (`r_data.c`, `r_draw.c`, `z_zone.c`, `p_saveg.c`, `d_net.c`); string config defaults stored in `int`s (`m_misc.c`); zone blocks aligned to 8 bytes; savegames built in their own buffer instead of `screens[1]`, which 64-bit structs overflow (`g_game.c`).
- **ARM:** built with `-fsigned-char`, because `ticcmd_t`'s movement fields are plain `char`.
- **Bionic:** no `<values.h>`; `boolean` coexists with `<stdbool.h>`.
- **Touch (`__ANDROID__` only):** Quit Game quits at once, with no "press y" prompt; Enter answers "yes" in other prompts; an empty save slot gets a default name, the tic loop sleeps instead of spinning, and 8 sound channels by default.
- **Ride mode (`__ANDROID__` only):** `G_BuildTiccmd` hands the ticcmd to `RIDE_BuildTiccmd` and `P_SetupLevel` calls `RIDE_LevelLoaded`, and `vissprite_t` records its `mobj` so a touch can find the monster under it.

## Bike library

`libs/bike-core` and `libs/bike` are [soloton](https://github.com/jasonsperske/soloton)'s bike library (GPL-2.0-or-later), vendored at the commit in `libs/UPSTREAM`. Only their build files are this repository's own. To update them:

```bash
scripts/sync-bike-lib.sh ~/dev/soloton origin/main
./gradlew :libs:bike-core:test :app:assembleDebug
```

## Not done yet

- OPL (Sound Blaster) music: Android's General MIDI synthesiser sounds different from DOOM's original FM music
- Tuning ride mode's speed and power-to-weapon mapping on a real bike
- Multiplayer
- `-iwad` and other WAD names
- On-screen keyboard for save names
- Release signing (release builds use the debug key)
- Testing on a real bike: so far it has only run on the gen 1 emulator (x86_64, software rendering)
