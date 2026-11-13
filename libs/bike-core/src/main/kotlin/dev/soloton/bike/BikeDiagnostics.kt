// SPDX-License-Identifier: GPL-2.0-or-later
package dev.soloton.bike

/** A point-in-time copy of everything [BikeSensors] knows, for display and for the report. */
data class BikeDiagnostics(
    val state: BikeState,
    /** The route being tried or in use, or null. */
    val route: BikeRoute?,
    val latest: BikeReading?,
    val readingsPerSecond: Float,
    val readingsAccepted: Long,
    val messagesRejected: Long,
    val lastError: String?,
    /** Oldest first. */
    val events: List<String>,
)

/** Builds the plain-text report the owner copies off the bike. No Android imports. */
object BikeReport {
    const val FILE_NAME = "bike-diagnostics.txt"

    /**
     * @param header free-form lines identifying the app, device and time
     * @param inventory lines describing installed Peloton packages and services
     * @param paceMetersPerSecond the pace computed from cadence
     */
    fun text(
        header: List<String>,
        inventory: List<String>,
        diagnostics: BikeDiagnostics,
        paceMetersPerSecond: Float,
    ): String = buildString {
        appendLine("Soloton bike sensor report")
        header.forEach { appendLine(it) }
        appendLine()
        appendLine("== Packages and services ==")
        inventory.forEach { appendLine(it) }
        appendLine()
        appendLine("== Connection ==")
        appendLine("State: ${diagnostics.state.description}")
        appendLine("Route: ${diagnostics.route?.label ?: "none"}")
        appendLine("Last error: ${diagnostics.lastError ?: "none"}")
        appendLine()
        appendLine("== Live values ==")
        val reading = diagnostics.latest
        if (reading == null) {
            appendLine("No reading received")
        } else {
            appendLine("Cadence: ${one(reading.cadenceRpm)} rpm")
            appendLine("Power: ${one(reading.powerWatts)} W")
            appendLine("Resistance: ${one(reading.resistancePercent)} %")
        }
        appendLine("Readings per second: ${one(diagnostics.readingsPerSecond)}")
        appendLine("Readings accepted: ${diagnostics.readingsAccepted}")
        appendLine("Messages rejected: ${diagnostics.messagesRejected}")
        appendLine("Pace from cadence: ${two(paceMetersPerSecond)} m/s")
        appendLine()
        appendLine("== Events (seconds since start, oldest first) ==")
        if (diagnostics.events.isEmpty()) appendLine("none")
        diagnostics.events.forEach { appendLine(it) }
    }

    fun one(value: Float): String = "%.1f".format(java.util.Locale.ROOT, value)
    fun two(value: Float): String = "%.2f".format(java.util.Locale.ROOT, value)
}
