// SPDX-License-Identifier: GPL-2.0-or-later
package dev.soloton.bike

/** The ways of reaching the bike's sensors, in the order they are tried. */
enum class BikeRoute(val label: String) {
    /** Route A: callbacks from the Affernet service's `IV1Interface`. */
    AFFERNET("Affernet IV1Interface callbacks"),

    /** Route B: the legacy SensorData Messenger service, polled with raw read packets. */
    SENSORDATA_POLL("SensorData Messenger, polled reads"),

    /** Route B, older dialect: the same service asked to keep sending each value. */
    SENSORDATA_REPEATING("SensorData Messenger, repeating commands"),
}

/** Where the connection to the bike stands. [description] is for people. */
sealed interface BikeState {
    val description: String

    /** No route works here; the caller should use its fallback. [reason] says what was tried. */
    data class Unavailable(val reason: String) : BikeState {
        override val description get() = "Unavailable: $reason"
    }

    /** Binding a service and waiting for its first sample. */
    data object Connecting : BikeState {
        override val description get() = "Connecting"
    }

    /** Samples are arriving over [route]. */
    data class Connected(val route: BikeRoute) : BikeState {
        override val description get() = "Connected (${route.label})"
    }

    /** A route was delivering samples and has gone quiet. */
    data object NoData : BikeState {
        override val description get() = "No data: the sensor service has gone quiet"
    }

    /** A working connection broke. It is retried after a pause. */
    data class Error(val message: String) : BikeState {
        override val description get() = "Error: $message"
    }
}

/** Time limits for [BikeLink] and the transports, in nanoseconds. Shortened in tests. */
data class LinkTiming(
    /** How long a route may take to deliver its first sample before the next one is tried. */
    val firstDataTimeoutNanos: Long = 5 * SECOND,
    /** Connected becomes NoData after this long without a sample. */
    val staleAfterNanos: Long = 3 * SECOND,
    /** A quiet connection is dropped and detection starts over after this long. */
    val rebindAfterNanos: Long = 10 * SECOND,
    /** First pause before retrying; doubles on each failure up to [retryMaxNanos]. */
    val retryMinNanos: Long = 1 * SECOND,
    val retryMaxNanos: Long = 30 * SECOND,
    /** Route B polling: pause between read requests, and how long to wait for each answer. */
    val pollGapNanos: Long = SECOND / 10,
    val pollTimeoutNanos: Long = 3 * SECOND / 2,
) {
    companion object {
        const val SECOND = 1_000_000_000L
    }
}

/** What [BikeLink] wants done. A [Close] always precedes the next [Open]. */
sealed interface LinkAction {
    data class Open(val route: BikeRoute) : LinkAction
    data object Close : LinkAction
}

/**
 * Decides which route to use and when to give up or retry. It owns no Android objects: the caller
 * reports what happened (with the time) and carries out the returned [LinkAction]s. Not
 * thread-safe; [BikeSensors] drives it from one thread.
 *
 * - [start] tries each candidate in order. A route that fails to bind, or delivers nothing within
 *   [LinkTiming.firstDataTimeoutNanos], is closed and the next is tried.
 * - The first sample makes the state [BikeState.Connected]. Silence turns it into
 *   [BikeState.NoData], and longer silence drops the connection.
 * - When every candidate has failed the state is [BikeState.Unavailable] with what each one did,
 *   and the whole sequence is retried with a growing pause. With no candidates at all (not a
 *   Peloton tablet) nothing is retried.
 */
class BikeLink(private val timing: LinkTiming = LinkTiming()) {
    private enum class Phase { STOPPED, ABSENT, TRYING, LIVE, WAITING }

    private var phase = Phase.STOPPED
    private var candidates: List<BikeRoute> = emptyList()
    private var index = 0
    private var tryStartedNanos = 0L
    private var bound = false
    private var lastFrameNanos = 0L
    private var retryAtNanos = 0L
    private var retryDelayNanos = timing.retryMinNanos
    private val notes = ArrayList<String>()

    var state: BikeState = BikeState.Unavailable(NOT_STARTED)
        private set

    /** The route that is open (being tried or live), or null. */
    val route: BikeRoute?
        get() = if (phase == Phase.TRYING || phase == Phase.LIVE) candidates[index] else null

    /**
     * @param candidates routes whose service is installed, in preference order
     * @param absentReason shown when [candidates] is empty
     */
    fun start(nowNanos: Long, candidates: List<BikeRoute>, absentReason: String): List<LinkAction> {
        val actions = ArrayList<LinkAction>()
        if (route != null) actions += LinkAction.Close
        this.candidates = candidates
        retryDelayNanos = timing.retryMinNanos
        if (candidates.isEmpty()) {
            phase = Phase.ABSENT
            state = BikeState.Unavailable(absentReason)
            return actions
        }
        beginCycle(nowNanos, actions)
        return actions
    }

    fun stop(): List<LinkAction> {
        val actions = if (route != null) listOf(LinkAction.Close) else emptyList()
        phase = Phase.STOPPED
        state = BikeState.Unavailable(STOPPED)
        return actions
    }

    /** The open route's service is bound. Only improves the message if it then stays silent. */
    fun onBound() {
        if (phase == Phase.TRYING) bound = true
    }

    /** A valid sample arrived from the open route. */
    fun onFrame(nowNanos: Long) {
        when (phase) {
            Phase.TRYING -> {
                phase = Phase.LIVE
                lastFrameNanos = nowNanos
                retryDelayNanos = timing.retryMinNanos
                state = BikeState.Connected(candidates[index])
            }
            Phase.LIVE -> {
                lastFrameNanos = nowNanos
                if (state !is BikeState.Connected) state = BikeState.Connected(candidates[index])
            }
            else -> Unit
        }
    }

    /** The open route broke: bind refused, security exception, service died, call failed. */
    fun onFailure(nowNanos: Long, message: String): List<LinkAction> {
        val actions = ArrayList<LinkAction>()
        when (phase) {
            Phase.TRYING -> {
                notes += "${candidates[index].label}: $message"
                advance(nowNanos, actions)
            }
            Phase.LIVE -> {
                state = BikeState.Error("${candidates[index].label}: $message")
                actions += LinkAction.Close
                pause(nowNanos)
            }
            else -> Unit
        }
        return actions
    }

    /** Call a few times a second. */
    fun onTick(nowNanos: Long): List<LinkAction> {
        val actions = ArrayList<LinkAction>()
        when (phase) {
            Phase.TRYING -> if (nowNanos - tryStartedNanos >= timing.firstDataTimeoutNanos) {
                val waited = seconds(timing.firstDataTimeoutNanos)
                notes += candidates[index].label + ": " +
                    if (bound) "bound, but sent no valid data within $waited" else "did not connect within $waited"
                advance(nowNanos, actions)
            }
            Phase.LIVE -> {
                val quiet = nowNanos - lastFrameNanos
                if (quiet >= timing.rebindAfterNanos) {
                    state = BikeState.NoData
                    actions += LinkAction.Close
                    pause(nowNanos)
                } else if (quiet >= timing.staleAfterNanos) {
                    state = BikeState.NoData
                }
            }
            Phase.WAITING -> if (nowNanos >= retryAtNanos) beginCycle(nowNanos, actions)
            else -> Unit
        }
        return actions
    }

    private fun beginCycle(nowNanos: Long, actions: MutableList<LinkAction>) {
        index = 0
        notes.clear()
        open(nowNanos, actions)
    }

    private fun open(nowNanos: Long, actions: MutableList<LinkAction>) {
        phase = Phase.TRYING
        tryStartedNanos = nowNanos
        bound = false
        state = BikeState.Connecting
        actions += LinkAction.Open(candidates[index])
    }

    private fun advance(nowNanos: Long, actions: MutableList<LinkAction>) {
        actions += LinkAction.Close
        if (index + 1 < candidates.size) {
            index++
            open(nowNanos, actions)
        } else {
            state = BikeState.Unavailable(notes.joinToString("; "))
            pause(nowNanos)
        }
    }

    private fun pause(nowNanos: Long) {
        phase = Phase.WAITING
        retryAtNanos = nowNanos + retryDelayNanos
        retryDelayNanos = minOf(retryDelayNanos * 2, timing.retryMaxNanos)
    }

    private fun seconds(nanos: Long): String {
        val tenths = nanos / (LinkTiming.SECOND / 10)
        return if (tenths % 10 == 0L) "${tenths / 10} s" else "${tenths / 10}.${tenths % 10} s"
    }

    companion object {
        const val NOT_STARTED = "not started"
        const val STOPPED = "stopped"
    }
}

/**
 * The last [capacity] things that happened, oldest first, each stamped with seconds since the log
 * was created. Thread-safe.
 */
class EventLog(private val capacity: Int = 50, private val nanoTime: () -> Long = System::nanoTime) {
    private val startNanos = nanoTime()
    private val lines = ArrayDeque<String>()

    fun add(message: String) {
        val millis = (nanoTime() - startNanos) / 1_000_000
        val line = "%4d.%03d  %s".format(millis / 1000, millis % 1000, message)
        synchronized(lines) {
            if (lines.size == capacity) lines.removeFirst()
            lines.addLast(line)
        }
    }

    fun snapshot(): List<String> = synchronized(lines) { lines.toList() }
}

/** Samples per second over the last [windowNanos]. Thread-safe. */
class RateMeter(private val windowNanos: Long = 3 * LinkTiming.SECOND, capacity: Int = 256) {
    private val stamps = LongArray(capacity)
    private var count = 0
    private var next = 0

    @Synchronized
    fun mark(nowNanos: Long) {
        stamps[next] = nowNanos
        next = (next + 1) % stamps.size
        if (count < stamps.size) count++
    }

    @Synchronized
    fun perSecond(nowNanos: Long): Float {
        var inWindow = 0
        for (i in 0 until count) if (nowNanos - stamps[i] in 0 until windowNanos) inWindow++
        return inWindow * LinkTiming.SECOND.toFloat() / windowNanos
    }

    @Synchronized
    fun reset() {
        count = 0
        next = 0
    }
}

/**
 * Which packages and intents to bind. The defaults are the real Peloton services; tests point
 * them at stand-ins, since the real package names cannot be installed on an emulator.
 */
data class BikeEndpoints(
    val affernetPackage: String = AffernetWire.PACKAGE,
    val affernetAction: String = AffernetWire.INTERFACE,
    val sensorDataPackage: String = SensorDataWire.PACKAGE,
    val sensorDataAction: String = SensorDataWire.ACTION,
    val sensorDataCategory: String = SensorDataWire.CATEGORY_BIKE,
    /**
     * Whether the older repeating-command dialect of route B may be tried last. It asks the
     * service to keep sending values and has no known "stop" message, so it ends only when this
     * library unbinds.
     */
    val allowRepeatingCommands: Boolean = true,
    /** The name this client registers under with the Affernet service. */
    val clientId: String = "Soloton",
)
