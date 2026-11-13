// SPDX-License-Identifier: GPL-2.0-or-later
package dev.soloton.bike

import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertTrue

class BikeResistanceTest {
    private var nanos = 1_000_000_000_000L
    private fun advance(seconds: Double) { nanos += (seconds * 1e9).toLong() }
    private fun resistance() = BikeResistance(nanoTime = { nanos })

    @Test
    fun aReadingBecomesAFractionOfTheKnobsRange() {
        val r = resistance()
        assertEquals(0f, r.fraction)
        assertFalse(r.isFresh)
        r.onReading(BikeReading(cadenceRpm = 80f, powerWatts = 150f, resistancePercent = 45f, timestampNanos = nanos))
        assertTrue(r.isFresh)
        assertEquals(0.45f, r.fraction, 1e-6f)
        r.onResistance(100f)
        assertEquals(1f, r.fraction)
        r.onResistance(104f)
        assertEquals(1f, r.fraction, "an overshoot is the end of the range")
        r.onResistance(0f)
        assertEquals(0f, r.fraction)
    }

    @Test
    fun readingsThatAreNotNumbersOrAreNegativeAreIgnored() {
        val r = resistance()
        r.onResistance(30f)
        for (bad in listOf(Float.NaN, Float.POSITIVE_INFINITY, -1f)) r.onResistance(bad)
        assertEquals(0.3f, r.fraction, 1e-6f)
    }

    @Test
    fun aBikeThatGoesQuietFallsBackToNoResistance() {
        val r = resistance()
        r.onResistance(60f)
        advance(2.9)
        assertEquals(0.6f, r.fraction, 1e-6f)
        advance(0.2)
        assertFalse(r.isFresh)
        assertEquals(0f, r.fraction)
        r.onResistance(20f)
        assertEquals(0.2f, r.fraction, 1e-6f)
    }

    @Test
    fun theFallbackIsUsedWheneverTheBikeIsNotLive() {
        var live = false
        val bike = resistance().also { it.onResistance(70f) }
        val keys = object : ResistanceSource { override val fraction = 0.25f }
        val source = BikeOrFallbackResistance(bike, { live }, keys)
        assertEquals(0.25f, source.fraction)
        live = true
        assertEquals(0.7f, source.fraction, 1e-6f)
        live = false
        assertEquals(0.25f, source.fraction)
    }
}
