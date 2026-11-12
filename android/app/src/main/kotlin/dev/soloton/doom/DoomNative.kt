package dev.soloton.doom

import android.os.Handler
import android.os.Looper
import android.view.Surface

/**
 * The native engine (libdoom.so). DOOM keeps its state in C globals, so it runs once per
 * process on its own thread; an Activity created later only reattaches its surface.
 */
object DoomNative {
    init {
        System.loadLibrary("doom")
    }

    // Bits of [nativeUiState].
    const val UI_MENU = 1
    const val UI_PROMPT = 2
    const val UI_LEVEL = 4
    const val UI_AUTOMAP = 8
    const val UI_DEMO = 16
    const val UI_RIDE = 32

    /** Starts the engine; false if it is already running. */
    @JvmStatic external fun nativeStart(args: Array<String>, wadDir: String, homeDir: String): Boolean

    @JvmStatic external fun nativeSetSurface(surface: Surface?)

    @JvmStatic external fun nativeKey(doomKey: Int, down: Boolean)

    /** Turn by [dx] mouse units; positive turns right. */
    @JvmStatic external fun nativeMouse(dx: Int)

    @JvmStatic external fun nativeSetPaused(paused: Boolean)

    @JvmStatic external fun nativeUiState(): Int

    // Ride mode (ride.c): the bike moves the player along a route to the exit.

    @JvmStatic external fun nativeSetRide(enabled: Boolean)

    /** [pace] in metres per second, [resistance] 0..1; [live] says they come from a real bike. */
    @JvmStatic external fun nativeSetBike(live: Boolean, pace: Float, resistance: Float)

    /** A finger on the game view, [u] and [v] 0..1 across it; held on a monster, it fires. */
    @JvmStatic external fun nativeRideTouch(down: Boolean, u: Float, v: Float)

    /** Pedal power, 0..1: pace scaled up by resistance. It picks the weapon in ride mode. */
    @JvmStatic external fun nativePower(): Float

    /** The weapon ride mode has chosen (DOOM's weapontype_t), or -1 outside ride mode. */
    @JvmStatic external fun nativeRideWeapon(): Int

    /** Names of DOOM's weapontype_t values, for the readout. */
    val WEAPON_NAMES = listOf("FIST", "PISTOL", "SHOTGUN", "CHAINGUN", "ROCKETS", "PLASMA", "BFG", "CHAINSAW", "SUPER SG")

    fun tapKey(doomKey: Int) {
        nativeKey(doomKey, true)
        nativeKey(doomKey, false)
    }

    interface Listener {
        fun onQuit()
        fun onFatalError(message: String)
    }

    @Volatile var listener: Listener? = null

    private val main = Handler(Looper.getMainLooper())

    // Called from the game thread (see android_main.c), which then waits for the process to end.

    @JvmStatic
    fun onQuit() {
        main.post { listener?.onQuit() ?: endProcess() }
    }

    @JvmStatic
    fun onFatalError(message: String) {
        main.post { listener?.onFatalError(message) ?: endProcess() }
    }

    // Music (i_music_android.c). These must return void: the native side looks them up by signature.

    @JvmStatic fun onMusicPlay(path: String, looping: Boolean) {
        MusicPlayer.play(path, looping)
    }

    @JvmStatic fun onMusicStop() {
        MusicPlayer.stop()
    }

    @JvmStatic fun onMusicPause(paused: Boolean) {
        MusicPlayer.setGamePaused(paused)
    }

    @JvmStatic fun onMusicVolume(volume: Float) {
        MusicPlayer.setVolume(volume)
    }

    fun endProcess() {
        android.os.Process.killProcess(android.os.Process.myPid())
    }
}
