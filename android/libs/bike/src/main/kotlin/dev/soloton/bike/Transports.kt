// SPDX-License-Identifier: GPL-2.0-or-later
package dev.soloton.bike

import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.ServiceConnection
import android.content.pm.ServiceInfo
import android.os.Binder
import android.os.Bundle
import android.os.Handler
import android.os.IBinder
import android.os.Message
import android.os.Messenger
import android.os.Parcel
import android.os.RemoteException
import java.util.UUID

/** What a transport needs from, and reports to, [BikeSensors]. */
internal interface TransportHost {
    val context: Context

    /** The worker thread's handler. Transports do all their work on it. */
    val handler: Handler
    val endpoints: BikeEndpoints
    val timing: LinkTiming

    /** The service is bound and speaks the expected interface. */
    fun bound(transport: Transport)

    /** A valid sample. May be called from a Binder thread. */
    fun reading(transport: Transport, reading: BikeReading)

    /** A message that was dropped. May be called from a Binder thread. */
    fun rejected(transport: Transport, reason: String)

    /** The transport cannot continue. */
    fun failed(transport: Transport, message: String)

    /** Something worth showing in the event log. */
    fun note(message: String)
}

/** One attempt at one route. [open] and [close] are called once each, on the worker thread. */
internal abstract class Transport(protected val host: TransportHost) {
    @Volatile
    var closed = false
        protected set

    abstract fun open()
    abstract fun close()

    protected fun resolve(intent: Intent): ServiceInfo? = try {
        host.context.packageManager.resolveService(intent, 0)?.serviceInfo
    } catch (e: RuntimeException) {
        host.note("could not query services: $e")
        null
    }

    protected fun describe(info: ServiceInfo): String =
        "${info.packageName}/${info.name} exported=${info.exported} permission=${info.permission ?: "none"}"
}

/**
 * A bindService call whose results arrive on the worker thread, and which can always be released
 * safely. Android delivers [ServiceConnection] callbacks on the main thread.
 */
internal class ServiceBinding(
    private val context: Context,
    private val handler: Handler,
    private val onBinder: (IBinder) -> Unit,
    private val onLost: (String) -> Unit,
) : ServiceConnection, IBinder.DeathRecipient {
    private var bindCalled = false
    private var linked: IBinder? = null

    @Volatile
    private var released = false

    /** Null when the bind was accepted, otherwise why it was not. */
    fun bind(intent: Intent): String? = try {
        bindCalled = true
        if (context.bindService(intent, this, Context.BIND_AUTO_CREATE)) {
            null
        } else {
            "bindService was refused (service missing, not exported or not permitted)"
        }
    } catch (e: SecurityException) {
        "not permitted to bind: ${e.message}"
    } catch (e: RuntimeException) {
        "bind failed: $e"
    }

    fun release() {
        released = true
        try {
            linked?.unlinkToDeath(this, 0)
        } catch (_: RuntimeException) {
            // Already dead or never linked.
        }
        linked = null
        if (bindCalled) {
            bindCalled = false
            try {
                context.unbindService(this)
            } catch (_: RuntimeException) {
                // Not bound any more.
            }
        }
    }

    override fun onServiceConnected(name: ComponentName?, service: IBinder?) = onWorker {
        if (service == null) {
            onLost("the service returned no binder")
            return@onWorker
        }
        try {
            service.linkToDeath(this, 0)
            linked = service
        } catch (_: RemoteException) {
            onLost("the service died while connecting")
            return@onWorker
        }
        onBinder(service)
    }

    override fun onServiceDisconnected(name: ComponentName?) = lost("the service disconnected (its process stopped)")
    override fun onBindingDied(name: ComponentName?) = lost("the binding died")
    override fun onNullBinding(name: ComponentName?) = lost("the service refused the binding")
    override fun binderDied() = lost("the service's process died")

    private fun lost(why: String) = onWorker { onLost(why) }

    private fun onWorker(block: () -> Unit) {
        handler.post { if (!released) block() }
    }
}

/** [WireReader] over a Binder parcel. */
internal class ParcelReader(private val parcel: Parcel) : WireReader {
    override val bytesAvailable: Int get() = parcel.dataAvail()
    override fun readInt(): Int = parcel.readInt()
    override fun readLong(): Long = parcel.readLong()
}

/** Route A. See [AffernetWire] for the protocol. */
internal class AffernetTransport(host: TransportHost) : Transport(host) {
    private val callback = Callback()
    private var binding: ServiceBinding? = null
    private var remote: IBinder? = null
    private var registered = false

    override fun open() {
        val endpoints = host.endpoints
        val intent = Intent(endpoints.affernetAction).setPackage(endpoints.affernetPackage)
        val info = resolve(intent)
        if (info == null) {
            host.failed(this, "no service in ${endpoints.affernetPackage} answers ${endpoints.affernetAction}")
            return
        }
        host.note("Affernet service found: ${describe(info)}")
        intent.component = ComponentName(info.packageName, info.name)
        val binding = ServiceBinding(host.context, host.handler, ::connected) { host.failed(this, it) }
        this.binding = binding
        binding.bind(intent)?.let { host.failed(this, it) }
    }

    private fun connected(binder: IBinder) {
        try {
            val descriptor = binder.interfaceDescriptor
            if (descriptor != AffernetWire.INTERFACE) {
                // Never send transactions to an interface we do not know.
                host.failed(this, "unexpected Binder interface \"$descriptor\"")
                return
            }
            remote = binder
            host.bound(this)
            callService(binder, AffernetWire.TRANSACTION_REGISTER)
            registered = true
            host.note("registered a callback as \"${host.endpoints.clientId}\"")
        } catch (e: RemoteException) {
            host.failed(this, "the service died during registration: ${e.message}")
        } catch (e: RuntimeException) {
            host.failed(this, "registration failed: $e")
        }
    }

    /** `registerCallback` and `unregisterCallback` take the same arguments. */
    private fun callService(binder: IBinder, transaction: Int) {
        val data = Parcel.obtain()
        val reply = Parcel.obtain()
        try {
            data.writeInterfaceToken(AffernetWire.INTERFACE)
            data.writeStrongBinder(callback)
            data.writeString(host.endpoints.clientId)
            if (!binder.transact(transaction, data, reply, 0)) {
                throw IllegalStateException("the service does not understand transaction $transaction")
            }
            reply.readException()
        } finally {
            data.recycle()
            reply.recycle()
        }
    }

    override fun close() {
        closed = true
        val binder = remote
        if (registered && binder != null) {
            registered = false
            try {
                callService(binder, AffernetWire.TRANSACTION_UNREGISTER)
                host.note("unregistered the callback")
            } catch (e: RemoteException) {
                host.note("could not unregister (service gone): ${e.message}")
            } catch (e: RuntimeException) {
                host.note("could not unregister: $e")
            }
        }
        remote = null
        binding?.release()
        binding = null
    }

    /** The Binder the service calls. Runs on Binder threads; must never throw. */
    private inner class Callback : Binder() {
        init {
            attachInterface(null, AffernetWire.CALLBACK_INTERFACE)
        }

        override fun onTransact(code: Int, data: Parcel, reply: Parcel?, flags: Int): Boolean {
            if (code < FIRST_CALL_TRANSACTION || code > LAST_CALL_TRANSACTION) {
                return super.onTransact(code, data, reply, flags)
            }
            if (closed) return true
            try {
                data.enforceInterface(AffernetWire.CALLBACK_INTERFACE)
                when (code) {
                    AffernetWire.CALLBACK_SENSOR_DATA ->
                        when (val decoded = AffernetWire.decodeSensorData(ParcelReader(data), System.nanoTime())) {
                            is Decoded.Ok -> host.reading(this@AffernetTransport, decoded.reading)
                            is Decoded.Empty -> host.rejected(this@AffernetTransport, "the service sent an empty frame")
                            is Decoded.Rejected -> host.rejected(this@AffernetTransport, decoded.reason)
                        }
                    AffernetWire.CALLBACK_SENSOR_ERROR -> {
                        val error = if (data.dataAvail() >= 8) data.readLong().toString() else "unknown"
                        host.rejected(this@AffernetTransport, "the service reported sensor error $error")
                    }
                    else -> Unit
                }
            } catch (e: RuntimeException) {
                host.rejected(this@AffernetTransport, "malformed callback (code $code): $e")
            }
            return true
        }
    }
}

/** Route B, in either dialect. See [SensorDataWire] for the protocol. */
internal class SensorDataTransport(host: TransportHost, private val repeating: Boolean) : Transport(host) {
    private val handler = host.handler
    private val assembler = MetricAssembler()
    private var binding: ServiceBinding? = null
    private var remote: Messenger? = null
    private val firstReplyLogged = BooleanArray(BikeMetric.entries.size)

    // Polling. Cadence goes last so the first full reading comes at the end of the first round.
    private val pollOrder = arrayOf(BikeMetric.POWER, BikeMetric.RESISTANCE, BikeMetric.CADENCE)
    private val session = UUID.randomUUID().toString()
    private var sequence = 0L
    private var step = 0
    private var pendingId: String? = null
    private var pendingOpcode = 0
    private val pollReplies = Messenger(Handler(handler.looper) { onPollReply(it); true })
    private val pollNext = Runnable { poll() }
    private val pollTimeout = Runnable {
        if (!closed && pendingId != null) {
            pendingId = null
            pollProblem(pendingOpcode, "no answer to a read request")
            poll()
        }
    }

    // Resistance: the normalised read first; if the board never answers it, the raw count plus
    // the bike's own calibration table, which is how the tablet software itself does it.
    private var normalisedWorked = false
    private var normalisedFailures = 0
    private var useRawResistance = false
    private var resistanceGivenUp = false
    private val table = ResistanceTable()
    private var tableNext = 0

    // Repeating commands: one reply channel per value, as nothing in a reply names its command.
    private val repeatingReplies = ArrayList<Messenger>()
    private val lastReplyTime = LongArray(BikeMetric.entries.size)

    override fun open() {
        val endpoints = host.endpoints
        var intent = Intent(endpoints.sensorDataAction)
            .addCategory(endpoints.sensorDataCategory)
            .setPackage(endpoints.sensorDataPackage)
        var info = resolve(intent)
        if (info == null) {
            intent = Intent(endpoints.sensorDataAction).setPackage(endpoints.sensorDataPackage)
            info = resolve(intent)
            if (info != null) host.note("SensorData does not advertise the bike category; binding without it")
        }
        if (info == null) {
            host.failed(this, "no service in ${endpoints.sensorDataPackage} answers ${endpoints.sensorDataAction}")
            return
        }
        host.note("SensorData service found: ${describe(info)}")
        intent.component = ComponentName(info.packageName, info.name)
        val binding = ServiceBinding(host.context, handler, ::connected) { host.failed(this, it) }
        this.binding = binding
        binding.bind(intent)?.let { host.failed(this, it) }
    }

    private fun connected(binder: IBinder) {
        try {
            val descriptor = binder.interfaceDescriptor
            if (descriptor != SensorDataWire.MESSENGER_INTERFACE) {
                host.failed(this, "unexpected Binder interface \"$descriptor\"")
                return
            }
            remote = Messenger(binder)
            host.bound(this)
            if (repeating) startRepeating() else poll()
        } catch (e: RemoteException) {
            host.failed(this, "the service died while connecting: ${e.message}")
        } catch (e: RuntimeException) {
            host.failed(this, "could not start reading: $e")
        }
    }

    override fun close() {
        closed = true
        handler.removeCallbacks(pollNext)
        handler.removeCallbacks(pollTimeout)
        remote = null
        binding?.release()
        binding = null
    }

    private fun poll() {
        val service = remote
        if (closed || service == null) return
        var metric = pollOrder[step]
        step = (step + 1) % pollOrder.size
        if (metric == BikeMetric.RESISTANCE && resistanceGivenUp) {
            metric = pollOrder[step]
            step = (step + 1) % pollOrder.size
        }
        val opcode: Int
        val command: String
        when {
            metric != BikeMetric.RESISTANCE || !useRawResistance -> {
                opcode = metric.opcode
                command = SensorDataWire.readPacket(metric)
            }
            !table.isComplete -> {
                opcode = SensorDataWire.OP_TABLE_ENTRY
                command = SensorDataWire.tablePacket(tableNext)
            }
            else -> {
                opcode = SensorDataWire.OP_RAW_RESISTANCE
                command = SensorDataWire.readPacket(opcode)
            }
        }
        val id = "$session:${sequence++}"
        val message = Message.obtain(null, SensorDataWire.MESSAGE_READ_REQUEST)
        message.data = Bundle().apply {
            putString(SensorDataWire.KEY_COMMAND, command)
            putString(SensorDataWire.KEY_REQUEST_ID, id)
        }
        message.replyTo = pollReplies
        try {
            service.send(message)
        } catch (e: RemoteException) {
            host.failed(this, "the service died: ${e.message}")
            return
        } catch (e: RuntimeException) {
            host.failed(this, "could not send a read request: $e")
            return
        }
        pendingId = id
        pendingOpcode = opcode
        handler.postDelayed(pollTimeout, host.timing.pollTimeoutNanos / NANOS_PER_MILLI)
    }

    private fun onPollReply(message: Message) {
        if (closed || message.what != SensorDataWire.MESSAGE_READ_RESPONSE) return
        val id: String?
        val hex: String?
        try {
            val data = message.peekData()
            id = data?.getString(SensorDataWire.KEY_REQUEST_ID)
            hex = data?.getString(SensorDataWire.KEY_RESPONSE_HEX)
        } catch (e: RuntimeException) {
            host.rejected(this, "unreadable reply: $e")
            return
        }
        // A late answer to a request that already timed out must not become a new sample.
        if (id == null || id != pendingId) return
        pendingId = null
        handler.removeCallbacks(pollTimeout)
        val opcode = pendingOpcode
        val raw = SensorDataWire.rawValue(opcode, hex) { pollProblem(opcode, it) }
        if (raw != null) onPollValue(opcode, raw, hex)
        handler.postDelayed(pollNext, host.timing.pollGapNanos / NANOS_PER_MILLI)
    }

    private fun onPollValue(opcode: Int, raw: Int, hex: String?) {
        when (opcode) {
            SensorDataWire.OP_TABLE_ENTRY -> {
                table.put(tableNext, raw)
                tableNext++
                if (table.isComplete) {
                    if (table.isValid) {
                        host.note("read the bike's ${ResistanceTable.SIZE}-entry resistance table")
                    } else {
                        resistanceGivenUp = true
                        host.note("the bike's resistance table does not rise steadily; resistance will read 0")
                    }
                }
            }
            SensorDataWire.OP_RAW_RESISTANCE -> {
                logFirstReply(BikeMetric.RESISTANCE, "raw count, hex=\"$hex\"")
                val percent = table.percent(raw) ?: return
                accept(BikeMetric.RESISTANCE, SensorDataWire.checked(BikeMetric.RESISTANCE, percent))
            }
            else -> {
                val metric = BikeMetric.entries.first { it.opcode == opcode }
                logFirstReply(metric, "hex=\"$hex\"")
                if (metric == BikeMetric.RESISTANCE) normalisedWorked = true
                accept(metric, SensorDataWire.checked(metric, SensorDataWire.fromRaw(metric, raw)))
            }
        }
    }

    /** A read that timed out or came back invalid. */
    private fun pollProblem(opcode: Int, reason: String) {
        host.rejected(this, "read %02X: %s".format(opcode, reason))
        if (opcode == BikeMetric.RESISTANCE.opcode && !normalisedWorked && ++normalisedFailures >= RAW_FALLBACK_AFTER) {
            useRawResistance = true
            host.note(
                "the board does not answer the normalised resistance read (49); " +
                    "using the raw count (4A) and the bike's calibration table",
            )
        }
    }

    private fun startRepeating() {
        val service = remote ?: return
        for (metric in BikeMetric.entries) {
            val replies = Messenger(Handler(handler.looper) { onRepeatingReply(metric, it); true })
            repeatingReplies += replies
            val message = Message.obtain(null, metric.repeatingCommand)
            message.data = Bundle().apply { putString(SensorDataWire.KEY_REQUEST_ID, UUID.randomUUID().toString()) }
            message.replyTo = replies
            service.send(message)
        }
        host.note("sent repeating commands 1, 2 and 3")
    }

    private fun onRepeatingReply(metric: BikeMetric, message: Message) {
        if (closed) return
        if (message.what < 0) {
            host.failed(this, "the service refused the ${metric.name.lowercase()} command (reply ${message.what})")
            return
        }
        val name = metric.name.lowercase()
        try {
            val data = message.peekData()
            if (data == null) {
                host.rejected(this, "$name: reply without data")
                return
            }
            val hex = data.getString(SensorDataWire.KEY_RESPONSE_HEX)
            val hasValue = data.containsKey(SensorDataWire.KEY_VALUE)
            val value = data.getFloat(SensorDataWire.KEY_VALUE)
            logFirstReply(metric, "hex=\"$hex\" data=${if (hasValue) value.toString() else "absent"}")
            if (hex?.trim() == SensorDataWire.TIMED_OUT) {
                host.rejected(this, "$name: the service reported TIME_OUT")
                return
            }
            val time = data.getLong(SensorDataWire.KEY_TIME)
            if (time < lastReplyTime[metric.ordinal]) {
                host.rejected(this, "$name: reply older than the previous one")
                return
            }
            lastReplyTime[metric.ordinal] = time
            // Prefer the raw packet. The service's parsed value is used only when there is no
            // packet, or the packet is sound but answers another command (resistance arrives as
            // a raw 4A count). A damaged packet is dropped: the parsed value came from it.
            val parsed = if (hex == null) null else SensorDataWire.parseResponse(hex)
            val decoded = when {
                parsed is SensorDataWire.Parsed.Invalid -> MetricDecoded.Rejected(parsed.reason)
                parsed is SensorDataWire.Parsed.Valid && parsed.packet.opcode == metric.opcode ->
                    SensorDataWire.checked(metric, SensorDataWire.fromRaw(metric, parsed.packet.value))
                hasValue -> SensorDataWire.fromRepeatingValue(metric, value)
                else -> MetricDecoded.Rejected("no usable value")
            }
            accept(metric, decoded)
        } catch (e: RuntimeException) {
            host.rejected(this, "$name: unreadable reply: $e")
        }
    }

    private fun accept(metric: BikeMetric, decoded: MetricDecoded) {
        when (decoded) {
            is MetricDecoded.Rejected -> host.rejected(this, "${metric.name.lowercase()}: ${decoded.reason}")
            is MetricDecoded.Ok -> assembler.accept(metric, decoded.value, System.nanoTime())?.let {
                host.reading(this, it)
            }
        }
    }

    /** The first raw reply for each value goes in the event log: it is what settles units on a real bike. */
    private fun logFirstReply(metric: BikeMetric, text: String) {
        if (firstReplyLogged[metric.ordinal]) return
        firstReplyLogged[metric.ordinal] = true
        host.note("first ${metric.name.lowercase()} reply: $text")
    }

    private companion object {
        const val NANOS_PER_MILLI = 1_000_000L
        const val RAW_FALLBACK_AFTER = 3
    }
}
