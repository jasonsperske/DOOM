// SPDX-License-Identifier: GPL-2.0-or-later
package dev.soloton.bike

import android.content.Context
import android.os.SystemClock
import android.view.View
import android.view.ViewGroup
import android.widget.Button
import android.widget.TextView
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import dev.soloton.bike.fake.FakeScript
import dev.soloton.bike.fake.FakeStatus
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File
import java.util.concurrent.CopyOnWriteArrayList
import kotlin.math.abs

/**
 * The client against the stand-in services in src/androidTest/.../fake, over real Binder calls
 * between two processes.
 *
 * These prove that the client and the stand-ins agree with each other, and that the client
 * behaves when a service is absent, silent, wrong or dies. Both were written from the same public
 * descriptions of the protocol, so none of this proves the client works on a real bike.
 */
@RunWith(AndroidJUnit4::class)
class BikeSensorsTest {
    private val context: Context = InstrumentationRegistry.getInstrumentation().targetContext
    private val here: String = context.packageName
    private val absent = "dev.soloton.bike.absent"

    /** Short enough to keep the tests quick, long enough for a slow emulator to start a process. */
    private val timing = LinkTiming(
        firstDataTimeoutNanos = 4 * LinkTiming.SECOND,
        staleAfterNanos = 1 * LinkTiming.SECOND,
        rebindAfterNanos = 3 * LinkTiming.SECOND,
        retryMinNanos = LinkTiming.SECOND / 2,
        retryMaxNanos = 2 * LinkTiming.SECOND,
        pollGapNanos = LinkTiming.SECOND / 50,
        pollTimeoutNanos = LinkTiming.SECOND / 2,
    )
    private val both = BikeEndpoints(affernetPackage = here, sensorDataPackage = here)
    private val legacyOnly = BikeEndpoints(affernetPackage = absent, sensorDataPackage = here)

    private var sensors: BikeSensors? = null
    private val readings = CopyOnWriteArrayList<BikeReading>()
    private val states = CopyOnWriteArrayList<BikeState>()

    @Before
    fun resetFakes() {
        FakeStatus.clear(context)
        script(FakeScript())
    }

    @After
    fun stop() {
        sensors?.stop()
        // Let the unbind land so the next test starts with no client attached.
        SystemClock.sleep(300)
    }

    private fun script(script: FakeScript) = script.write(context)

    private fun start(endpoints: BikeEndpoints): BikeSensors {
        val created = BikeSensors(context, endpoints, timing)
        created.addListener(object : BikeSensors.Listener {
            override fun onReading(reading: BikeReading) {
                readings += reading
            }

            override fun onStateChanged(state: BikeState) {
                states += state
            }
        })
        sensors = created
        created.start()
        return created
    }

    private fun await(what: String, timeoutMillis: Long = 45_000, condition: () -> Boolean) {
        val deadline = SystemClock.uptimeMillis() + timeoutMillis
        while (SystemClock.uptimeMillis() < deadline) {
            if (condition()) return
            SystemClock.sleep(50)
        }
        val d = sensors?.diagnostics()
        throw AssertionError("Timed out waiting for $what. State: ${d?.state}; latest: ${d?.latest}; events:\n" +
            d?.events?.joinToString("\n") + "\nfake: ${FakeStatus.read(context)}")
    }

    private fun awaitConnected(s: BikeSensors, route: BikeRoute) =
        await("Connected over ${route.label}") { s.state == BikeState.Connected(route) }

    private fun BikeReading?.isAbout(rpm: Float, watts: Float, resistance: Float): Boolean =
        this != null && cadenceRpm == rpm && abs(powerWatts - watts) < 0.01f && abs(resistancePercent - resistance) < 0.6f

    // Route A

    @Test
    fun routeA_connectsAndDeliversScriptedReadingsInRpmWattsAndPercent() {
        val s = start(both)
        awaitConnected(s, BikeRoute.AFFERNET)
        // The fake sends 12340 centiwatts.
        await("the scripted values") { s.latest.isAbout(72f, 123.4f, 37f) }
        assertEquals(BikeRoute.AFFERNET, s.route)
        assertEquals("Soloton", FakeStatus.read(context).clientId)
        assertEquals(1, FakeStatus.read(context).registerCalls)
        assertEquals(0, FakeStatus.read(context).pollRequests) // route B was never touched

        script(FakeScript(rpm = 95, powerTenths = 2505, resistance = 50))
        await("the new values") { s.latest.isAbout(95f, 250.5f, 50f) }
        assertTrue(s.diagnostics().readingsPerSecond > 1f)
        assertTrue(s.latest!!.timestampNanos <= System.nanoTime())
    }

    @Test
    fun routeA_aStationaryBikeStillCountsAsConnected() {
        script(FakeScript(rpm = 0, powerTenths = 0, resistance = 1))
        val s = start(both)
        awaitConnected(s, BikeRoute.AFFERNET)
        await("zeros") { s.latest.isAbout(0f, 0f, 1f) }
    }

    @Test
    fun routeA_malformedFramesAreRejectedAndGoodOnesStillArrive() {
        // Short parcels, a negative cadence, a wrong interface token and null data, with good frames between.
        script(FakeScript(affernetMode = "mixed"))
        val s = start(both)
        awaitConnected(s, BikeRoute.AFFERNET)
        await("several rejections") { s.diagnostics().messagesRejected >= 8 && readings.size >= 3 }
        assertTrue(readings.toString(), readings.all { it.isAbout(72f, 123.4f, 37f) })
        assertTrue(s.diagnostics().lastError != null)
    }

    @Test
    fun routeA_onlyMalformedFramesNeverCountAsConnected() {
        script(FakeScript(affernetMode = "negative", polling = false, repeating = false))
        val s = start(both)
        await("giving up on every route") { s.state is BikeState.Unavailable && states.size >= 2 }
        assertTrue(readings.isEmpty())
        assertNull(s.latest)
        assertTrue(s.diagnostics().messagesRejected > 0)
        assertFalse(states.any { it is BikeState.Connected })
    }

    @Test
    fun stop_unregistersTheCallbackAndUnbinds_andStartWorksAgain() {
        val s = start(both)
        awaitConnected(s, BikeRoute.AFFERNET)
        assertEquals(1, FakeStatus.read(context).callbacks)

        s.stop()
        assertEquals(BikeState.Unavailable("stopped"), s.state)
        await("the unregister call") { FakeStatus.read(context).let { it.unregisterCalls == 1 && it.callbacks == 0 } }
        SystemClock.sleep(300)
        val count = readings.size
        SystemClock.sleep(700)
        assertEquals("no readings after stop", count, readings.size)
        s.stop() // harmless twice

        s.start()
        awaitConnected(s, BikeRoute.AFFERNET)
        await("readings again") { readings.size > count }
        assertEquals(2, FakeStatus.read(context).registerCalls)
    }

    @Test
    fun survivesTheServiceProcessDying_andReconnects() {
        val s = start(both)
        awaitConnected(s, BikeRoute.AFFERNET)
        val firstPid = FakeStatus.read(context).pid
        assertNotEquals(0, firstPid)

        FakeScript.requestKill(context)
        await("noticing the death") { states.any { it is BikeState.Error } }
        states.clear()
        awaitConnected(s, BikeRoute.AFFERNET)
        val before = readings.size
        await("readings from the new process") { readings.size > before + 3 }
        assertNotEquals(firstPid, FakeStatus.read(context).pid)
    }

    @Test
    fun silenceWhileConnectedBecomesNoData_andDataComingBackReconnects() {
        val s = start(both)
        awaitConnected(s, BikeRoute.AFFERNET)
        script(FakeScript(affernetMode = "silent", polling = false, repeating = false))
        await("NoData") { s.state == BikeState.NoData }
        script(FakeScript())
        awaitConnected(s, BikeRoute.AFFERNET)
    }

    // Fallback

    @Test
    fun fallsBackToRouteB_whenRouteAIsNotInstalled() {
        val s = start(legacyOnly)
        awaitConnected(s, BikeRoute.SENSORDATA_POLL)
        // The fake's packets carry 1234 tenths of a watt.
        await("the scripted values") { s.latest.isAbout(72f, 123.4f, 37f) }
        val status = FakeStatus.read(context)
        assertEquals(0, status.registerCalls)
        assertEquals(0, status.badRequests)
        assertTrue(status.pollRequests > 0)

        script(FakeScript(rpm = 101, powerTenths = 3001, resistance = 64))
        await("the new values") { s.latest.isAbout(101f, 300.1f, 64f) }
    }

    @Test
    fun fallsBackToRouteB_whenRouteABindsButStaysSilent() {
        script(FakeScript(affernetMode = "silent"))
        val s = start(both)
        awaitConnected(s, BikeRoute.SENSORDATA_POLL)
        // It let go of route A properly before moving on.
        val status = FakeStatus.read(context)
        assertEquals(1, status.registerCalls)
        assertEquals(1, status.unregisterCalls)
        assertEquals(0, status.callbacks)
        assertTrue(states.toString(), states.first() == BikeState.Connecting)
    }

    @Test
    fun fallsBackToRouteB_whenRouteAIsSomeOtherBinderInterface() {
        val s = start(both.copy(affernetAction = "dev.soloton.bike.fake.IMPOSTER"))
        awaitConnected(s, BikeRoute.SENSORDATA_POLL)
        assertTrue(s.diagnostics().events.any { it.contains("unexpected Binder interface") })
    }

    @Test
    fun routeB_usesRepeatingCommandsWhenTheServiceDoesNotAnswerPolls() {
        script(FakeScript(polling = false))
        val s = start(legacyOnly)
        awaitConnected(s, BikeRoute.SENSORDATA_REPEATING)
        // Cadence and power from the raw packets; resistance from the service's parsed value,
        // because the packet beside it is a raw count.
        await("the scripted values") { s.latest.isAbout(72f, 123.4f, 37f) }
        assertEquals(3, FakeStatus.read(context).repeatingCommands)
    }

    @Test
    fun routeB_repeatingCommandsWithoutRawPacketsUseTheParsedValues() {
        script(FakeScript(polling = false, legacyMode = "nohex"))
        val s = start(legacyOnly)
        awaitConnected(s, BikeRoute.SENSORDATA_REPEATING)
        await("the scripted values") { s.latest.isAbout(72f, 123.4f, 37f) }
    }

    @Test
    fun routeB_repeatingCommandsCanBeSwitchedOff() {
        script(FakeScript(polling = false))
        val s = start(legacyOnly.copy(allowRepeatingCommands = false))
        await("giving up") { s.state is BikeState.Unavailable && states.size >= 2 }
        assertEquals(0, FakeStatus.read(context).repeatingCommands)
    }

    @Test
    fun routeB_readsRawResistanceThroughTheBikesTableWhenTheNormalisedReadIsNotAnswered() {
        script(FakeScript(normalisedResistance = false))
        val s = start(legacyOnly)
        awaitConnected(s, BikeRoute.SENSORDATA_POLL)
        // Cadence and power flow at once, with resistance 0 until the 31-entry table is in.
        await("cadence and power") { s.latest?.cadenceRpm == 72f }
        await("resistance through the table", 90_000) { s.latest.isAbout(72f, 123.4f, 37f) }
        assertTrue(s.diagnostics().events.any { it.contains("resistance table") })
    }

    @Test
    fun routeB_worksWithAServiceThatDeclaresNoCategory() {
        val s = start(legacyOnly.copy(sensorDataAction = "dev.soloton.bike.fake.OLD_SENSOR_DATA"))
        awaitConnected(s, BikeRoute.SENSORDATA_POLL)
        await("the scripted values") { s.latest.isAbout(72f, 123.4f, 37f) }
    }

    @Test
    fun routeB_badChecksumsTimeoutsAndGarbageAreRejectedWithoutCrashing() {
        for (mode in listOf("badchecksum", "timeout", "garbage")) {
            readings.clear()
            states.clear()
            script(FakeScript(legacyMode = mode))
            val s = start(legacyOnly)
            await("giving up with $mode") { s.state is BikeState.Unavailable && states.size >= 2 }
            assertTrue(mode, readings.isEmpty())
            assertNull(mode, s.latest)
            assertTrue(mode, s.diagnostics().messagesRejected > 0)
            assertFalse(mode, states.any { it is BikeState.Connected })
            s.stop()
            SystemClock.sleep(300)
        }
    }

    // Nothing there

    @Test
    fun reportsUnavailableWithAReason_whenNeitherServiceExists() {
        val s = start(BikeEndpoints(affernetPackage = absent, sensorDataPackage = "$absent.too"))
        await("Unavailable") { s.state is BikeState.Unavailable && s.state != BikeState.Unavailable("not started") }
        val reason = (s.state as BikeState.Unavailable).reason
        assertTrue(reason, reason.contains("no Peloton sensor service on this device"))
        assertTrue(reason, reason.contains("$absent is not installed"))
        SystemClock.sleep(1500)
        assertEquals("it does not keep trying", listOf<BikeState>(s.state), states.toList())
        assertNull(s.latest)
        assertNull(s.route)
    }

    @Test
    fun withTheDefaultEndpoints_aDeviceThatIsNotABikeReportsUnavailable() {
        for (name in listOf("com.onepeloton.affernetservice", "com.peloton.service.SensorData")) {
            try {
                context.packageManager.getPackageInfo(name, 0)
                return // A real bike: this test does not apply.
            } catch (_: android.content.pm.PackageManager.NameNotFoundException) {
                // Expected on an emulator.
            }
        }
        val s = BikeSensors(context).also { sensors = it }
        s.start()
        await("Unavailable") { s.state is BikeState.Unavailable && s.state != BikeState.Unavailable("not started") }
        val reason = (s.state as BikeState.Unavailable).reason
        assertTrue(reason, reason.contains("com.onepeloton.affernetservice is not installed"))
        assertTrue(reason, reason.contains("com.peloton.service.SensorData is not installed"))
        assertNull(s.latest)
    }

    @Test
    fun reportsUnavailableAndKeepsRetrying_whenServicesExistButSendNothing() {
        script(FakeScript(affernetMode = "silent", polling = false, repeating = false))
        val s = start(both)
        await("Unavailable") { s.state is BikeState.Unavailable && states.size >= 2 }
        val reason = (s.state as BikeState.Unavailable).reason
        assertTrue(reason, reason.contains("Affernet IV1Interface callbacks: bound, but sent no valid data"))
        assertTrue(reason, reason.contains("SensorData Messenger, polled reads: bound, but sent no valid data"))
        // The bike wakes up: the retry finds it.
        script(FakeScript())
        awaitConnected(s, BikeRoute.AFFERNET)
    }

    // Pace

    @Test
    fun bikePaceSource_followsTheBikeWhenLiveAndTheFallbackOtherwise() {
        script(FakeScript(rpm = 60))
        val fallback = object : PaceSource {
            override val metersPerSecond = 1.25f
        }
        val bike = BikePaceSource(BikeSensors(context, both, timing).also { sensors = it })
        val pace = bike.orElse(fallback)
        assertFalse(bike.isLive)
        assertEquals(1.25f, pace.metersPerSecond)

        bike.start()
        await("live") { bike.isLive }
        assertTrue(pace.usingBike)
        // 60 rpm x 0.042 = 2.52 m/s, reached after a few seconds of smoothing.
        await("about 2.5 m/s") { abs(pace.metersPerSecond - 2.52f) < 0.1f }

        bike.stop()
        assertFalse(bike.isLive)
        assertEquals(1.25f, pace.metersPerSecond)
    }

    // Diagnostics screen

    @Test
    fun diagnosticsActivity_opensShowsLiveStateAndWritesTheReport() {
        BikeDiagnosticsActivity.endpointsForTest = both
        BikeDiagnosticsActivity.timingForTest = timing
        val instrumentation = InstrumentationRegistry.getInstrumentation()
        val activity = instrumentation.startActivitySync(
            BikeDiagnosticsActivity.intent(context).addFlags(android.content.Intent.FLAG_ACTIVITY_NEW_TASK),
        ) as BikeDiagnosticsActivity
        try {
            fun views(): List<TextView> {
                val found = ArrayList<TextView>()
                instrumentation.runOnMainSync {
                    fun walk(v: View) {
                        if (v is TextView) found += v
                        if (v is ViewGroup) for (i in 0 until v.childCount) walk(v.getChildAt(i))
                    }
                    walk(activity.window.decorView)
                }
                return found
            }
            fun texts() = views().map { it.text.toString() }

            await("live values on screen") {
                val t = texts()
                t.any { it == "State: Connected (Affernet IV1Interface callbacks)" } &&
                    t.any { it == "Cadence: 72.0 rpm" } && t.any { it == "Power: 123.4 W" } &&
                    t.any { it == "Resistance: 37.0 %" }
            }
            val shown = texts()
            assertTrue(shown.toString(), shown.any { it == "Route: Affernet IV1Interface callbacks" })
            assertTrue(shown.toString(), shown.any { it.startsWith("Pace from cadence: ") })
            assertTrue(shown.toString(), shown.any { it.startsWith("Readings per second: ") })
            assertTrue(shown.toString(), shown.any { it.startsWith("Last error: ") })
            assertTrue(shown.toString(), shown.any { it.contains("Route A service: $here/dev.soloton.bike.fake.FakeAffernetService") })
            assertTrue(shown.toString(), shown.any { it.contains("Package $here: version") })
            assertTrue(shown.toString(), shown.any { it.contains("registered a callback as \"Soloton\"") })

            // Sizes: at least 28 px text and 112 px buttons on a 1080-line screen.
            val metrics = activity.resources.displayMetrics
            val unit = minOf(metrics.widthPixels, metrics.heightPixels) / 1080f
            val all = views()
            assertTrue(all.all { it.textSize >= 28f * unit - 0.5f })
            val buttons = all.filterIsInstance<Button>()
            assertEquals(listOf("Copy report", "Reconnect", "Close"), buttons.map { it.text.toString() })
            assertTrue(buttons.all { it.height >= 112f * unit - 1f })

            val report = File(context.getExternalFilesDir(null) ?: context.filesDir, "bike-diagnostics.txt")
            report.delete()
            instrumentation.runOnMainSync { buttons.first().performClick() }
            await("the report file") { report.exists() && report.length() > 0 }
            val text = report.readText()
            for (expected in listOf(
                "Soloton bike sensor report", "App: $here", "Device: ", "State: Connected",
                "Cadence: 72.0 rpm", "Power: 123.4 W", "Resistance: 37.0 %", "Route A service: ", "== Events",
            )) {
                assertTrue("report lacks \"$expected\":\n$text", text.contains(expected))
            }
            assertTrue(texts().any { it.contains("adb pull ${report.path}") })
        } finally {
            instrumentation.runOnMainSync { activity.finish() }
            BikeDiagnosticsActivity.endpointsForTest = null
            BikeDiagnosticsActivity.timingForTest = null
            SystemClock.sleep(500)
        }
    }
}
