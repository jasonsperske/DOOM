// SPDX-License-Identifier: GPL-2.0-or-later
package dev.soloton.bike

import android.annotation.SuppressLint
import android.app.Activity
import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.content.Intent
import android.content.pm.PackageInfo
import android.content.pm.PackageManager
import android.graphics.Color
import android.graphics.Typeface
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.provider.Settings
import android.util.TypedValue
import android.view.Gravity
import android.view.View
import android.view.WindowManager
import android.widget.Button
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * Shows, live, everything needed to tell whether the bike's sensors can be read on this tablet:
 * which Peloton packages and services exist, which route is in use, the connection state, the
 * values, how often they arrive, the pace they produce, the last error and a log of events.
 * "Copy report" puts the same as plain text on the clipboard and in a file `adb pull` can fetch.
 *
 * Start it from any app that includes this library:
 * `adb shell am start -n <application id>/dev.soloton.bike.BikeDiagnosticsActivity`
 *
 * It runs its own [BikeSensors] while visible and reads nothing from the starting intent. It is a
 * plain Activity that keeps the screen on and fills it, so it needs nothing from the app it is in.
 */
class BikeDiagnosticsActivity : Activity() {
    private lateinit var sensors: BikeSensors
    private val pace = CadencePace()
    private val feed = object : BikeSensors.Listener {
        override fun onReading(reading: BikeReading) = pace.onReading(reading)
    }
    private val ui = Handler(Looper.getMainLooper())
    private val refresher = object : Runnable {
        override fun run() {
            refresh()
            ui.postDelayed(this, REFRESH_MILLIS)
        }
    }

    private lateinit var stateView: TextView
    private lateinit var routeView: TextView
    private lateinit var cadenceView: TextView
    private lateinit var powerView: TextView
    private lateinit var resistanceView: TextView
    private lateinit var rateView: TextView
    private lateinit var paceView: TextView
    private lateinit var errorView: TextView
    private lateinit var noticeView: TextView
    private lateinit var packagesView: TextView
    private lateinit var eventsView: TextView
    private lateinit var eventsScroll: ScrollView
    private var shownEvents: List<String> = emptyList()
    private var inventory: List<String> = emptyList()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        fillScreen()
        val endpoints = endpointsForTest ?: BikeEndpoints()
        sensors = BikeSensors(applicationContext, endpoints, timingForTest ?: LinkTiming())
        sensors.addListener(feed)
        setContentView(buildUi())
        inventory = BikeInventory.describe(this, endpoints)
        packagesView.text = inventory.joinToString("\n")
    }

    /** Hides the status and navigation bars. Android shows them again when focus is lost, hence the two callers below. */
    @Suppress("DEPRECATION") // The bike's tablet is API 24, which has no WindowInsetsController.
    private fun fillScreen() {
        window.addFlags(WindowManager.LayoutParams.FLAG_FULLSCREEN)
        window.decorView.systemUiVisibility = View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY or
            View.SYSTEM_UI_FLAG_FULLSCREEN or
            View.SYSTEM_UI_FLAG_HIDE_NAVIGATION or
            View.SYSTEM_UI_FLAG_LAYOUT_STABLE or
            View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION or
            View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
    }

    override fun onResume() {
        super.onResume()
        fillScreen()
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (hasFocus) fillScreen()
    }

    override fun onStart() {
        super.onStart()
        sensors.start()
        ui.post(refresher)
    }

    override fun onStop() {
        ui.removeCallbacks(refresher)
        sensors.stop()
        super.onStop()
    }

    private fun refresh() {
        val d = sensors.diagnostics()
        val live = d.state is BikeState.Connected
        stateView.text = "State: ${d.state.description}"
        stateView.setTextColor(
            when (d.state) {
                is BikeState.Connected -> GOOD
                BikeState.Connecting, BikeState.NoData -> WAITING
                else -> BAD
            },
        )
        routeView.text = "Route: ${d.route?.label ?: "none"}"
        val reading = d.latest
        val stale = if (reading != null && !live) "  (last value, not live)" else ""
        cadenceView.text = "Cadence: ${reading?.let { BikeReport.one(it.cadenceRpm) + " rpm" } ?: "no reading"}$stale"
        powerView.text = "Power: ${reading?.let { BikeReport.one(it.powerWatts) + " W" } ?: "no reading"}$stale"
        resistanceView.text =
            "Resistance: ${reading?.let { BikeReport.one(it.resistancePercent) + " %" } ?: "no reading"}$stale"
        rateView.text = "Readings per second: ${BikeReport.one(d.readingsPerSecond)}" +
            "   accepted ${d.readingsAccepted}, rejected ${d.messagesRejected}"
        val mps = pace.metersPerSecond
        paceView.text = "Pace from cadence: ${BikeReport.two(mps)} m/s (${BikeReport.one(mps * 3.6f)} km/h)"
        errorView.text = "Last error: ${d.lastError ?: "none"}"
        if (d.events != shownEvents) {
            shownEvents = d.events
            eventsView.text = d.events.joinToString("\n")
            eventsScroll.post { eventsScroll.fullScroll(View.FOCUS_DOWN) }
        }
    }

    /** The report as text. Also used by the test. */
    internal fun reportText(): String {
        val now = SimpleDateFormat("yyyy-MM-dd HH:mm:ss Z", Locale.ROOT).format(Date())
        return BikeReport.text(
            header = listOf("App: $packageName", "Time: $now"),
            inventory = inventory,
            diagnostics = sensors.diagnostics(),
            paceMetersPerSecond = pace.metersPerSecond,
        )
    }

    private fun copyReport() {
        val text = reportText()
        val copied = try {
            val clipboard = getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
            clipboard.setPrimaryClip(ClipData.newPlainText("Soloton bike sensor report", text))
            true
        } catch (_: RuntimeException) {
            false
        }
        val file = File(getExternalFilesDir(null) ?: filesDir, BikeReport.FILE_NAME)
        val saved = try {
            file.writeText(text)
            true
        } catch (_: java.io.IOException) {
            false
        }
        noticeView.text = (if (copied) "Report copied to the clipboard" else "Could not use the clipboard") +
            (if (saved) " and saved. Fetch it with: adb pull ${file.path}" else "; could not save ${file.path}")
    }

    private fun reconnect() {
        sensors.stop()
        pace.reset()
        sensors.start()
        noticeView.text = "Reconnecting"
    }

    @SuppressLint("SetTextI18n")
    private fun buildUi(): View {
        val pad = px(24f).toInt()
        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setBackgroundColor(Color.rgb(16, 18, 22))
            setPadding(pad, pad, pad, pad)
        }
        val top = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
        }
        top.addView(
            label("Bike sensor diagnostics", TITLE_UNITS, bold = true),
            LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f),
        )
        top.addView(button("Copy report") { copyReport() })
        top.addView(button("Reconnect") { reconnect() })
        top.addView(button("Close") { finish() })
        root.addView(top)
        noticeView = label("Pedal and turn the resistance knob; the values below should follow.", TEXT_UNITS)
        noticeView.setTextColor(WAITING)
        root.addView(noticeView)

        val columns = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL }
        val live = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        stateView = label("State:", VALUE_UNITS, bold = true).also { live.addView(it) }
        routeView = label("Route:", TEXT_UNITS).also { live.addView(it) }
        cadenceView = label("Cadence:", VALUE_UNITS, bold = true).also { live.addView(it) }
        powerView = label("Power:", VALUE_UNITS, bold = true).also { live.addView(it) }
        resistanceView = label("Resistance:", VALUE_UNITS, bold = true).also { live.addView(it) }
        paceView = label("Pace from cadence:", TEXT_UNITS).also { live.addView(it) }
        rateView = label("Readings per second:", TEXT_UNITS).also { live.addView(it) }
        errorView = label("Last error:", TEXT_UNITS).also { live.addView(it) }
        live.addView(label("Packages and services", TEXT_UNITS, bold = true))
        packagesView = label("", TEXT_UNITS).also { live.addView(it) }
        val liveScroll = ScrollView(this).apply { addView(live) }
        columns.addView(liveScroll, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.MATCH_PARENT, 1f))

        val events = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(pad, 0, 0, 0)
        }
        events.addView(label("Events (newest last)", TEXT_UNITS, bold = true))
        eventsView = label("", TEXT_UNITS).apply { typeface = Typeface.MONOSPACE }
        eventsScroll = ScrollView(this).apply { addView(eventsView) }
        events.addView(eventsScroll, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f))
        columns.addView(events, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.MATCH_PARENT, 1f))

        root.addView(columns, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f))
        return root
    }

    private fun label(value: String, units: Float, bold: Boolean = false) = TextView(this).apply {
        text = value
        setTextSize(TypedValue.COMPLEX_UNIT_PX, px(units))
        setTextColor(Color.WHITE)
        if (bold) setTypeface(typeface, Typeface.BOLD)
        setPadding(0, px(6f).toInt(), 0, px(6f).toInt())
    }

    private fun button(value: String, onClick: () -> Unit) = Button(this).apply {
        text = value
        isAllCaps = false
        setTextSize(TypedValue.COMPLEX_UNIT_PX, px(TITLE_UNITS))
        minHeight = px(BUTTON_UNITS).toInt()
        minimumHeight = px(BUTTON_UNITS).toInt()
        minWidth = px(BUTTON_UNITS * 2).toInt()
        setPadding(px(28f).toInt(), 0, px(28f).toInt(), 0)
        setOnClickListener { onClick() }
    }

    /**
     * Sizes follow the screen's pixel height, not its density, because nobody knows what density
     * the bike reports. One unit is one pixel on a 1080-line screen.
     */
    private val unit: Float by lazy {
        val m = resources.displayMetrics
        minOf(m.widthPixels, m.heightPixels) / 1080f
    }

    private fun px(units: Float): Float = units * unit

    companion object {
        /** Smallest text: 28 px on a 1080-line screen. */
        const val TEXT_UNITS = 28f
        const val TITLE_UNITS = 34f
        const val VALUE_UNITS = 44f

        /** Button height: 112 px on a 1080-line screen. */
        const val BUTTON_UNITS = 112f
        private const val REFRESH_MILLIS = 250L
        private val GOOD = Color.rgb(120, 220, 130)
        private val WAITING = Color.rgb(240, 200, 90)
        private val BAD = Color.rgb(255, 130, 120)

        /** Instrumented tests point the screen at stand-in services. Never set in an app. */
        @Volatile
        internal var endpointsForTest: BikeEndpoints? = null

        @Volatile
        internal var timingForTest: LinkTiming? = null

        fun intent(context: Context): Intent = Intent(context, BikeDiagnosticsActivity::class.java)
    }
}

/** What Peloton software this tablet has, as lines of text for the screen and the report. */
internal object BikeInventory {
    private val hints = listOf("peloton", "affernet")

    fun describe(context: Context, endpoints: BikeEndpoints): List<String> {
        val lines = ArrayList<String>()
        lines += "Device: ${Build.MANUFACTURER} ${Build.MODEL}, Android ${Build.VERSION.RELEASE} " +
            "(API ${Build.VERSION.SDK_INT}), build ${Build.DISPLAY}"
        lines += "peloton_platform setting: " + try {
            Settings.Global.getString(context.contentResolver, "peloton_platform") ?: "not set"
        } catch (e: RuntimeException) {
            "unreadable ($e)"
        }
        val pm = context.packageManager
        val names = LinkedHashSet<String>()
        names += endpoints.affernetPackage
        names += endpoints.sensorDataPackage
        try {
            for (info in pm.getInstalledPackages(0)) {
                if (hints.any { info.packageName.contains(it, ignoreCase = true) }) names += info.packageName
            }
        } catch (e: RuntimeException) {
            lines += "Could not list installed packages: $e"
        }
        for (name in names) {
            lines += try {
                val info = pm.getPackageInfo(name, 0)
                "Package $name: version ${info.versionName} (${versionCode(info)})"
            } catch (_: PackageManager.NameNotFoundException) {
                "Package $name: not installed"
            } catch (e: RuntimeException) {
                "Package $name: unreadable ($e)"
            }
        }
        lines += service(context, "Route A service", Intent(endpoints.affernetAction).setPackage(endpoints.affernetPackage))
        lines += service(
            context,
            "Route B service",
            Intent(endpoints.sensorDataAction).setPackage(endpoints.sensorDataPackage),
        )
        val permission = AffernetWire.PERMISSION
        val granted = context.checkPermission(permission, android.os.Process.myPid(), android.os.Process.myUid()) ==
            PackageManager.PERMISSION_GRANTED
        lines += "Permission $permission: " + if (granted) "granted to this app" else "not granted (or not defined here)"
        return lines
    }

    private fun service(context: Context, title: String, intent: Intent): String = try {
        val info = context.packageManager.resolveService(intent, 0)?.serviceInfo
        if (info == null) {
            "$title: none answers ${intent.action}"
        } else {
            val needs = info.permission
            val allowed = needs == null ||
                context.packageManager.checkPermission(needs, context.packageName) == PackageManager.PERMISSION_GRANTED
            "$title: ${info.packageName}/${info.name}, exported=${info.exported}, " +
                "permission=${needs ?: "none"}" + if (needs != null) (if (allowed) " (granted)" else " (NOT granted)") else ""
        }
    } catch (e: RuntimeException) {
        "$title: could not query ($e)"
    }

    @Suppress("DEPRECATION")
    private fun versionCode(info: PackageInfo): Long =
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) info.longVersionCode else info.versionCode.toLong()
}
