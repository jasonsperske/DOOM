// SPDX-License-Identifier: GPL-2.0-or-later
package dev.soloton.bike

/**
 * How fast the rider is going. This and [ResistanceSource] are the two things a game reads from a
 * bike: write the game against them, and it runs unchanged whether they are backed by the real
 * bike ([CadencePace], [BikeResistance]) or by on-screen buttons, a keyboard or a test.
 *
 * Polled every frame from the render thread; reads must be cheap and thread-safe.
 */
interface PaceSource {
    /** Current forward speed in metres per second, 0 or more. */
    val metersPerSecond: Float
}

/**
 * How hard the rider has set the bike. A game may use it as a difficulty or intensity setting.
 *
 * Polled every frame from the render thread; reads must be cheap and thread-safe.
 */
interface ResistanceSource {
    /** 0 (no resistance) to 1 (the knob at its end). */
    val fraction: Float
}
