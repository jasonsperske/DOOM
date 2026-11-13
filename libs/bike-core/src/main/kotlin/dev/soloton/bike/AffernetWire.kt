// SPDX-License-Identifier: GPL-2.0-or-later
package dev.soloton.bike

/**
 * Sequential reads from a Binder parcel, so the frame layout below can be exercised on the JVM.
 * On Android it is backed by `android.os.Parcel`.
 */
interface WireReader {
    /** Bytes left to read. Parcel reads past the end return 0 instead of failing, so check first. */
    val bytesAvailable: Int
    fun readInt(): Int
    fun readLong(): Long
}

/**
 * Route A: the Affernet service's `IV1Interface`, a hand-written reading of a Binder interface
 * whose AIDL was never published. Names, transaction codes and field order are from the public
 * protocol references listed in docs/BIKE_SENSORS.md.
 *
 * The client registers a callback Binder; the service then calls it with a `BikeData` parcelable
 * about once a second. Only the front of that parcelable is read here:
 *
 * ```
 * int   hasData               0 means a null BikeData follows (nothing more to read)
 * long  rpm                   crank cadence, already in rpm
 * long  power                 centiwatts (watts x 100)
 * long  stepper motor position   unused, read to stay aligned
 * long  load cell reading        unused, read to stay aligned
 * int   current resistance    already 0 to 100
 * ...   about fifty more fields, ignored
 * ```
 */
object AffernetWire {
    const val PACKAGE = "com.onepeloton.affernetservice"

    /** Both the bind Intent action and the Binder interface descriptor. */
    const val INTERFACE = "com.onepeloton.affernetservice.IV1Interface"
    const val CALLBACK_INTERFACE = "com.onepeloton.affernetservice.IV1Callback"
    const val PERMISSION = "onepeloton.permission.ACCESS_SENSOR_SERVICE"

    /** `registerCallback(IV1Callback, String clientId)`. */
    const val TRANSACTION_REGISTER = 1

    /** `unregisterCallback(IV1Callback, String clientId)`. */
    const val TRANSACTION_UNREGISTER = 2
    // Codes 3 and up (fake data, report rate, calibration) are never sent by this library.

    /** `onSensorDataChange(BikeData)`, one-way. */
    const val CALLBACK_SENSOR_DATA = 1

    /** `onSensorError(long)`, one-way. */
    const val CALLBACK_SENSOR_ERROR = 2

    /** `onCalibrationStatus(int, boolean, long)`, one-way. Ignored. */
    const val CALLBACK_CALIBRATION_STATUS = 3

    const val CENTIWATTS_PER_WATT = 100f

    private const val FLAG_BYTES = 4
    private const val HEAD_BYTES = 8 * 4 + 4

    /**
     * Decodes the arguments of `onSensorDataChange`. Call it after the interface token has been
     * checked. Never throws for short or nonsense input.
     */
    fun decodeSensorData(reader: WireReader, timestampNanos: Long): Decoded {
        if (reader.bytesAvailable < FLAG_BYTES) return Decoded.Rejected("empty sensor frame")
        if (reader.readInt() == 0) return Decoded.Empty
        if (reader.bytesAvailable < HEAD_BYTES) {
            return Decoded.Rejected("sensor frame too short: ${reader.bytesAvailable} bytes after the flag")
        }
        val rpm = reader.readLong()
        val centiwatts = reader.readLong()
        reader.readLong()
        reader.readLong()
        val resistance = reader.readInt()
        return toReading(rpm, centiwatts, resistance, timestampNanos)
    }

    /** Unit conversion and validation for the three raw fields. */
    fun toReading(rpm: Long, powerCentiwatts: Long, resistance: Int, timestampNanos: Long): Decoded {
        // Range-check the integers before converting: a misaligned parcel yields huge longs that
        // would otherwise lose precision as floats.
        if (rpm < 0 || rpm > BikeLimits.MAX_CADENCE_RPM.toLong()) return Decoded.Rejected("cadence out of range: $rpm")
        if (powerCentiwatts < 0 || powerCentiwatts > (BikeLimits.MAX_POWER_WATTS * CENTIWATTS_PER_WATT).toLong()) {
            return Decoded.Rejected("power out of range: $powerCentiwatts centiwatts")
        }
        return BikeLimits.reading(
            cadenceRpm = rpm.toFloat(),
            powerWatts = powerCentiwatts / CENTIWATTS_PER_WATT,
            resistancePercent = resistance.toFloat(),
            timestampNanos = timestampNanos,
        )
    }
}
