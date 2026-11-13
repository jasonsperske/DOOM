// SPDX-License-Identifier: GPL-2.0-or-later
package dev.soloton.bike

/**
 * One sample from the bike.
 *
 * @property cadenceRpm crank revolutions per minute, 0 or more
 * @property powerWatts rider output in watts, 0 or more
 * @property resistancePercent position of the resistance knob, 0 to 100
 * @property timestampNanos when the sample was received, on the [System.nanoTime] clock
 */
data class BikeReading(
    val cadenceRpm: Float,
    val powerWatts: Float,
    val resistancePercent: Float,
    val timestampNanos: Long,
)

/** The outcome of decoding one message from a sensor service. */
sealed interface Decoded {
    /** A plausible sample. */
    data class Ok(val reading: BikeReading) : Decoded

    /** A well-formed message that carries no sample (the service sent "no data"). */
    data object Empty : Decoded

    /** A message that was malformed or carried impossible values. Nothing is passed on. */
    data class Rejected(val reason: String) : Decoded
}

/**
 * What counts as a believable sample. Anything outside these ranges is treated as a misread
 * parcel or a sensor glitch and dropped whole, never clamped into range and passed on. The one
 * exception is a resistance a few points above 100, which is a rounding overshoot.
 */
object BikeLimits {
    /** Track sprinters peak near 250 rpm; nobody does that on this bike. */
    const val MAX_CADENCE_RPM = 250f

    /** World-class sprint peaks are about 2500 W. */
    const val MAX_POWER_WATTS = 3000f
    const val MAX_RESISTANCE = 100f

    /** Resistance up to this is clamped to 100 instead of rejected. */
    const val RESISTANCE_OVERSHOOT = 105f

    /** Null when [rpm] is usable, otherwise why not. */
    fun cadenceProblem(rpm: Float): String? = when {
        rpm.isNaN() || rpm.isInfinite() -> "cadence is not a number"
        rpm < 0f -> "negative cadence $rpm"
        rpm > MAX_CADENCE_RPM -> "absurd cadence $rpm rpm"
        else -> null
    }

    fun powerProblem(watts: Float): String? = when {
        watts.isNaN() || watts.isInfinite() -> "power is not a number"
        watts < 0f -> "negative power $watts"
        watts > MAX_POWER_WATTS -> "absurd power $watts W"
        else -> null
    }

    fun resistanceProblem(percent: Float): String? = when {
        percent.isNaN() || percent.isInfinite() -> "resistance is not a number"
        percent < 0f -> "negative resistance $percent"
        percent > RESISTANCE_OVERSHOOT -> "absurd resistance $percent"
        else -> null
    }

    /** Builds a reading from values already in rpm, watts and percent, or says why it cannot. */
    fun reading(cadenceRpm: Float, powerWatts: Float, resistancePercent: Float, timestampNanos: Long): Decoded {
        val problem = cadenceProblem(cadenceRpm) ?: powerProblem(powerWatts) ?: resistanceProblem(resistancePercent)
        if (problem != null) return Decoded.Rejected(problem)
        return Decoded.Ok(
            BikeReading(cadenceRpm, powerWatts, minOf(resistancePercent, MAX_RESISTANCE), timestampNanos),
        )
    }
}

/**
 * Removes single-sample upward spikes by reporting the lowest of the last [window] values.
 * Both Grupetto versions note that the bike's resistance reading "occasionally spikes for a
 * single reading" and filter it this way. The cost is that a real increase shows up
 * `window - 1` samples late. Not thread-safe; the caller serialises access.
 */
class SpikeFilter(private val window: Int = 3) {
    private val recent = FloatArray(window)
    private var count = 0
    private var next = 0

    init {
        require(window >= 1) { "window must be at least 1" }
    }

    fun filter(value: Float): Float {
        recent[next] = value
        next = (next + 1) % window
        if (count < window) count++
        var lowest = recent[0]
        for (i in 1 until count) if (recent[i] < lowest) lowest = recent[i]
        return lowest
    }

    fun reset() {
        count = 0
        next = 0
    }
}
