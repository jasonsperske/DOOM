// SPDX-License-Identifier: GPL-2.0-or-later
package dev.soloton.bike

import android.content.Context

/**
 * The runner's pace from the bike's cadence: [BikeSensors] feeding a [CadencePace].
 *
 * ```
 * val bike = BikePaceSource(applicationContext)           // onCreate
 * val pace: PaceSource = bike.orElse(buttonsPace)         // hand this to the game
 * bike.start()                                            // onResume; bike.stop() in onPause
 * ```
 *
 * [metersPerSecond] and [isLive] are cheap and thread-safe, for the render thread.
 */
class BikePaceSource(
    val sensors: BikeSensors,
    val pace: CadencePace = CadencePace(),
) : PaceSource {
    constructor(context: Context) : this(BikeSensors(context))

    /** The resistance knob, from the same readings. */
    val resistance = BikeResistance()

    private val feed = object : BikeSensors.Listener {
        override fun onReading(reading: BikeReading) {
            pace.onReading(reading)
            resistance.onReading(reading)
        }
    }

    init {
        sensors.addListener(feed)
    }

    /**
     * True while the bike is connected and a cadence sample arrived within the pace's stale window.
     * A rider who has stopped pedalling is still live (the bike reports 0 rpm); a bike that is
     * absent, asleep or disconnected is not.
     */
    val isLive: Boolean
        get() = sensors.state is BikeState.Connected && pace.isFresh

    override val metersPerSecond: Float
        get() = pace.metersPerSecond

    fun start() = sensors.start()

    fun stop() = sensors.stop()

    /** A source that follows the bike while [isLive] and [fallback] the rest of the time. */
    fun orElse(fallback: PaceSource): BikeOrFallbackPace = BikeOrFallbackPace(this, { isLive }, fallback)

    /** The bike's resistance while [isLive] and [fallback] the rest of the time. */
    fun resistanceOrElse(fallback: ResistanceSource): BikeOrFallbackResistance =
        BikeOrFallbackResistance(resistance, { isLive && resistance.isFresh }, fallback)
}
