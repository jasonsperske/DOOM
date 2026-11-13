// SPDX-License-Identifier: GPL-2.0-or-later
package dev.soloton.bike

import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertIs
import kotlin.test.assertNull
import kotlin.test.assertTrue

/** A parcel stand-in: a list of ints and longs read back in order. */
private class FakeWire(vararg fields: Number) : WireReader {
    private val queue = ArrayDeque(fields.toList())
    override val bytesAvailable: Int get() = queue.sumOf { if (it is Long) 8 else 4 }
    override fun readInt(): Int = queue.removeFirst() as Int
    override fun readLong(): Long = queue.removeFirst() as Long
}

class AffernetWireTest {
    private fun frame(rpm: Long, centiwatts: Long, resistance: Int) =
        FakeWire(1, rpm, centiwatts, 123_456L, 789L, resistance, 55, 0xBEEF)

    @Test
    fun decodesCadencePowerAndResistanceFromTheFrontOfTheFrame() {
        val decoded = AffernetWire.decodeSensorData(frame(72, 12_345, 37), timestampNanos = 99)
        val reading = assertIs<Decoded.Ok>(decoded).reading
        assertEquals(72f, reading.cadenceRpm)
        assertEquals(123.45f, reading.powerWatts, 0.001f)
        assertEquals(37f, reading.resistancePercent)
        assertEquals(99, reading.timestampNanos)
    }

    @Test
    fun powerIsCentiwatts() {
        // OpenRide's live capture peaked at 63 W; in the parcel that is 6300.
        assertEquals(63f, ok(AffernetWire.toReading(78, 6_300, 32, 0)).powerWatts)
        assertEquals(0.5f, ok(AffernetWire.toReading(10, 50, 0, 0)).powerWatts)
    }

    @Test
    fun aStationaryBikeIsAValidReading() {
        val reading = ok(AffernetWire.toReading(0, 0, 1, 0))
        assertEquals(0f, reading.cadenceRpm)
        assertEquals(1f, reading.resistancePercent)
    }

    @Test
    fun aNullBikeDataIsEmptyNotAReading() {
        assertEquals(Decoded.Empty, AffernetWire.decodeSensorData(FakeWire(0), 0))
    }

    @Test
    fun shortOrEmptyParcelsAreRejectedWithoutReading() {
        assertIs<Decoded.Rejected>(AffernetWire.decodeSensorData(FakeWire(), 0))
        assertIs<Decoded.Rejected>(AffernetWire.decodeSensorData(FakeWire(1), 0))
        assertIs<Decoded.Rejected>(AffernetWire.decodeSensorData(FakeWire(1, 72L, 100L, 0L), 0))
    }

    @Test
    fun impossibleValuesAreRejectedNotClamped() {
        assertIs<Decoded.Rejected>(AffernetWire.toReading(-1, 100, 30, 0))
        assertIs<Decoded.Rejected>(AffernetWire.toReading(251, 100, 30, 0))
        assertIs<Decoded.Rejected>(AffernetWire.toReading(Long.MAX_VALUE, 100, 30, 0))
        assertIs<Decoded.Rejected>(AffernetWire.toReading(80, -1, 30, 0))
        assertIs<Decoded.Rejected>(AffernetWire.toReading(80, 300_001, 30, 0))
        assertIs<Decoded.Rejected>(AffernetWire.toReading(80, 100, -1, 0))
        assertIs<Decoded.Rejected>(AffernetWire.toReading(80, 100, 106, 0))
        assertIs<Decoded.Rejected>(AffernetWire.toReading(80, 100, Int.MIN_VALUE, 0))
    }

    @Test
    fun resistanceJustOver100IsClamped() {
        assertEquals(100f, ok(AffernetWire.toReading(80, 100, 103, 0)).resistancePercent)
        assertEquals(100f, ok(AffernetWire.toReading(80, 100, 100, 0)).resistancePercent)
    }

    @Test
    fun limitsRejectNotANumber() {
        assertIs<Decoded.Rejected>(BikeLimits.reading(Float.NaN, 1f, 1f, 0))
        assertIs<Decoded.Rejected>(BikeLimits.reading(1f, Float.POSITIVE_INFINITY, 1f, 0))
        assertIs<Decoded.Rejected>(BikeLimits.reading(1f, 1f, Float.NaN, 0))
    }

    private fun ok(decoded: Decoded) = assertIs<Decoded.Ok>(decoded).reading
}

class SensorDataWireTest {
    /** Builds a bike response the way the board does: digits least significant first, additive checksum. */
    private fun response(opcode: Int, value: Int, digits: Int, separator: String = " "): String {
        val bytes = ArrayList<Int>()
        bytes += 0xF1
        bytes += opcode
        bytes += digits
        var rest = value
        repeat(digits) {
            bytes += '0'.code + rest % 10
            rest /= 10
        }
        bytes += bytes.sum() and 0xFF
        bytes += 0xF6
        return bytes.joinToString(separator) { "%02X".format(it) }
    }

    @Test
    fun requestPacketsCarryTheirChecksum() {
        assertEquals("F5,41,36,F6", SensorDataWire.readPacket(BikeMetric.CADENCE))
        assertEquals("F5,44,39,F6", SensorDataWire.readPacket(BikeMetric.POWER))
        assertEquals("F5,49,3E,F6", SensorDataWire.readPacket(BikeMetric.RESISTANCE))
        assertEquals("F5,4A,3F,F6", SensorDataWire.readPacket(SensorDataWire.OP_RAW_RESISTANCE))
        assertEquals("F7,00,F7,F6", SensorDataWire.tablePacket(0))
        assertEquals("F7,1E,15,F6", SensorDataWire.tablePacket(30))
    }

    @Test
    fun decodesResponsesCapturedFromARealBike() {
        // Published in the Grupetto fork's notes (docs/pedal-calibration.md), read from a stationary bike.
        assertEquals(MetricDecoded.Ok(0f), SensorDataWire.decode(BikeMetric.CADENCE, "F1 41 03 30 30 30 C5 F6"))
        assertEquals(MetricDecoded.Ok(0f), SensorDataWire.decode(BikeMetric.POWER, "F1 44 05 30 30 30 30 30 2A F6"))
        assertEquals(MetricDecoded.Ok(31f), SensorDataWire.decode(BikeMetric.RESISTANCE, "F1 49 03 31 33 30 D1 F6"))
        assertEquals(556, SensorDataWire.rawValue(SensorDataWire.OP_RAW_RESISTANCE, "F1 4A 04 36 35 35 30 0F F6"))
        assertEquals(173, SensorDataWire.rawValue(SensorDataWire.OP_TABLE_ENTRY, "F1 F7 04 33 37 31 30 B7 F6"))
    }

    @Test
    fun powerIsTenthsOfAWattAndDigitsAreLeastSignificantFirst() {
        assertEquals(MetricDecoded.Ok(123.4f), SensorDataWire.decode(BikeMetric.POWER, response(0x44, 1234, 5)))
        assertEquals(MetricDecoded.Ok(87f), SensorDataWire.decode(BikeMetric.CADENCE, response(0x41, 87, 3)))
        assertEquals(MetricDecoded.Ok(45f), SensorDataWire.decode(BikeMetric.RESISTANCE, response(0x49, 45, 3)))
    }

    @Test
    fun acceptsCommaSeparatedAndLowerCaseHex() {
        val text = response(0x41, 90, 3, separator = ",").lowercase()
        assertEquals(MetricDecoded.Ok(90f), SensorDataWire.decode(BikeMetric.CADENCE, " $text "))
    }

    @Test
    fun rejectsDamagedResponses() {
        val good = response(0x41, 90, 3)
        fun rejected(hex: String?) = assertIs<MetricDecoded.Rejected>(SensorDataWire.decode(BikeMetric.CADENCE, hex))
        rejected(null)
        rejected("")
        rejected("TIME_OUT")
        rejected("hello there")
        rejected(good.replace("F1", "F2"))
        rejected(good.dropLast(2) + "F7")
        rejected(good.replace(" 30 ", " 31 ")) // digit changed, checksum now wrong
        rejected("F1 41 03 3A 30 30 CF F6") // checksum right, but 3A is not a digit
        rejected("F1 41 04 30 30 30 C6 F6") // length byte disagrees with the packet
        rejected("F1 41 03 30 30 30 C5 F6 00")
        rejected("F1 41 03 +0 30 30 C5 F6")
        rejected("F1 ".repeat(60))
        assertTrue(rejected(response(0x44, 90, 3)).reason.contains("expected 41"))
    }

    @Test
    fun rejectsValuesOutOfRange() {
        assertIs<MetricDecoded.Rejected>(SensorDataWire.decode(BikeMetric.CADENCE, response(0x41, 999, 3)))
        assertIs<MetricDecoded.Rejected>(SensorDataWire.decode(BikeMetric.POWER, response(0x44, 99_999, 5)))
        assertIs<MetricDecoded.Rejected>(SensorDataWire.decode(BikeMetric.RESISTANCE, response(0x49, 250, 3)))
    }

    @Test
    fun repeatingDialectValueUsesGrupettosScaling() {
        assertEquals(MetricDecoded.Ok(80f), SensorDataWire.fromRepeatingValue(BikeMetric.CADENCE, 80f))
        assertEquals(MetricDecoded.Ok(150f), SensorDataWire.fromRepeatingValue(BikeMetric.POWER, 15_000f))
        assertEquals(MetricDecoded.Ok(42f), SensorDataWire.fromRepeatingValue(BikeMetric.RESISTANCE, 42f))
        assertIs<MetricDecoded.Rejected>(SensorDataWire.fromRepeatingValue(BikeMetric.CADENCE, Float.NaN))
        assertIs<MetricDecoded.Rejected>(SensorDataWire.fromRepeatingValue(BikeMetric.CADENCE, -3f))
    }
}

class ResistanceTableTest {
    private fun table(counts: List<Int>) = ResistanceTable().apply {
        counts.forEachIndexed { i, count -> assertTrue(put(i, count)) }
    }

    private val linear = List(31) { 100 + it * 30 } // 100 at 0 %, 1000 at 100 %

    @Test
    fun interpolatesBetweenEntries() {
        val t = table(linear)
        assertTrue(t.isValid)
        assertEquals(0f, t.percent(100))
        assertEquals(100f, t.percent(1000))
        assertEquals(50f, t.percent(550)!!, 0.001f)
        assertEquals(100f / 30f, t.percent(130)!!, 0.001f)
        assertEquals(100f / 60f, t.percent(115)!!, 0.001f)
    }

    @Test
    fun followsAnUnevenTable() {
        // Entry 0 at 164, entry 1 at 169, entry 2 at 186: halfway from 1 to 2 is 1.5 steps.
        val counts = ArrayList<Int>().apply {
            add(164)
            add(169)
            add(186)
            for (i in 3..30) add(186 + (i - 2) * 40)
        }
        assertEquals(1.5f * ResistanceTable.STEP, table(counts).percent(177)!!, 0.2f)
    }

    @Test
    fun clampsBeyondTheEnds() {
        val t = table(linear)
        assertEquals(0f, t.percent(5))
        assertEquals(100f, t.percent(5000))
    }

    @Test
    fun incompleteOrNonRisingTablesGiveNothing() {
        assertNull(ResistanceTable().percent(500))
        assertNull(table(linear.take(30)).percent(500))
        val flat = linear.toMutableList().also { it[10] = it[9] }
        val t = table(flat)
        assertTrue(t.isComplete)
        assertFalse(t.isValid)
        assertNull(t.percent(500))
    }

    @Test
    fun entriesMustArriveInOrder() {
        val t = ResistanceTable()
        assertFalse(t.put(1, 10))
        assertTrue(t.put(0, 10))
        assertFalse(t.put(0, 10))
    }
}

class MetricAssemblerTest {
    @Test
    fun emitsOncePowerAndCadenceAreKnownAndThenOnEveryCadence() {
        val a = MetricAssembler()
        assertNull(a.accept(BikeMetric.CADENCE, 80f, 1))
        assertNull(a.accept(BikeMetric.POWER, 150f, 2))
        val first = a.accept(BikeMetric.CADENCE, 81f, 3)!!
        assertEquals(BikeReading(81f, 150f, 0f, 3), first)
        assertNull(a.accept(BikeMetric.RESISTANCE, 40f, 4))
        assertEquals(BikeReading(82f, 150f, 40f, 5), a.accept(BikeMetric.CADENCE, 82f, 5))
    }

    @Test
    fun stalePowerStopsReadingsAndStaleResistanceReadsZero() {
        val a = MetricAssembler(maxAgeNanos = 100)
        a.accept(BikeMetric.POWER, 150f, 0)
        a.accept(BikeMetric.RESISTANCE, 40f, 0)
        assertEquals(40f, a.accept(BikeMetric.CADENCE, 80f, 50)!!.resistancePercent)
        assertNull(a.accept(BikeMetric.CADENCE, 80f, 200))
        a.accept(BikeMetric.POWER, 150f, 210)
        assertEquals(0f, a.accept(BikeMetric.CADENCE, 80f, 220)!!.resistancePercent)
    }

    @Test
    fun resetForgetsEverything() {
        val a = MetricAssembler()
        a.accept(BikeMetric.POWER, 150f, 0)
        a.reset()
        assertNull(a.accept(BikeMetric.CADENCE, 80f, 1))
    }
}

class SpikeFilterTest {
    @Test
    fun dropsASingleSpikeAndFollowsARealChangeLate() {
        val f = SpikeFilter()
        assertEquals(30f, f.filter(30f))
        assertEquals(30f, f.filter(30f))
        assertEquals(30f, f.filter(90f)) // spike
        assertEquals(30f, f.filter(30f))
        assertEquals(30f, f.filter(50f))
        assertEquals(30f, f.filter(50f))
        assertEquals(50f, f.filter(50f)) // three in a row: believed
        assertEquals(20f, f.filter(20f)) // drops show at once
    }

    @Test
    fun resetStartsOver() {
        val f = SpikeFilter()
        f.filter(10f)
        f.reset()
        assertEquals(70f, f.filter(70f))
    }
}
