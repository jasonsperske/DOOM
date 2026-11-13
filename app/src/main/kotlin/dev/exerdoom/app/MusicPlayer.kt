package dev.exerdoom.app

import android.media.MediaPlayer
import android.os.Handler
import android.os.Looper
import android.util.Log

/**
 * Plays DOOM's music (converted to MIDI files by i_music_android.c) with Android's built-in MIDI
 * synthesiser. The engine calls in from its own thread; everything here runs on the main thread.
 *
 * The song pauses when DOOM pauses it (the pause key, the menu in some cases) and while the app is
 * in the background; it plays only when neither holds it.
 */
object MusicPlayer {
    private val main = Handler(Looper.getMainLooper())
    private var player: MediaPlayer? = null
    private var volume = 1f
    private var gamePaused = false
    private var appPaused = false

    fun play(path: String, looping: Boolean) = main.post {
        release()
        try {
            player = MediaPlayer().apply {
                setDataSource(path)
                isLooping = looping
                setVolume(volume, volume)
                prepare()
            }
            gamePaused = false
            update()
        } catch (e: Exception) {
            Log.w("DOOM", "Music: cannot play $path", e)
            release()
        }
    }

    fun stop() = main.post { release() }

    fun setGamePaused(paused: Boolean) = main.post {
        gamePaused = paused
        update()
    }

    fun setAppPaused(paused: Boolean) = main.post {
        appPaused = paused
        update()
    }

    fun setVolume(value: Float) = main.post {
        volume = value.coerceIn(0f, 1f)
        player?.setVolume(volume, volume)
    }

    private fun update() {
        val p = player ?: return
        val shouldPlay = !gamePaused && !appPaused
        if (shouldPlay && !p.isPlaying) p.start()
        if (!shouldPlay && p.isPlaying) p.pause()
    }

    private fun release() {
        player?.release()
        player = null
    }
}
