// SPDX-License-Identifier: GPL-2.0-or-later
package dev.soloton.bike

/** The three values the legacy service is asked for, with the bike board's command byte for each. */
enum class BikeMetric(val opcode: Int, val repeatingCommand: Int) {
    CADENCE(0x41, 1),
    POWER(0x44, 2),

    /** Resistance already normalised to 0 to 100 by the board. See [SensorDataWire.OP_RAW_RESISTANCE]. */
    RESISTANCE(0x49, 3),
}

/**
 * Route B: the legacy `com.peloton.service.SensorData` service, which speaks `android.os.Messenger`
 * and relays packets to the bike's sensor board over the tablet's serial port.
 *
 * ## The serial protocol underneath
 *
 * Documented by the PeloMon project (Imran S. Haque, https://github.com/ihaque/pelomon and
 * https://ihaque.org/tag/pelomon.html), which tapped the cable between tablet and bike:
 * 19200 baud; the tablet asks, the bike answers.
 *
 * ```
 * request   F5 <op> <sum> F6                 read a live value
 *           F7 <i>  <sum> F6                 read entry i (00 to 1E) of the resistance table
 * response  F1 <op> <n> <n ASCII digits, least significant first> <sum> F6
 * ```
 *
 * `<sum>` is the low byte of the sum of every byte before it, and a response is `n + 5` bytes.
 * Commands: `41` cadence in rpm; `44` power in tenths of a watt; `4A` raw resistance (a sensor
 * count, four digits), which the tablet turns into 0 to 100 by linear interpolation in a 31-entry
 * calibration table stored on the bike, entry i standing for i * 100 / 30 percent. The Grupetto
 * fork additionally found `49`, which returns resistance already normalised.
 *
 * ## The two Messenger dialects
 *
 * **Polled raw reads.** Send message 12 with a Bundle holding `command` (a request packet as
 * comma-separated hex) and `requestId`; the reply is message 14 holding the same `requestId` and
 * `responseHexString`, the board's answer or the literal `TIME_OUT`.
 *
 * **Repeating commands.** Send message 1, 2 or 3 (cadence, power, resistance) once with a
 * `requestId` and a reply Messenger; the service then keeps sending messages whose Bundle holds
 * `responseHexString`, a pre-parsed float `data` and an epoch `time`.
 */
object SensorDataWire {
    const val PACKAGE = "com.peloton.service.SensorData"
    const val ACTION = "android.intent.action.peloton.SensorData"
    const val CATEGORY_BIKE = "com.peloton.sensor.category.BIKE"
    const val MESSENGER_INTERFACE = "android.os.IMessenger"

    const val MESSAGE_READ_REQUEST = 12
    const val MESSAGE_READ_RESPONSE = 14

    const val KEY_COMMAND = "command"
    const val KEY_REQUEST_ID = "requestId"
    const val KEY_RESPONSE_HEX = "responseHexString"
    const val KEY_VALUE = "data"
    const val KEY_TIME = "time"
    const val TIMED_OUT = "TIME_OUT"

    const val DECIWATTS_PER_WATT = 10f

    /** Raw resistance sensor count; needs the [ResistanceTable]. */
    const val OP_RAW_RESISTANCE = 0x4A

    /** Both the first byte of a table request and the command byte of its response. */
    const val OP_TABLE_ENTRY = 0xF7

    private const val READ_START = 0xF5
    private const val RESPONSE_START = 0xF1
    private const val END = 0xF6
    private const val MAX_DIGITS = 6
    private const val MAX_TEXT = 128
    private val separators = Regex("[\\s,]+")

    /** The packet that reads live value [opcode], as the service wants it: `F5,41,36,F6` for cadence. */
    fun readPacket(opcode: Int): String = packet(READ_START, opcode)

    fun readPacket(metric: BikeMetric): String = readPacket(metric.opcode)

    /** The packet that reads resistance table entry [index] (0 to 30). */
    fun tablePacket(index: Int): String {
        require(index in 0 until ResistanceTable.SIZE) { "table index out of range: $index" }
        return packet(OP_TABLE_ENTRY, index)
    }

    private fun packet(first: Int, second: Int): String =
        intArrayOf(first, second, (first + second) and 0xFF, END).joinToString(",") { "%02X".format(it) }

    /** A structurally valid response packet: which command it answers and the integer it carries. */
    data class Packet(val opcode: Int, val value: Int)

    sealed interface Parsed {
        data class Valid(val packet: Packet) : Parsed
        data class Invalid(val reason: String) : Parsed
    }

    /** Checks framing, length, digits and checksum of a `responseHexString`. Never throws. */
    fun parseResponse(hex: String?): Parsed {
        if (hex == null) return Parsed.Invalid("no response text")
        if (hex.trim() == TIMED_OUT) return Parsed.Invalid("the service reported TIME_OUT")
        if (hex.length > MAX_TEXT) return Parsed.Invalid("response text too long")
        val tokens = hex.trim().split(separators)
        val bytes = IntArray(tokens.size)
        for ((i, token) in tokens.withIndex()) {
            val byte = if (token.length == 2 && token.all { it.isLetterOrDigit() }) token.toIntOrNull(16) else null
            if (byte == null) return Parsed.Invalid("not hex bytes: \"${hex.take(40)}\"")
            bytes[i] = byte
        }
        // start, opcode, count, at least one digit, checksum, end
        if (bytes.size < 6) return Parsed.Invalid("response too short")
        if (bytes[0] != RESPONSE_START || bytes[bytes.size - 1] != END) return Parsed.Invalid("bad framing bytes")
        val digits = bytes[2]
        if (digits !in 1..MAX_DIGITS || bytes.size != digits + 5) return Parsed.Invalid("length byte does not match")
        var sum = 0
        for (i in 0 until bytes.size - 2) sum += bytes[i]
        if (sum and 0xFF != bytes[bytes.size - 2]) return Parsed.Invalid("checksum mismatch")
        var value = 0
        for (i in 2 + digits downTo 3) {
            val digit = bytes[i] - '0'.code
            if (digit !in 0..9) return Parsed.Invalid("non-digit in value")
            value = value * 10 + digit
        }
        return Parsed.Valid(Packet(bytes[1], value))
    }

    /** The integer in a response that must answer command [opcode], or null with the reason via [onProblem]. */
    fun rawValue(opcode: Int, hex: String?, onProblem: (String) -> Unit = {}): Int? {
        val packet = when (val parsed = parseResponse(hex)) {
            is Parsed.Invalid -> {
                onProblem(parsed.reason)
                return null
            }
            is Parsed.Valid -> parsed.packet
        }
        if (packet.opcode != opcode) {
            onProblem("answer to command %02X, expected %02X".format(packet.opcode, opcode))
            return null
        }
        return packet.value
    }

    /** The value of a raw response for [metric] in rpm, watts or percent, or why it was rejected. */
    fun decode(metric: BikeMetric, hex: String?): MetricDecoded {
        var problem = "invalid"
        val raw = rawValue(metric.opcode, hex) { problem = it } ?: return MetricDecoded.Rejected(problem)
        return checked(metric, fromRaw(metric, raw))
    }

    /** Raw packet integer to rpm, watts or percent. */
    fun fromRaw(metric: BikeMetric, raw: Int): Float = when (metric) {
        BikeMetric.POWER -> raw / DECIWATTS_PER_WATT
        else -> raw.toFloat()
    }

    /**
     * The repeating dialect's pre-parsed `data` float, used only when there is no raw packet beside
     * it or that packet answers a different command (for resistance it may well be a raw `4A`
     * count). Grupetto's original reader takes cadence and resistance as they come and divides
     * power by 100. The fork's notes suggest a different power scale; no published capture settles
     * it, so the diagnostics screen logs both forms for the owner to compare.
     */
    fun fromRepeatingValue(metric: BikeMetric, data: Float): MetricDecoded = checked(
        metric,
        if (metric == BikeMetric.POWER) data / AffernetWire.CENTIWATTS_PER_WATT else data,
    )

    /** Range check for a value already in rpm, watts or percent. */
    fun checked(metric: BikeMetric, value: Float): MetricDecoded {
        val problem = when (metric) {
            BikeMetric.CADENCE -> BikeLimits.cadenceProblem(value)
            BikeMetric.POWER -> BikeLimits.powerProblem(value)
            BikeMetric.RESISTANCE -> BikeLimits.resistanceProblem(value)
        }
        return if (problem != null) MetricDecoded.Rejected(problem) else MetricDecoded.Ok(value)
    }
}

sealed interface MetricDecoded {
    data class Ok(val value: Float) : MetricDecoded
    data class Rejected(val reason: String) : MetricDecoded
}

/**
 * The bike's resistance calibration: 31 raw sensor counts, entry i being the count at
 * i * 100 / 30 percent. A raw reading is turned into a percentage by linear interpolation between
 * the two entries around it. The method is the one the PeloMon project documents
 * (https://ihaque.org/tag/pelomon.html); this code is written from that description. Readings
 * beyond either end of the table are clamped to 0 or 100. Not thread-safe.
 */
class ResistanceTable {
    private val counts = IntArray(SIZE)
    private var filled = 0

    val isComplete: Boolean get() = filled == SIZE

    /** A usable table rises strictly from entry to entry. */
    val isValid: Boolean
        get() {
            if (!isComplete) return false
            for (i in 1 until SIZE) if (counts[i] <= counts[i - 1]) return false
            return true
        }

    /** Entries must be supplied in order, starting at 0. Returns false if [index] is not the next one. */
    fun put(index: Int, count: Int): Boolean {
        if (index != filled || index >= SIZE) return false
        counts[index] = count
        filled++
        return true
    }

    fun clear() {
        filled = 0
    }

    /** The percentage for raw count [raw], or null while the table is incomplete or unusable. */
    fun percent(raw: Int): Float? {
        if (!isValid) return null
        if (raw <= counts[0]) return 0f
        if (raw >= counts[SIZE - 1]) return 100f
        var lower = 0
        while (lower < SIZE - 2 && raw > counts[lower + 1]) lower++
        val within = (raw - counts[lower]).toFloat() / (counts[lower + 1] - counts[lower])
        return (lower + within) * STEP
    }

    companion object {
        const val SIZE = 31
        const val STEP = 100f / (SIZE - 1)
    }
}

/**
 * Route B delivers cadence, power and resistance one at a time. This joins them into
 * [BikeReading]s: nothing until cadence and power have each been seen, then one reading per
 * cadence value, carrying the latest power and resistance. Resistance is not required, because an
 * older sensor board may not answer the normalised read and the fallback takes a while to load
 * its table; until a resistance arrives it is reported as 0. A value older than [maxAgeNanos] no
 * longer counts as seen. Not thread-safe; the caller serialises access.
 */
class MetricAssembler(private val maxAgeNanos: Long = 5_000_000_000L) {
    private val values = FloatArray(BikeMetric.entries.size)
    private val seenAt = LongArray(BikeMetric.entries.size)
    private val seen = BooleanArray(BikeMetric.entries.size)

    fun accept(metric: BikeMetric, value: Float, nowNanos: Long): BikeReading? {
        values[metric.ordinal] = value
        seenAt[metric.ordinal] = nowNanos
        seen[metric.ordinal] = true
        if (metric != BikeMetric.CADENCE || !fresh(BikeMetric.POWER, nowNanos)) return null
        return BikeReading(
            cadenceRpm = value,
            powerWatts = values[BikeMetric.POWER.ordinal],
            resistancePercent = if (fresh(BikeMetric.RESISTANCE, nowNanos)) {
                minOf(values[BikeMetric.RESISTANCE.ordinal], BikeLimits.MAX_RESISTANCE)
            } else {
                0f
            },
            timestampNanos = nowNanos,
        )
    }

    private fun fresh(metric: BikeMetric, nowNanos: Long) =
        seen[metric.ordinal] && nowNanos - seenAt[metric.ordinal] <= maxAgeNanos

    fun reset() {
        seen.fill(false)
    }
}
