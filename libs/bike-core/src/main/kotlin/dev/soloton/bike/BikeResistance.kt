// SPDX-License-Identifier: GPL-2.0-or-later
package dev.soloton.bike


/**
 * The position of the bike's resistance knob as a 0 to 1 fraction, from [BikeReading]s.
 *
 * If no reading arrives for [staleAfterSeconds] the fraction falls back to 0, so a bike that has
 * gone quiet does not leave a game stuck at its last setting. Smoothing is left to the reader
 * (the game eases it). [onReading] may be called from any thread; [fraction] and [isFresh] are
 * cheap and thread-safe, for the render thread.
 *
 * @param nanoTime clock, replaceable in tests
 */
class BikeResistance(
    val staleAfterSeconds: Float = 3f,
    private val nanoTime: () -> Long = System::nanoTime,
) : ResistanceSource {
    private val lock = Any()
    private var percent = 0f
    private var lastNanos = 0L
    private var hasReading = false

    init {
        require(staleAfterSeconds > 0f) { "staleAfterSeconds must be positive" }
    }

    /** Feed a resistance in percent. Values that are not finite or are negative are ignored. */
    fun onResistance(resistancePercent: Float) {
        if (resistancePercent.isNaN() || resistancePercent.isInfinite() || resistancePercent < 0f) return
        val now = nanoTime()
        synchronized(lock) {
            percent = minOf(resistancePercent, BikeLimits.MAX_RESISTANCE)
            lastNanos = now
            hasReading = true
        }
    }

    /** Convenience for wiring to [BikeReading]s. */
    fun onReading(reading: BikeReading) = onResistance(reading.resistancePercent)

    override val fraction: Float
        get() {
            val now = nanoTime()
            synchronized(lock) { return if (fresh(now)) percent / BikeLimits.MAX_RESISTANCE else 0f }
        }

    /** True while the last reading is younger than [staleAfterSeconds]. */
    val isFresh: Boolean
        get() {
            val now = nanoTime()
            synchronized(lock) { return fresh(now) }
        }

    private fun fresh(now: Long) = hasReading && (now - lastNanos) < (staleAfterSeconds * NANOS).toLong()

    private companion object {
        const val NANOS = 1_000_000_000.0
    }
}

/**
 * Uses [bike] while [bikeIsLive] says the bike is delivering data, and [fallback] (for example a
 * resistance set from the keyboard) otherwise. The switch is checked on every read.
 */
class BikeOrFallbackResistance(
    private val bike: ResistanceSource,
    private val bikeIsLive: () -> Boolean,
    private val fallback: ResistanceSource,
) : ResistanceSource {
    override val fraction: Float
        get() = if (bikeIsLive()) bike.fraction else fallback.fraction
}
