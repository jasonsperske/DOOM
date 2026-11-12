// SPDX-License-Identifier: GPL-2.0-or-later
package dev.soloton.bike

import kotlin.math.exp
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertIs
import kotlin.test.assertNull
import kotlin.test.assertTrue

private const val S = LinkTiming.SECOND

private class FakeClock(var nanos: Long = 1_000 * S) {
    fun advance(seconds: Double) {
        nanos += (seconds * S).toLong()
    }
}

class CadencePaceTest {
    private val clock = FakeClock()
    private fun pace(smoothing: Float = 1f) = CadencePace(smoothingSeconds = smoothing, nanoTime = { clock.nanos })

    /** Feed [rpm] every 0.1 s for [seconds]. */
    private fun CadencePace.pedal(rpm: Float, seconds: Double) {
        repeat((seconds * 10).toInt()) {
            onCadence(rpm)
            clock.advance(0.1)
        }
    }

    @Test
    fun sixtyRpmIsAboutTwoAndAHalfMetresPerSecond() {
        val p = pace()
        assertEquals(2.52f, p.targetFor(60f), 0.001f)
        assertEquals(3.78f, p.targetFor(90f), 0.001f)
        p.pedal(60f, 10.0)
        assertEquals(2.52f, p.metersPerSecond, 0.01f)
    }

    @Test
    fun mappingIsClampedAndConfigurable() {
        assertEquals(6f, pace().targetFor(200f))
        assertEquals(0f, pace().targetFor(0f))
        val custom = CadencePace(metersPerSecondPerRpm = 0.1f, maxMetersPerSecond = 4f, nanoTime = { clock.nanos })
        assertEquals(3f, custom.targetFor(30f), 0.0001f)
        assertEquals(4f, custom.targetFor(90f))
    }

    @Test
    fun startsAtRestAndEasesTowardTheTargetWithAOneSecondTimeConstant() {
        val p = pace()
        assertEquals(0f, p.metersPerSecond)
        p.onCadence(60f)
        assertEquals(0f, p.metersPerSecond, 0.0001f)
        clock.advance(1.0)
        assertEquals(2.52f * (1 - exp(-1.0).toFloat()), p.metersPerSecond, 0.001f) // about 63 %
        clock.advance(1.0)
        assertEquals(2.52f * (1 - exp(-2.0).toFloat()), p.metersPerSecond, 0.001f)
    }

    @Test
    fun readingOftenOrRarelyGivesTheSameCurve() {
        val often = pace()
        often.onCadence(80f)
        repeat(100) {
            clock.advance(0.01)
            often.metersPerSecond
        }
        val stepped = often.metersPerSecond
        val rarely = pace()
        rarely.onCadence(80f)
        clock.advance(1.0)
        assertEquals(rarely.metersPerSecond, stepped, 0.001f)
    }

    @Test
    fun zeroSmoothingFollowsAtOnce() {
        val p = pace(smoothing = 0f)
        p.onCadence(60f)
        clock.advance(0.001)
        assertEquals(2.52f, p.metersPerSecond, 0.0001f)
    }

    @Test
    fun staysFreshForThreeSecondsThenDecaysToAStop() {
        val p = pace()
        p.pedal(60f, 10.0)
        clock.advance(2.8) // 2.9 s since the last sample
        assertTrue(p.isFresh)
        assertEquals(2.52f, p.metersPerSecond, 0.01f)
        clock.advance(0.2)
        assertFalse(p.isFresh)
        clock.advance(1.0)
        val slowing = p.metersPerSecond
        assertTrue(slowing in 0.5f..1.2f, "decaying, was $slowing")
        clock.advance(10.0)
        assertEquals(0f, p.metersPerSecond)
    }

    @Test
    fun pedallingAgainAfterGoingStaleRecovers() {
        val p = pace()
        p.pedal(60f, 5.0)
        clock.advance(20.0)
        assertEquals(0f, p.metersPerSecond)
        p.pedal(60f, 10.0)
        assertTrue(p.isFresh)
        assertEquals(2.52f, p.metersPerSecond, 0.01f)
    }

    @Test
    fun stoppingPedallingWhileConnectedSlowsToZero() {
        val p = pace()
        p.pedal(90f, 10.0)
        p.pedal(0f, 10.0)
        assertTrue(p.isFresh)
        assertEquals(0f, p.metersPerSecond)
    }

    @Test
    fun garbageCadenceIsIgnored() {
        val p = pace()
        p.pedal(60f, 10.0)
        p.onCadence(Float.NaN)
        p.onCadence(-5f)
        p.onCadence(Float.POSITIVE_INFINITY)
        assertEquals(60f, p.cadenceRpm)
        assertEquals(2.52f, p.metersPerSecond, 0.01f)
    }

    @Test
    fun neverNegativeAndNeverAboveTheMaximum() {
        val p = pace()
        p.pedal(250f, 20.0)
        assertTrue(p.metersPerSecond <= 6f)
        p.reset()
        assertEquals(0f, p.metersPerSecond)
        assertFalse(p.isFresh)
    }

    @Test
    fun readsAreSafeFromSeveralThreads() {
        val p = CadencePace()
        val threads = List(4) { n ->
            Thread {
                repeat(20_000) {
                    if (n == 0) p.onCadence(60f + it % 30)
                    val v = p.metersPerSecond
                    check(v in 0f..6f) { "out of range: $v" }
                }
            }
        }
        val failures = ArrayList<Throwable>()
        threads.forEach { it.setUncaughtExceptionHandler { _, e -> synchronized(failures) { failures += e } } }
        threads.forEach { it.start() }
        threads.forEach { it.join() }
        assertTrue(failures.isEmpty(), failures.toString())
    }
}

class BikeOrFallbackPaceTest {
    private class Fixed(override val metersPerSecond: Float) : PaceSource

    @Test
    fun usesTheBikeOnlyWhileItIsLive() {
        var live = false
        val pace = BikeOrFallbackPace(Fixed(3.5f), { live }, Fixed(2f))
        assertEquals(2f, pace.metersPerSecond)
        assertFalse(pace.usingBike)
        live = true
        assertEquals(3.5f, pace.metersPerSecond)
        assertTrue(pace.usingBike)
        live = false
        assertEquals(2f, pace.metersPerSecond)
    }

    @Test
    fun aLiveBikeAtRestMeansStandingStillNotTheFallbackSpeed() {
        val pace = BikeOrFallbackPace(Fixed(0f), { true }, Fixed(2f))
        assertEquals(0f, pace.metersPerSecond)
    }
}

class BikeLinkTest {
    private val timing = LinkTiming()
    private val all = listOf(BikeRoute.AFFERNET, BikeRoute.SENSORDATA_POLL, BikeRoute.SENSORDATA_REPEATING)
    private val open = { route: BikeRoute -> LinkAction.Open(route) }
    private val close = LinkAction.Close

    @Test
    fun beforeStartItIsUnavailable() {
        val link = BikeLink()
        assertEquals(BikeState.Unavailable("not started"), link.state)
        assertNull(link.route)
        assertTrue(link.onTick(0).isEmpty())
    }

    @Test
    fun withNoServiceInstalledItIsUnavailableForGoodAndBindsNothing() {
        val link = BikeLink()
        assertTrue(link.start(0, emptyList(), "no Peloton sensor service on this device").isEmpty())
        assertEquals(BikeState.Unavailable("no Peloton sensor service on this device"), link.state)
        assertTrue(link.onTick(1_000 * S).isEmpty())
        assertIs<BikeState.Unavailable>(link.state)
    }

    @Test
    fun theFirstRouteIsTriedFirstAndConnectsOnItsFirstSample() {
        val link = BikeLink()
        assertEquals(listOf<LinkAction>(open(BikeRoute.AFFERNET)), link.start(0, all, ""))
        assertEquals(BikeState.Connecting, link.state)
        assertEquals(BikeRoute.AFFERNET, link.route)
        link.onBound()
        assertEquals(BikeState.Connecting, link.state) // bound is not connected
        assertTrue(link.onTick(4 * S).isEmpty())
        link.onFrame(4 * S)
        assertEquals(BikeState.Connected(BikeRoute.AFFERNET), link.state)
        assertTrue(link.onTick(6 * S).isEmpty())
    }

    @Test
    fun aSilentRouteGivesWayToTheNextAfterFiveSeconds() {
        val link = BikeLink()
        link.start(0, all, "")
        link.onBound()
        assertTrue(link.onTick(5 * S - 1).isEmpty())
        assertEquals(listOf(close, open(BikeRoute.SENSORDATA_POLL)), link.onTick(5 * S))
        assertEquals(BikeState.Connecting, link.state)
        link.onFrame(6 * S)
        assertEquals(BikeState.Connected(BikeRoute.SENSORDATA_POLL), link.state)
    }

    @Test
    fun aRouteThatFailsToBindGivesWayAtOnce() {
        val link = BikeLink()
        link.start(0, all, "")
        assertEquals(listOf(close, open(BikeRoute.SENSORDATA_POLL)), link.onFailure(S / 10, "not permitted to bind"))
        assertEquals(listOf(close, open(BikeRoute.SENSORDATA_REPEATING)), link.onTick(S / 10 + 5 * S))
        assertEquals(BikeRoute.SENSORDATA_REPEATING, link.route)
    }

    @Test
    fun whenEveryRouteFailsItSaysWhatEachDidAndRetriesWithAGrowingPause() {
        val link = BikeLink()
        link.start(0, all, "")
        link.onFailure(0, "not permitted to bind")
        link.onBound()
        link.onTick(5 * S)
        assertEquals(listOf<LinkAction>(close), link.onTick(10 * S))
        val state = assertIs<BikeState.Unavailable>(link.state)
        assertEquals(
            "Affernet IV1Interface callbacks: not permitted to bind; " +
                "SensorData Messenger, polled reads: bound, but sent no valid data within 5 s; " +
                "SensorData Messenger, repeating commands: did not connect within 5 s",
            state.reason,
        )
        assertNull(link.route)

        // First retry after 1 s.
        assertTrue(link.onTick(11 * S - 1).isEmpty())
        assertEquals(listOf<LinkAction>(open(BikeRoute.AFFERNET)), link.onTick(11 * S))
        assertEquals(BikeState.Connecting, link.state)
        // Fail all three again: the next pause is 2 s.
        var now = 11 * S
        repeat(3) { link.onFailure(now, "nope") }
        assertIs<BikeState.Unavailable>(link.state)
        assertTrue(link.onTick(now + 2 * S - 1).isEmpty())
        assertEquals(listOf<LinkAction>(open(BikeRoute.AFFERNET)), link.onTick(now + 2 * S))
        // The pause never exceeds the maximum.
        now += 2 * S
        var waitedTheMaximum = 0
        repeat(10) {
            repeat(3) { link.onFailure(now, "nope") }
            // Either the pause has already ended just before the maximum, or it ends exactly at it.
            if (link.onTick(now + timing.retryMaxNanos - 1).isEmpty()) {
                waitedTheMaximum++
                assertEquals(1, link.onTick(now + timing.retryMaxNanos).size)
            }
            now += timing.retryMaxNanos
        }
        assertEquals(7, waitedTheMaximum) // pauses of 4, 8 and 16 s, then 30 s every time
    }

    @Test
    fun silenceWhileConnectedBecomesNoDataThenARebind() {
        val link = BikeLink()
        link.start(0, all, "")
        link.onFrame(S)
        assertTrue(link.onTick(S + 3 * S - 1).isEmpty())
        assertEquals(BikeState.Connected(BikeRoute.AFFERNET), link.state)
        assertTrue(link.onTick(4 * S).isEmpty())
        assertEquals(BikeState.NoData, link.state)
        assertEquals(BikeRoute.AFFERNET, link.route) // still bound, still listening

        // Data returns: connected again without rebinding.
        link.onFrame(5 * S)
        assertEquals(BikeState.Connected(BikeRoute.AFFERNET), link.state)

        // Ten seconds of silence: drop the binding and start over after the pause.
        assertEquals(listOf<LinkAction>(close), link.onTick(15 * S))
        assertEquals(BikeState.NoData, link.state)
        assertNull(link.route)
        assertEquals(listOf<LinkAction>(open(BikeRoute.AFFERNET)), link.onTick(16 * S))
        assertEquals(BikeState.Connecting, link.state)
    }

    @Test
    fun aDeadServiceWhileConnectedIsAnErrorThenARetry() {
        val link = BikeLink()
        link.start(0, all, "")
        link.onFrame(S)
        assertEquals(listOf<LinkAction>(close), link.onFailure(2 * S, "the service's process died"))
        assertEquals(BikeState.Error("Affernet IV1Interface callbacks: the service's process died"), link.state)
        assertTrue(link.onTick(3 * S - 1).isEmpty())
        assertEquals(listOf<LinkAction>(open(BikeRoute.AFFERNET)), link.onTick(3 * S))
        link.onFrame(3 * S + 1)
        assertEquals(BikeState.Connected(BikeRoute.AFFERNET), link.state)
    }

    @Test
    fun connectingResetsTheRetryPause() {
        val link = BikeLink()
        link.start(0, listOf(BikeRoute.AFFERNET), "")
        link.onFailure(0, "x") // pause 1 s, next would be 2 s
        link.onTick(S)
        link.onFailure(S, "x") // pause 2 s, next would be 4 s
        link.onTick(3 * S)
        link.onFrame(3 * S)
        link.onFailure(4 * S, "died")
        assertTrue(link.onTick(5 * S - 1).isEmpty())
        assertEquals(1, link.onTick(5 * S).size) // back to 1 s
    }

    @Test
    fun stopClosesAnOpenRouteAndIgnoresLateEvents() {
        val link = BikeLink()
        link.start(0, all, "")
        link.onFrame(S)
        assertEquals(listOf<LinkAction>(close), link.stop())
        assertEquals(BikeState.Unavailable("stopped"), link.state)
        link.onFrame(2 * S)
        assertTrue(link.onFailure(2 * S, "late").isEmpty())
        assertTrue(link.onTick(100 * S).isEmpty())
        assertEquals(BikeState.Unavailable("stopped"), link.state)
        assertTrue(link.stop().isEmpty())
    }

    @Test
    fun startingAgainClosesWhatWasOpen() {
        val link = BikeLink()
        link.start(0, all, "")
        assertEquals(listOf(close, open(BikeRoute.SENSORDATA_POLL)), link.start(S, listOf(BikeRoute.SENSORDATA_POLL), ""))
    }

    @Test
    fun statesDescribeThemselves() {
        assertEquals("Connected (Affernet IV1Interface callbacks)", BikeState.Connected(BikeRoute.AFFERNET).description)
        assertEquals("Unavailable: nothing here", BikeState.Unavailable("nothing here").description)
        assertEquals("Error: boom", BikeState.Error("boom").description)
        assertEquals("Connecting", BikeState.Connecting.description)
        assertTrue(BikeState.NoData.description.startsWith("No data"))
    }
}

class DiagnosticsTest {
    @Test
    fun eventLogKeepsTheLastFiftyWithTimes() {
        val clock = FakeClock()
        val log = EventLog(nanoTime = { clock.nanos })
        repeat(60) {
            clock.advance(0.5)
            log.add("event $it")
        }
        val lines = log.snapshot()
        assertEquals(50, lines.size)
        assertTrue(lines.first().endsWith("event 10"), lines.first())
        assertEquals("  30.000  event 59", lines.last())
    }

    @Test
    fun rateMeterCountsTheLastThreeSeconds() {
        val meter = RateMeter()
        assertEquals(0f, meter.perSecond(0))
        for (i in 0 until 100) meter.mark(i * S / 10) // 10 Hz for 10 s
        assertEquals(10f, meter.perSecond(10 * S - 1), 0.4f)
        assertEquals(0f, meter.perSecond(20 * S))
        meter.reset()
        assertEquals(0f, meter.perSecond(10 * S))
    }

    @Test
    fun reportListsEverythingTheOwnerNeedsToSendBack() {
        val text = BikeReport.text(
            header = listOf("App: dev.soloton.example"),
            inventory = listOf("Package com.onepeloton.affernetservice: version 2.4 (2040490)"),
            diagnostics = BikeDiagnostics(
                state = BikeState.Connected(BikeRoute.AFFERNET),
                route = BikeRoute.AFFERNET,
                latest = BikeReading(81f, 123.46f, 37f, 0),
                readingsPerSecond = 1.0f,
                readingsAccepted = 42,
                messagesRejected = 3,
                lastError = "cadence out of range: -1",
                events = listOf("   0.100  trying Affernet IV1Interface callbacks"),
            ),
            paceMetersPerSecond = 3.402f,
        )
        for (expected in listOf(
            "App: dev.soloton.example",
            "Package com.onepeloton.affernetservice: version 2.4 (2040490)",
            "State: Connected (Affernet IV1Interface callbacks)",
            "Route: Affernet IV1Interface callbacks",
            "Cadence: 81.0 rpm",
            "Power: 123.5 W",
            "Resistance: 37.0 %",
            "Readings per second: 1.0",
            "Readings accepted: 42",
            "Messages rejected: 3",
            "Pace from cadence: 3.40 m/s",
            "Last error: cadence out of range: -1",
            "trying Affernet IV1Interface callbacks",
        )) {
            assertTrue(text.contains(expected), "missing \"$expected\" in:\n$text")
        }
    }

    @Test
    fun reportSaysSoWhenNothingWasReceived() {
        val text = BikeReport.text(
            emptyList(),
            emptyList(),
            BikeDiagnostics(BikeState.Unavailable("no service"), null, null, 0f, 0, 0, null, emptyList()),
            0f,
        )
        assertTrue(text.contains("No reading received"))
        assertTrue(text.contains("State: Unavailable: no service"))
        assertTrue(text.contains("Last error: none"))
    }
}
