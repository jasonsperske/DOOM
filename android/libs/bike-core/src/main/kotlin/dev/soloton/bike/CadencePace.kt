// SPDX-License-Identifier: GPL-2.0-or-later
package dev.soloton.bike

import kotlin.math.exp

/**
 * Turns pedalling cadence into the runner's speed.
 *
 * `target = clamp(rpm * metersPerSecondPerRpm, 0, maxMetersPerSecond)`, so with the defaults
 * 60 rpm is about 2.5 m/s (a jog) and 90 rpm about 3.8 m/s. The value returned eases toward the
 * target with time constant [smoothingSeconds], which hides the bike's coarse (about 1 Hz)
 * updates. If no cadence arrives for [staleAfterSeconds] the target becomes 0 and the pace eases
 * down to a stop.
 *
 * [onCadence] may be called from any thread. [metersPerSecond] is meant for the render thread:
 * one short uncontended lock and no allocation.
 *
 * @param nanoTime clock, replaceable in tests
 */
class CadencePace(
    val metersPerSecondPerRpm: Float = DEFAULT_METERS_PER_SECOND_PER_RPM,
    val maxMetersPerSecond: Float = DEFAULT_MAX_METERS_PER_SECOND,
    val smoothingSeconds: Float = 1f,
    val staleAfterSeconds: Float = 3f,
    private val nanoTime: () -> Long = System::nanoTime,
) : PaceSource {
    private val lock = Any()
    private var target = 0f
    private var smoothed = 0f
    private var lastCadenceRpm = 0f
    private var lastCadenceNanos = 0L
    private var hasCadence = false
    private var lastStepNanos = nanoTime()

    init {
        require(metersPerSecondPerRpm >= 0f && maxMetersPerSecond >= 0f) { "pace mapping must not be negative" }
        require(smoothingSeconds >= 0f && staleAfterSeconds > 0f) { "times must be positive" }
    }

    /** Feed a cadence sample. Values that are not finite or are negative are ignored. */
    fun onCadence(rpm: Float) {
        if (rpm.isNaN() || rpm.isInfinite() || rpm < 0f) return
        val now = nanoTime()
        synchronized(lock) {
            step(now)
            lastCadenceRpm = rpm
            lastCadenceNanos = now
            hasCadence = true
            target = targetFor(rpm)
        }
    }

    /** Convenience for wiring to [BikeReading]s. */
    fun onReading(reading: BikeReading) = onCadence(reading.cadenceRpm)

    /** The unsmoothed speed for [rpm]. */
    fun targetFor(rpm: Float): Float = (rpm * metersPerSecondPerRpm).coerceIn(0f, maxMetersPerSecond)

    override val metersPerSecond: Float
        get() {
            val now = nanoTime()
            synchronized(lock) {
                step(now)
                return smoothed
            }
        }

    /** True while the last cadence sample is younger than [staleAfterSeconds]. */
    val isFresh: Boolean
        get() {
            val now = nanoTime()
            synchronized(lock) { return fresh(now) }
        }

    /** The last cadence fed in, for display. */
    val cadenceRpm: Float
        get() = synchronized(lock) { lastCadenceRpm }

    /** Forget everything and stop. */
    fun reset() {
        val now = nanoTime()
        synchronized(lock) {
            target = 0f
            smoothed = 0f
            hasCadence = false
            lastStepNanos = now
        }
    }

    private fun fresh(now: Long) = hasCadence && (now - lastCadenceNanos) < (staleAfterSeconds * NANOS).toLong()

    /** Advances the smoothing to [now]. Caller holds the lock. */
    private fun step(now: Long) {
        val dt = (now - lastStepNanos) / NANOS
        lastStepNanos = now
        val goal = if (fresh(now)) target else 0f
        if (dt <= 0.0) return
        smoothed = if (smoothingSeconds <= 0f) {
            goal
        } else {
            val keep = exp(-dt / smoothingSeconds).toFloat()
            goal + (smoothed - goal) * keep
        }
        // Do not creep toward zero forever.
        if (goal == 0f && smoothed < STOPPED) smoothed = 0f
    }

    companion object {
        const val DEFAULT_METERS_PER_SECOND_PER_RPM = 0.042f
        const val DEFAULT_MAX_METERS_PER_SECOND = 6.0f
        private const val NANOS = 1_000_000_000.0
        private const val STOPPED = 0.01f
    }
}

/**
 * Uses [bike] while [bikeIsLive] says the bike is delivering data, and [fallback] (for example
 * the on-screen slower/faster buttons) otherwise. The switch is checked on every read.
 */
class BikeOrFallbackPace(
    private val bike: PaceSource,
    private val bikeIsLive: () -> Boolean,
    private val fallback: PaceSource,
) : PaceSource {
    override val metersPerSecond: Float
        get() = if (bikeIsLive()) bike.metersPerSecond else fallback.metersPerSecond

    /** Whether the last answer would come from the bike. */
    val usingBike: Boolean
        get() = bikeIsLive()
}
