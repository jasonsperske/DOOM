package dev.exerdoom.app

import android.Manifest
import android.app.Activity
import android.app.AlertDialog
import android.content.pm.PackageManager
import android.graphics.Color
import android.graphics.PixelFormat
import android.os.Bundle
import android.os.Environment
import android.os.Handler
import android.os.Looper
import android.util.Log
import android.util.TypedValue
import android.view.Gravity
import android.view.KeyEvent
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.View
import android.view.WindowManager
import android.widget.Button
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.TextView
import dev.soloton.bike.BikePaceSource
import java.io.File

/**
 * Runs DOOM full screen: the game in a 4:3 [SurfaceView] centred on the panel, with
 * [TouchControls] over the whole screen.
 *
 * Needs an IWAD (doom1.wad, doom.wad, doomu.wad, doom2.wad, plutonia.wad or tnt.wad, lower case)
 * in the app's own folder, `/sdcard/Android/data/dev.exerdoom.app/files`, or in `/sdcard/Doom`.
 * An APK built with the shareware doom1.wad inside (see app/build.gradle.kts) falls back to that.
 *
 * The bike (via soloton's bike library) drives ride mode: its pace and resistance go to the engine
 * 20 times a second. Without a bike, the GO button stands in for pedalling.
 *
 * For testing without a bike, GO's stand-in pace (m/s) and resistance (0..1) can be set:
 * `--ef go_pace 4.5 --ef go_resistance 1`. Add `--ez go_hold true` to pedal at that pace without
 * holding GO (for unattended test runs).
 *
 * The cadence that turns on auto-fire in ride mode defaults by skill level; set it with
 * `--ef autofire_rpm 70` (remembered; 0 returns to the default).
 *
 * Extra command-line arguments can be passed for testing:
 * `adb shell am start -n dev.exerdoom.app/.DoomActivity --es args "-warp 1 1 -skill 3"`
 */
class DoomActivity : Activity(), DoomNative.Listener, SurfaceHolder.Callback {

    private var running = false
    private var controls: TouchControls? = null
    private val bike by lazy { BikePaceSource(applicationContext) }
    private val handler = Handler(Looper.getMainLooper())
    private val goPace by lazy { intent.getFloatExtra("go_pace", GO_PACE) }
    private val goResistance by lazy { intent.getFloatExtra("go_resistance", GO_RESISTANCE) }
    private val goHold by lazy { intent.getBooleanExtra("go_hold", false) }

    private val feedBike = object : Runnable {
        override fun run() {
            val live = bike.isLive
            val pace = if (live) bike.metersPerSecond else if (goHold || controls?.goHeld == true) goPace else 0f
            val resistance = if (live) bike.resistance.fraction else goResistance
            DoomNative.nativeSetBike(live, pace, resistance)
            controls?.bikeStatus = TouchControls.BikeStatus(live, bike.sensors.latest?.cadenceRpm ?: 0f, resistance)
            handler.postDelayed(this, BIKE_FEED_MS)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON or WindowManager.LayoutParams.FLAG_FULLSCREEN)
        enterImmersive()
        DoomNative.listener = this
        launchOrShowSetup()
    }

    private fun launchOrShowSetup() {
        val wadDir = findWadDir()
        if (wadDir == null) {
            showSetup()
            return
        }
        showGame()
        val args = intent.getStringExtra("args")?.trim()?.split(Regex("\\s+"))?.filter { it.isNotEmpty() } ?: emptyList()
        Log.i(TAG, "Starting with IWAD folder $wadDir, args $args")
        DoomNative.nativeStart(args.toTypedArray(), wadDir.absolutePath, filesDir.absolutePath)
        running = true
        applyAutofireSetting()
    }

    /** The auto-fire cadence: from the launch intent if given there (and remembered), else saved. */
    private fun applyAutofireSetting() {
        val prefs = getSharedPreferences("ride", MODE_PRIVATE)
        if (intent.hasExtra("autofire_rpm")) {
            prefs.edit().putFloat("autofire_rpm", intent.getFloatExtra("autofire_rpm", 0f)).apply()
        }
        DoomNative.nativeSetAutofireRpm(prefs.getFloat("autofire_rpm", 0f))
    }

    // ---------------------------------------------------------------------------------------
    // WAD discovery

    private fun wadFolders(): List<File> = listOfNotNull(
        getExternalFilesDir(null),
        File(Environment.getExternalStorageDirectory(), "Doom").takeIf { hasStoragePermission() },
    )

    private fun findWadDir(): File? =
        wadFolders().firstOrNull { dir -> IWADS.any { File(dir, it).isFile } } ?: extractEmbeddedWad()

    /** The doom1.wad packed in the APK, if any, copied once to private storage. */
    private fun extractEmbeddedWad(): File? {
        val dir = File(filesDir, "embedded")
        val wad = File(dir, "doom1.wad")
        if (wad.isFile) return dir
        return try {
            assets.open("doom1.wad").use { input ->
                dir.mkdirs()
                val partial = File(dir, "doom1.wad.part")
                partial.outputStream().use { input.copyTo(it) }
                partial.renameTo(wad)
            }
            Log.i(TAG, "Using the shareware IWAD built into the APK")
            dir
        } catch (e: java.io.IOException) {
            null   // not built in
        }
    }

    private fun hasStoragePermission() =
        checkSelfPermission(Manifest.permission.READ_EXTERNAL_STORAGE) == PackageManager.PERMISSION_GRANTED

    override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<out String>, grantResults: IntArray) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (!running) launchOrShowSetup()
    }

    // ---------------------------------------------------------------------------------------
    // Screens

    private fun showGame() {
        val surface = SurfaceView(this).apply {
            holder.setFormat(PixelFormat.RGBX_8888)
            holder.addCallback(this@DoomActivity)
        }
        val root = FourByThreeFrame(this).apply {
            setBackgroundColor(Color.BLACK)
            addView(surface, FrameLayout.LayoutParams(0, 0, Gravity.CENTER))
            addView(TouchControls(this@DoomActivity).also { controls = it }, FrameLayout.LayoutParams(-1, -1))
        }
        setContentView(root)
    }

    private fun showSetup() {
        val appDir = getExternalFilesDir(null)
        val message = TextView(this).apply {
            setTextColor(Color.WHITE)
            setTextSize(TypedValue.COMPLEX_UNIT_PX, 34f)
            text = buildString {
                append("DOOM needs a game data file (IWAD).\n\n")
                append("Copy one of ${IWADS.joinToString(", ")} (lower case) to:\n\n")
                append("    $appDir\n")
                append("    /sdcard/Doom  (needs storage access)\n\n")
                append("For example:  adb push doom1.wad $appDir/\n\n")
                append("The shareware doom1.wad is free to download.")
            }
        }
        val retry = Button(this).apply {
            text = "Check again"
            setTextSize(TypedValue.COMPLEX_UNIT_PX, 34f)
            minHeight = 110
            minWidth = 360
            setOnClickListener { launchOrShowSetup() }
        }
        val allow = Button(this).apply {
            text = "Allow storage access"
            setTextSize(TypedValue.COMPLEX_UNIT_PX, 34f)
            minHeight = 110
            minWidth = 360
            visibility = if (hasStoragePermission()) View.GONE else View.VISIBLE
            setOnClickListener { requestPermissions(arrayOf(Manifest.permission.READ_EXTERNAL_STORAGE), 1) }
        }
        val column = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(120, 100, 120, 100)
            setBackgroundColor(Color.rgb(40, 0, 0))
            addView(message)
            addView(LinearLayout(context).apply {
                setPadding(0, 60, 0, 0)
                addView(retry)
                addView(allow)
            })
        }
        setContentView(column)
    }

    // ---------------------------------------------------------------------------------------
    // Surface

    override fun surfaceCreated(holder: SurfaceHolder) {}

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
        DoomNative.nativeSetSurface(holder.surface)
    }

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        DoomNative.nativeSetSurface(null)
    }

    // ---------------------------------------------------------------------------------------
    // Lifecycle

    override fun onResume() {
        super.onResume()
        enterImmersive()
        if (running) DoomNative.nativeSetPaused(false)
        MusicPlayer.setAppPaused(false)
        bike.start()
        handler.post(feedBike)
    }

    override fun onPause() {
        if (running) {
            // Come back to the menu (which pauses a single-player game) rather than mid-fight.
            if (DoomNative.nativeUiState() and (DoomNative.UI_LEVEL or DoomNative.UI_MENU) == DoomNative.UI_LEVEL) {
                DoomNative.tapKey(DoomKeys.ESCAPE)
            }
            DoomNative.nativeSetPaused(true)
        }
        MusicPlayer.setAppPaused(true)
        handler.removeCallbacks(feedBike)
        bike.stop()
        super.onPause()
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (hasFocus) enterImmersive()
    }

    @Suppress("DEPRECATION")
    private fun enterImmersive() {
        window.decorView.systemUiVisibility = View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY or
            View.SYSTEM_UI_FLAG_FULLSCREEN or
            View.SYSTEM_UI_FLAG_HIDE_NAVIGATION or
            View.SYSTEM_UI_FLAG_LAYOUT_STABLE or
            View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION or
            View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
    }

    // ---------------------------------------------------------------------------------------
    // Hardware keys (emulator, USB/Bluetooth keyboards). Back opens and closes the menu.

    override fun onKeyDown(keyCode: Int, event: KeyEvent): Boolean {
        if (!running) return super.onKeyDown(keyCode, event)
        val key = DoomKeys.fromAndroid(keyCode, event) ?: return super.onKeyDown(keyCode, event)
        if (event.repeatCount == 0) DoomNative.nativeKey(key, true)
        return true
    }

    override fun onKeyUp(keyCode: Int, event: KeyEvent): Boolean {
        if (!running) return super.onKeyUp(keyCode, event)
        val key = DoomKeys.fromAndroid(keyCode, event) ?: return super.onKeyUp(keyCode, event)
        DoomNative.nativeKey(key, false)
        return true
    }

    // ---------------------------------------------------------------------------------------
    // Engine callbacks (main thread)

    override fun onQuit() {
        finishAndRemoveTask()
        DoomNative.endProcess()
    }

    override fun onFatalError(message: String) {
        AlertDialog.Builder(this)
            .setTitle("DOOM error")
            .setMessage(message)
            .setCancelable(false)
            .setPositiveButton("Close") { _, _ -> onQuit() }
            .show()
    }

    override fun onDestroy() {
        if (DoomNative.listener === this) DoomNative.listener = null
        super.onDestroy()
    }

    /** Sizes its first child to the largest centred 4:3 rectangle; other children fill it. */
    private class FourByThreeFrame(context: android.content.Context) : FrameLayout(context) {
        override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
            super.onMeasure(widthMeasureSpec, heightMeasureSpec)
            val w = MeasureSpec.getSize(widthMeasureSpec)
            val h = MeasureSpec.getSize(heightMeasureSpec)
            var gw = w
            var gh = w * 3 / 4
            if (gh > h) {
                gh = h
                gw = h * 4 / 3
            }
            getChildAt(0)?.measure(
                MeasureSpec.makeMeasureSpec(gw, MeasureSpec.EXACTLY),
                MeasureSpec.makeMeasureSpec(gh, MeasureSpec.EXACTLY),
            )
        }
    }

    private companion object {
        const val TAG = "DOOM"
        const val BIKE_FEED_MS = 50L

        /** Pace while GO is held without a bike: about 70 rpm. */
        const val GO_PACE = 3.0f

        /** Resistance assumed without a bike: halfway. */
        const val GO_RESISTANCE = 0.5f

        /** The names linuxdoom-1.10's IdentifyVersion looks for. */
        val IWADS = listOf("doom2f.wad", "doom2.wad", "plutonia.wad", "tnt.wad", "doomu.wad", "doom.wad", "doom1.wad")
    }
}
