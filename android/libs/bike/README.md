# The bike library: read the bike's sensors

Reads live cadence (rpm), power (watts) and resistance (0 to 100) from the sensor service that Peloton's own software runs on the original Bike's tablet, and turns them into the pace and resistance a game reads.

It is a library in its own right, in two modules that depend on nothing else in this repository, so another game can take it as it is ([Use it in another game](#use-it-in-another-game)). It is free software under GPL-2.0-or-later, so it can go into a GPL 2 project as well as a GPL 3 one ([what that means for a game that uses it](#credits-and-licence)).

| Module | Artifact | What it is |
|---|---|---|
| `libs/bike-core` | `dev.soloton:bike-core` | Pure Kotlin, no Android: a reading, the two services' wire formats, the connection state machine, the cadence-to-pace and resistance mapping, and the `PaceSource` and `ResistanceSource` interfaces a game is written against. Needs only the Kotlin standard library |
| `libs/bike` | `dev.soloton:bike-android` | Android: binds the services (`BikeSensors`), hands a game its pace and resistance (`BikePaceSource`), and carries the diagnostics screen. Needs only `bike-core` |

**Status: operational on the owner's bike.** The owner reported on 2026-11-10 that live sensor data works on the real bike. Resistance has not been tested there, and which route connected and whether the units match are not yet recorded. The automated tests run against stand-in services written from public descriptions of the protocol. See "Check it on a real bike" below, and [docs/BIKE_SENSORS.md](../../docs/BIKE_SENSORS.md) for the evidence.

## What it does

An app cannot open the tablet's serial port to the bike. Two Peloton system services can, and both hand the values to any app that binds them. `BikeSensors` tries them in order and gives each 5 seconds to produce a valid sample:

| | Route | How it works |
|---|---|---|
| A | `com.onepeloton.affernetservice`, interface `IV1Interface` | Register a callback Binder; the service pushes a `BikeData` frame about once a second. Cadence in rpm, power in centiwatts, resistance 0 to 100. |
| B | `com.peloton.service.SensorData`, a Messenger | Polled: message 12 carries a raw read packet for the bike's sensor board, message 14 brings the board's raw answer, which this library checks and decodes (power in tenths of a watt). |
| B, older dialect | same service | Messages 1, 2 and 3 ask the service to keep sending cadence, power and resistance. Tried last. Can be switched off with `BikeEndpoints(allowRepeatingCommands = false)`. |

A route whose package is not installed is skipped. What happens when nothing works:

- **Neither package installed** (an emulator, a phone, another tablet): the state is `Unavailable` with the reason, and nothing is retried.
- **Installed but silent or refusing**: `Unavailable` with what each route did, and the search repeats after 1 s, 2 s, 4 s and so on up to 30 s.
- **A working connection goes quiet**: `NoData` after 3 s; after 10 s the binding is dropped and the search starts again.
- **The service's process dies**: `Error`, then the search starts again after the pause.

The library only binds and reads. It never stops, restarts or reconfigures a Peloton service, and never sends the calibration or fake-data calls those services also offer. Messages that are malformed or carry impossible values (negative, over 250 rpm, over 3000 W, resistance over 105) are dropped whole.

Two details worth knowing:

- Resistance is reported as the lowest of the last three samples, because the sensor is known to spike for a single sample. A real increase therefore shows two samples late. `BikeSensors(context, filterResistance = false)` turns this off.
- On route B, if the board never answers the "resistance, already normalised" read, the library reads the raw resistance count instead and converts it with the 31-entry calibration table stored on the bike, as the tablet's own software does. Until that table is loaded, resistance reads 0.

## Use it in another game

Write the game against `PaceSource` and `ResistanceSource` from `bike-core`. They are the whole of what a game needs to know about the bike, they are plain interfaces, and anything can stand in for them: on-screen buttons, a keyboard, a test. The game's platform-free code depends on `bike-core` only; the Android launcher adds `bike-android` and supplies the real thing.

**Getting the library.** There is no published Maven repository yet. Either:

- **Build it into your local Maven repository.** In a checkout of this repository:

  ```sh
  ./gradlew :libs:bike-core:publishToMavenLocal :libs:bike:publishToMavenLocal
  ```

  then in the other project add `mavenLocal()` to its repositories and:

  ```kotlin
  implementation("dev.soloton:bike-core:0.1.0")      // game code, any JVM module
  implementation("dev.soloton:bike-android:0.1.0")   // the Android app module; brings bike-core with it
  ```

  `-PbikeLibraryVersion=1.2.3` on the publish command sets another version. Sources are published alongside.
- **Copy the two directories** `libs/bike-core` and `libs/bike` into the other project. Their build files use this repository's convention plugins (`soloton.kotlin.jvm`, `soloton.android.library`); replace those two lines with `kotlin("jvm")` and `com.android.library` plus your own SDK levels. The minimum is Android 7.0 (API 24), the bike's tablet.

**In the Android launcher:**

```kotlin
val bike = BikePaceSource(applicationContext)        // in onCreate
val pace: PaceSource = bike.orElse(buttonsPace)      // give this to the game instead of buttonsPace
// onResume: bike.start()        onPause: bike.stop()
```

`pace.metersPerSecond` follows the bike while `bike.isLive`, and `buttonsPace` (any other `PaceSource`, such as the on-screen slower/faster buttons) the rest of the time. Show `bike.isLive` in the HUD so the rider can tell which one is driving. Both are cheap, lock-light reads meant for the render thread.

The same object carries the resistance knob as a 0 to 1 `ResistanceSource`: `bike.resistanceOrElse(fallback)` follows the bike while it is live and `fallback` otherwise, and reads 0 if the bike goes quiet for 3 s. Roadwork passes it to its environments, where Skylift Mesa uses it for height and difficulty ([docs/ENVIRONMENTS.md](../../docs/ENVIRONMENTS.md#resistance)). Resistance has not yet been checked on a real bike.

The pace mapping is `metres per second = clamp(rpm x 0.042, 0, 6.0)`, so 60 rpm is about 2.5 m/s and 90 rpm about 3.8 m/s. It is smoothed with a 1 s time constant and eases to a stop if no sample arrives for 3 s. Change it with `BikePaceSource(BikeSensors(context), CadencePace(metersPerSecondPerRpm = ..., maxMetersPerSecond = ...))`.

For the raw values, use `BikeSensors` directly: `start()`, `stop()`, `state`, `latest` (a `BikeReading`), `addListener(...)` and `diagnostics()`.

The library's manifest adds the `onepeloton.permission.ACCESS_SENSOR_SERVICE` permission request, `<queries>` entries for the two packages (only needed on Android 11 and later) and the diagnostics screen.

## Check it on a real bike

Any app that includes this library also contains a diagnostics screen. With the app installed (for Roadwork the application id is `dev.soloton.roadwork`):

```sh
adb shell am start -n <application id>/dev.soloton.bike.BikeDiagnosticsActivity
```

1. Wait about 20 seconds without touching the bike. Note the state line and the route.
2. Pedal slowly, then quickly. Cadence should follow your legs.
3. Turn the resistance knob from one end to the other. Resistance should go from near 0 to near 100.
4. Pedal hard for a few seconds. Compare the power shown with what you would expect.
5. Stop pedalling. Cadence and power should fall to 0 and the state should stay Connected.
6. Tap **Copy report**, then fetch the file it names:

```sh
adb pull /sdcard/Android/data/<application id>/files/bike-diagnostics.txt
```

Send back that file, and say whether the numbers in steps 2 to 5 looked right. If the state never reaches Connected, the file still says which packages exist and what each route did, which is what is needed to fix it. The read-only `adb` checks in [docs/BIKE_SENSORS.md](../../docs/BIKE_SENSORS.md) add detail the app cannot see.

## Layout

- `libs/bike-core`: `Inputs.kt` (`PaceSource`, `ResistanceSource`), `BikeReading.kt`, `AffernetWire.kt`, `SensorDataWire.kt`, `CadencePace.kt`, `BikeResistance.kt`, `BikeLink.kt`, `BikeDiagnostics.kt`. Pure Kotlin, no Android imports: wire layouts and unit conversion, validation, the resistance table, the pace and resistance mapping, the connection state machine and the report text. Unit tested on the JVM (`src/test`).
- `libs/bike`: `BikeSensors.kt`, `Transports.kt`, `BikePaceSource.kt`, `BikeDiagnosticsActivity.kt`. The Android layer. The diagnostics screen is a plain Activity that sets up its own fullscreen display, so it needs nothing from the app it is in.
- `libs/bike/src/androidTest`: stand-ins for both Peloton services, in a separate process and speaking the same Binder and Messenger protocols, and the tests that run the client against them. They are in the test APK only.

```sh
./gradlew :libs:bike-core:check                  # JVM tests
./gradlew :libs:bike:check                       # lint
./gradlew :libs:bike:connectedDebugAndroidTest   # needs an emulator or device
```

Neither module may depend on another module of this repository; that is what lets it be used elsewhere. `./gradlew :libs:bike:dependencies --configuration releaseRuntimeClasspath` should list `bike-core` and the Kotlin standard library and nothing else.

The instrumented tests prove the client and the stand-ins agree, and that the client copes when a service is missing, silent, wrong or killed. Both sides were written from the same public descriptions, so the tests cannot show that those descriptions match a real bike.

## Credits and licence

Both modules are free software under the GNU General Public License, version 2 or, at your option, any later version (`GPL-2.0-or-later`): see [LICENSE](LICENSE) here and in `libs/bike-core`. That is deliberately wider than the rest of this repository, which is GPL-3.0-only, so that the library can be used in projects on either version. Every source file carries the SPDX identifier, the published `bike-core` jar carries the licence text in `META-INF/LICENSE`, and both artifacts name the licence in their Maven metadata.

For a game that uses the library, that means the GPL's terms reach the game: if you distribute an app that includes it, the app as a whole has to be offered under the GPL, with its source. Which version is up to the game:

| The game's licence | Using this library |
|---|---|
| GPL-2.0-only | Yes: take the library under version 2 |
| GPL-2.0-or-later | Yes |
| GPL-3.0-only or GPL-3.0-or-later | Yes: take the library under version 3. Roadwork, in this repository, does this |
| Anything not GPL-compatible, or closed source | No |

Using it privately, on your own bike, asks nothing of you. This is a summary, not legal advice; the licence text is what counts.

It contains no code from Peloton and none copied from the projects below; it was written from the protocol facts they document.

- **PeloMon** by Imran S. Haque ([repository](https://github.com/ihaque/pelomon), [write-up](https://ihaque.org/tag/pelomon.html)): the serial protocol between tablet and bike, on which all of this rests. Packet framing, the additive checksum, digits sent least significant first, the cadence, power and raw-resistance commands, power in tenths of a watt, and the 31-entry resistance table with linear interpolation. The repository's licence file is GPL-3.0, but the firmware sources that hold the decoder and the table code say CC BY-NC 4.0 in their headers, so nothing was ported from them: the interpolation here is written from the description.
- **Grupetto** ([selalipop/grupetto](https://github.com/selalipop/grupetto)) and its maintained fork ([doudar/grupetto](https://github.com/doudar/grupetto)): the protocol reference for both tablet services. Service and interface names, transaction codes, the order of fields in `BikeData`, the Messenger message numbers and Bundle keys, and the resistance spike. Neither repository has a licence, so only facts were taken.
- **OpenRide** ([digitalducktape/OpenRide](https://github.com/digitalducktape/OpenRide), Apache-2.0): confirmation of route A on a real original Bike (Android 11), including that the callback is one-way and that power is in centiwatts. Nothing was copied.

Not affiliated with or endorsed by Peloton Interactive, Inc. "Peloton" is used only to name the hardware this works with.
