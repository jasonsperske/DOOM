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

    /** Starts the engine; false if it is already running. */
    @JvmStatic external fun nativeStart(args: Array<String>, wadDir: String, homeDir: String): Boolean

    @JvmStatic external fun nativeSetSurface(surface: Surface?)

    @JvmStatic external fun nativeKey(doomKey: Int, down: Boolean)

    /** Turn by [dx] mouse units; positive turns right. */
    @JvmStatic external fun nativeMouse(dx: Int)

    @JvmStatic external fun nativeSetPaused(paused: Boolean)

    @JvmStatic external fun nativeUiState(): Int

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

    fun endProcess() {
        android.os.Process.killProcess(android.os.Process.myPid())
    }
}
