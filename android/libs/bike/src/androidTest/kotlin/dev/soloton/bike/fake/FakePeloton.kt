// SPDX-License-Identifier: GPL-2.0-or-later
package dev.soloton.bike.fake

import android.app.Service
import android.content.Context
import android.content.Intent
import android.os.Binder
import android.os.Bundle
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.os.Message
import android.os.Messenger
import android.os.Parcel
import android.os.Process
import android.os.RemoteException
import java.io.File
import java.util.Properties

/*
 * Stand-ins for the Peloton sensor services, for instrumented tests only.
 *
 * They are written from the same public protocol references as the client (docs/BIKE_SENSORS.md)
 * but share no code or constants with it: every name, code and layout below is spelled out again,
 * so a slip on either side shows up as a failing test. What they cannot show is whether the
 * references themselves match a real bike.
 *
 * The services run in a separate process. A test steers them by writing FakeScript to a file in
 * the app's data directory, which both processes can see, and reads FakeStatus back the same way.
 */

/** What the fakes should do. Written by the test process, re-read by the fakes on every tick. */
data class FakeScript(
    /** Changing this resets the fakes' counters. */
    val epoch: Long = System.nanoTime(),
    val rpm: Int = 72,
    /** Power in tenths of a watt: route A multiplies by 10 (centiwatts), route B sends it as is. */
    val powerTenths: Int = 1234,
    val resistance: Int = 37,
    val periodMillis: Long = 100,
    /** normal, silent, short, negative, wrongtoken, null, mixed */
    val affernetMode: String = "normal",
    /** Whether the legacy service understands message 12 (polled reads). */
    val polling: Boolean = true,
    /** Whether it understands messages 1 to 3 (repeating commands). */
    val repeating: Boolean = true,
    /** normal, badchecksum, timeout, garbage, nohex */
    val legacyMode: String = "normal",
    /** Whether the board answers command 49. If not, only raw 4A and the table work. */
    val normalisedResistance: Boolean = true,
) {
    fun write(context: Context) {
        val p = Properties()
        p["epoch"] = epoch.toString()
        p["rpm"] = rpm.toString()
        p["powerTenths"] = powerTenths.toString()
        p["resistance"] = resistance.toString()
        p["periodMillis"] = periodMillis.toString()
        p["affernetMode"] = affernetMode
        p["polling"] = polling.toString()
        p["repeating"] = repeating.toString()
        p["legacyMode"] = legacyMode
        p["normalisedResistance"] = normalisedResistance.toString()
        store(file(context), p)
    }

    companion object {
        private fun file(context: Context) = File(context.filesDir, "fake-peloton-script.properties")

        fun read(context: Context): FakeScript {
            val p = load(file(context)) ?: return FakeScript(epoch = 0)
            val d = FakeScript(epoch = 0)
            return FakeScript(
                epoch = p.getProperty("epoch")?.toLongOrNull() ?: 0,
                rpm = p.getProperty("rpm")?.toIntOrNull() ?: d.rpm,
                powerTenths = p.getProperty("powerTenths")?.toIntOrNull() ?: d.powerTenths,
                resistance = p.getProperty("resistance")?.toIntOrNull() ?: d.resistance,
                periodMillis = p.getProperty("periodMillis")?.toLongOrNull() ?: d.periodMillis,
                affernetMode = p.getProperty("affernetMode") ?: d.affernetMode,
                polling = p.getProperty("polling")?.toBoolean() ?: d.polling,
                repeating = p.getProperty("repeating")?.toBoolean() ?: d.repeating,
                legacyMode = p.getProperty("legacyMode") ?: d.legacyMode,
                normalisedResistance = p.getProperty("normalisedResistance")?.toBoolean() ?: d.normalisedResistance,
            )
        }

        /** Ask the fake process to kill itself on its next tick. */
        fun requestKill(context: Context) {
            killFile(context).writeText("die")
        }

        internal fun killFile(context: Context) = File(context.filesDir, "fake-peloton-kill")
    }
}

/** What the fakes have seen. Written by the fake process. */
data class FakeStatus(
    val epoch: Long = 0,
    val pid: Int = 0,
    val registerCalls: Int = 0,
    val unregisterCalls: Int = 0,
    val callbacks: Int = 0,
    val clientId: String = "",
    val framesSent: Int = 0,
    val pollRequests: Int = 0,
    val repeatingCommands: Int = 0,
    val badRequests: Int = 0,
) {
    companion object {
        private fun file(context: Context) = File(context.filesDir, "fake-peloton-status.properties")

        fun read(context: Context): FakeStatus {
            val p = load(file(context)) ?: return FakeStatus()
            fun int(key: String) = p.getProperty(key)?.toIntOrNull() ?: 0
            return FakeStatus(
                epoch = p.getProperty("epoch")?.toLongOrNull() ?: 0,
                pid = int("pid"),
                registerCalls = int("registerCalls"),
                unregisterCalls = int("unregisterCalls"),
                callbacks = int("callbacks"),
                clientId = p.getProperty("clientId") ?: "",
                framesSent = int("framesSent"),
                pollRequests = int("pollRequests"),
                repeatingCommands = int("repeatingCommands"),
                badRequests = int("badRequests"),
            )
        }

        fun clear(context: Context) {
            file(context).delete()
        }

        internal fun write(context: Context, status: FakeStatus) {
            val p = Properties()
            p["epoch"] = status.epoch.toString()
            p["pid"] = status.pid.toString()
            p["registerCalls"] = status.registerCalls.toString()
            p["unregisterCalls"] = status.unregisterCalls.toString()
            p["callbacks"] = status.callbacks.toString()
            p["clientId"] = status.clientId
            p["framesSent"] = status.framesSent.toString()
            p["pollRequests"] = status.pollRequests.toString()
            p["repeatingCommands"] = status.repeatingCommands.toString()
            p["badRequests"] = status.badRequests.toString()
            store(file(context), p)
        }
    }
}

private fun load(file: File): Properties? = try {
    if (!file.exists()) null else file.inputStream().use { Properties().apply { load(it) } }
} catch (_: java.io.IOException) {
    null
}

/** Write then rename, so the other process never reads half a file. */
private fun store(file: File, properties: Properties) {
    val temp = File(file.parentFile, file.name + "." + Process.myPid() + ".tmp")
    temp.outputStream().use { properties.store(it, null) }
    temp.renameTo(file)
}

/** Counters shared by the fake services, which live in one process. Main thread and Binder threads. */
private object FakeBoard {
    private var status = FakeStatus()
    private var lastWritten: FakeStatus? = null

    @Synchronized
    fun script(context: Context): FakeScript {
        val file = FakeScript.killFile(context)
        if (file.exists()) {
            file.delete()
            Process.killProcess(Process.myPid())
        }
        val script = FakeScript.read(context)
        if (script.epoch != status.epoch) status = FakeStatus(epoch = script.epoch)
        update(context) { it }
        return script
    }

    @Synchronized
    fun update(context: Context, change: (FakeStatus) -> FakeStatus) {
        status = change(status).copy(pid = Process.myPid())
        if (status != lastWritten) {
            FakeStatus.write(context, status)
            lastWritten = status
        }
    }
}

/** Route A stand-in: `IV1Interface` with callback registration and one-way `onSensorDataChange` pushes. */
class FakeAffernetService : Service() {
    private val callbacks = LinkedHashMap<IBinder, String>()
    private val handler = Handler(Looper.getMainLooper())
    private var frame = 0
    private val ticker = object : Runnable {
        override fun run() {
            val script = FakeBoard.script(this@FakeAffernetService)
            push(script)
            handler.postDelayed(this, script.periodMillis)
        }
    }

    private val stub = object : Binder() {
        init {
            attachInterface(null, INTERFACE)
        }

        override fun onTransact(code: Int, data: Parcel, reply: Parcel?, flags: Int): Boolean {
            if (code != 1 && code != 2) return super.onTransact(code, data, reply, flags)
            data.enforceInterface(INTERFACE)
            val callback = data.readStrongBinder()
            val clientId = data.readString() ?: ""
            val count = synchronized(callbacks) {
                if (code == 1) callbacks[callback] = clientId else callbacks.remove(callback)
                callbacks.size
            }
            FakeBoard.update(this@FakeAffernetService) {
                if (code == 1) {
                    it.copy(registerCalls = it.registerCalls + 1, callbacks = count, clientId = clientId)
                } else {
                    it.copy(unregisterCalls = it.unregisterCalls + 1, callbacks = count)
                }
            }
            reply?.writeNoException()
            return true
        }
    }

    override fun onBind(intent: Intent?): IBinder {
        FakeBoard.script(this)
        handler.removeCallbacks(ticker)
        handler.post(ticker)
        return stub
    }

    override fun onDestroy() {
        handler.removeCallbacks(ticker)
        super.onDestroy()
    }

    private fun push(script: FakeScript) {
        val targets = synchronized(callbacks) { callbacks.keys.toList() }
        if (targets.isEmpty() || script.affernetMode == "silent") return
        frame++
        val mode = if (script.affernetMode == "mixed") MIXED[frame % MIXED.size] else script.affernetMode
        for (target in targets) {
            val data = Parcel.obtain()
            try {
                data.writeInterfaceToken(if (mode == "wrongtoken") "com.example.SomethingElse" else CALLBACK)
                when (mode) {
                    "null" -> data.writeInt(0)
                    "short" -> {
                        data.writeInt(1)
                        data.writeLong(script.rpm.toLong())
                    }
                    else -> {
                        data.writeInt(1)
                        writeBikeData(data, script, rpm = if (mode == "negative") -1L else script.rpm.toLong())
                    }
                }
                target.transact(1, data, null, IBinder.FLAG_ONEWAY)
                FakeBoard.update(this) { it.copy(framesSent = it.framesSent + 1) }
            } catch (_: RemoteException) {
                val count = synchronized(callbacks) {
                    callbacks.remove(target)
                    callbacks.size
                }
                FakeBoard.update(this) { it.copy(callbacks = count) }
            } finally {
                data.recycle()
            }
        }
    }

    /**
     * The front of the BikeData parcelable in the documented order, followed by a few of the
     * fields that come after it (a string, a byte array, a string, some ints), which the client
     * must leave unread without harm.
     */
    private fun writeBikeData(data: Parcel, script: FakeScript, rpm: Long) {
        data.writeLong(rpm)
        data.writeLong(script.powerTenths * 10L) // centiwatts
        data.writeLong(4_321L) // stepper motor position
        data.writeLong(87_654L) // load cell reading
        data.writeInt(script.resistance) // current resistance
        data.writeInt(script.resistance + 5) // target resistance
        data.writeString("fake-firmware-1.0")
        data.writeByteArray(byteArrayOf(0xF1.toByte(), 0x41, 0x03, 0x30, 0x30, 0x30))
        data.writeString("12:00:00")
        repeat(6) { data.writeInt(it) }
    }

    private companion object {
        const val INTERFACE = "com.onepeloton.affernetservice.IV1Interface"
        const val CALLBACK = "com.onepeloton.affernetservice.IV1Callback"
        val MIXED = arrayOf("short", "negative", "wrongtoken", "null", "normal")
    }
}

/** Route B stand-in: a Messenger that answers polled raw reads (12/14) and repeating commands (1 to 3). */
open class FakeSensorDataService : Service() {
    private val handler = Handler(Looper.getMainLooper())
    private val subscribers = arrayOfNulls<Messenger>(4)
    private val messenger = Messenger(Handler(Looper.getMainLooper()) { handle(it); true })
    private val ticker = object : Runnable {
        override fun run() {
            val script = FakeBoard.script(this@FakeSensorDataService)
            for (what in 1..3) subscribers[what]?.let { sendRepeating(it, what, script) }
            handler.postDelayed(this, script.periodMillis)
        }
    }

    override fun onBind(intent: Intent?): IBinder {
        FakeBoard.script(this)
        handler.removeCallbacks(ticker)
        handler.post(ticker)
        return messenger.binder
    }

    override fun onDestroy() {
        handler.removeCallbacks(ticker)
        super.onDestroy()
    }

    private fun handle(message: Message) {
        val script = FakeBoard.script(this)
        val replyTo = message.replyTo ?: return
        when (message.what) {
            12 -> if (script.polling) answerPoll(message.data, replyTo, script)
            1, 2, 3 -> if (script.repeating) {
                subscribers[message.what] = replyTo
                FakeBoard.update(this) { it.copy(repeatingCommands = it.repeatingCommands + 1) }
            }
        }
    }

    private fun answerPoll(request: Bundle, replyTo: Messenger, script: FakeScript) {
        FakeBoard.update(this) { it.copy(pollRequests = it.pollRequests + 1) }
        val command = request.getString("command") ?: ""
        val bytes = command.split(",").map { it.toIntOrNull(16) ?: -1 }
        val wellFormed = bytes.size == 4 && bytes[3] == 0xF6 && bytes[2] == (bytes[0] + bytes[1]) and 0xFF
        val hex: String? = when {
            !wellFormed -> {
                FakeBoard.update(this) { it.copy(badRequests = it.badRequests + 1) }
                "TIME_OUT"
            }
            script.legacyMode == "timeout" -> "TIME_OUT"
            script.legacyMode == "garbage" -> "ZZ not a packet"
            bytes[0] == 0xF7 && bytes[1] in 0..30 -> packet(0xF7, TABLE_BASE + TABLE_STEP * bytes[1], 4, script)
            bytes[0] != 0xF5 -> "TIME_OUT"
            bytes[1] == 0x41 -> packet(0x41, script.rpm, 3, script)
            bytes[1] == 0x44 -> packet(0x44, script.powerTenths, 5, script)
            bytes[1] == 0x49 -> if (script.normalisedResistance) packet(0x49, script.resistance, 3, script) else "TIME_OUT"
            bytes[1] == 0x4A -> packet(0x4A, rawCount(script.resistance), 4, script)
            else -> "TIME_OUT"
        }
        val reply = Message.obtain(null, 14)
        reply.data = Bundle().apply {
            putString("requestId", request.getString("requestId"))
            putString("responseHexString", hex)
        }
        send(replyTo, reply)
    }

    private fun sendRepeating(to: Messenger, what: Int, script: FakeScript) {
        val message = Message.obtain(null, what)
        message.data = Bundle().apply {
            putLong("time", System.currentTimeMillis())
            when (what) {
                1 -> {
                    putFloat("data", script.rpm.toFloat())
                    putString("responseHexString", packet(0x41, script.rpm, 3, script))
                }
                2 -> {
                    // The scale Grupetto's original reader assumes: it divides by 100 to get watts.
                    putFloat("data", script.powerTenths * 10f)
                    putString("responseHexString", packet(0x44, script.powerTenths, 5, script))
                }
                else -> {
                    // The tablet reads the raw count and normalises it itself.
                    putFloat("data", script.resistance.toFloat())
                    putString("responseHexString", packet(0x4A, rawCount(script.resistance), 4, script))
                }
            }
            when (script.legacyMode) {
                "nohex" -> remove("responseHexString")
                "timeout" -> putString("responseHexString", "TIME_OUT")
                "garbage" -> {
                    putString("responseHexString", "ZZ not a packet")
                    putFloat("data", Float.NaN)
                }
            }
        }
        if (!send(to, message)) subscribers[what] = null
    }

    private fun send(to: Messenger, message: Message): Boolean = try {
        to.send(message)
        true
    } catch (_: RemoteException) {
        false
    }

    /** `F1 <op> <n> <digits, least significant first> <sum> F6`, space separated. */
    private fun packet(opcode: Int, value: Int, digits: Int, script: FakeScript): String {
        val bytes = ArrayList<Int>()
        bytes += 0xF1
        bytes += opcode
        bytes += digits
        var rest = value
        repeat(digits) {
            bytes += 0x30 + rest % 10
            rest /= 10
        }
        var sum = bytes.sum() and 0xFF
        if (script.legacyMode == "badchecksum") sum = sum xor 0x55
        bytes += sum
        bytes += 0xF6
        return bytes.joinToString(" ") { "%02X".format(it) }
    }

    private companion object {
        // A made-up calibration: 200 counts at 0 %, rising 25 per table entry to 950 at 100 %.
        const val TABLE_BASE = 200
        const val TABLE_STEP = 25
        fun rawCount(percent: Int): Int = TABLE_BASE + Math.round(percent * 7.5f)
    }
}

class FakeOldSensorDataService : FakeSensorDataService()

class ImposterService : Service() {
    private val binder = object : Binder() {
        init {
            attachInterface(null, "com.example.NotTheSensorService")
        }
    }

    override fun onBind(intent: Intent?): IBinder = binder
}
