// SPDX-License-Identifier: GPL-2.0-or-later
package dev.soloton.bike

import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Handler
import android.os.HandlerThread
import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.atomic.AtomicLong

/**
 * Live cadence, power and resistance from the Peloton Bike tablet's own sensor service.
 *
 * [start] looks for a route and keeps it alive on a private thread; [stop] unregisters and
 * unbinds. In between, [state] and [latest] are plain volatile reads, safe from a render thread.
 *
 * Routes are tried in this order, each given [LinkTiming.firstDataTimeoutNanos] to produce a
 * valid sample: the Affernet callback service, then the legacy SensorData Messenger service
 * polled, then the same service with repeating commands. A route whose package or service is not
 * installed is skipped. If none is installed (any device that is not a Peloton Bike) the state is
 * [BikeState.Unavailable] for good; if some are installed but none delivers, it is Unavailable
 * with the reasons and the search is repeated with a growing pause. A connection that dies or
 * goes quiet is dropped and found again the same way.
 *
 * This class only binds services and reads. It never stops, restarts or reconfigures a Peloton
 * service, and it never lets an exception from one reach the caller.
 *
 * @param endpoints which packages to bind; the defaults are the real ones
 * @param filterResistance report the lowest of the last three resistance values, which hides the
 *   single-sample spikes the sensor is known for
 */
class BikeSensors(
    context: Context,
    private val endpoints: BikeEndpoints = BikeEndpoints(),
    private val timing: LinkTiming = LinkTiming(),
    private val filterResistance: Boolean = true,
) {
    /** Called on a private worker thread or a Binder thread, never the main thread. Keep it quick. */
    interface Listener {
        fun onReading(reading: BikeReading) {}
        fun onStateChanged(state: BikeState) {}
    }

    private val appContext: Context = context.applicationContext
    private val listeners = CopyOnWriteArrayList<Listener>()
    private val log = EventLog()
    private val rate = RateMeter()
    private val accepted = AtomicLong()
    private val rejected = AtomicLong()
    private val lock = Any()

    @Volatile
    private var run: Run? = null

    /** Where the connection stands. */
    @Volatile
    var state: BikeState = BikeState.Unavailable(BikeLink.NOT_STARTED)
        private set

    /** The route being tried or in use. */
    @Volatile
    var route: BikeRoute? = null
        private set

    /** The most recent valid sample, or null if there has been none since [start]. It may be old: check [state]. */
    @Volatile
    var latest: BikeReading? = null
        private set

    /** The most recent failure or rejected message, for display. */
    @Volatile
    var lastError: String? = null
        private set

    fun addListener(listener: Listener) {
        listeners.addIfAbsent(listener)
    }

    fun removeListener(listener: Listener) {
        listeners.remove(listener)
    }

    /** Begin looking for the bike. Does nothing if already started. Returns at once. */
    fun start() {
        synchronized(lock) {
            if (run != null) return
            val thread = HandlerThread("soloton-bike")
            thread.start()
            val started = Run(thread)
            run = started
            latest = null
            rate.reset()
            started.handler.post { started.begin() }
        }
    }

    /** Unregister, unbind and stop the worker thread. Does nothing if not started. Returns at once. */
    fun stop() {
        synchronized(lock) {
            val stopping = run ?: return
            run = null
            stopping.handler.post { stopping.end() }
            stopping.thread.quitSafely()
            setState(BikeState.Unavailable(BikeLink.STOPPED), null)
        }
    }

    val isStarted: Boolean get() = run != null

    /** Everything worth showing on a diagnostics screen. Allocates; not for the render loop. */
    fun diagnostics(): BikeDiagnostics = BikeDiagnostics(
        state = state,
        route = route,
        latest = latest,
        readingsPerSecond = rate.perSecond(System.nanoTime()),
        readingsAccepted = accepted.get(),
        messagesRejected = rejected.get(),
        lastError = lastError,
        events = log.snapshot(),
    )

    private fun setState(newState: BikeState, newRoute: BikeRoute?) {
        route = newRoute
        if (newState == state) return
        state = newState
        log.add("state: ${newState.description}")
        for (listener in listeners) {
            try {
                listener.onStateChanged(newState)
            } catch (e: RuntimeException) {
                log.add("listener failed: $e")
            }
        }
    }

    /** One start-to-stop lifetime: its own thread, state machine and transport. */
    private inner class Run(val thread: HandlerThread) : TransportHost {
        override val handler = Handler(thread.looper)
        override val context: Context get() = appContext
        override val endpoints: BikeEndpoints get() = this@BikeSensors.endpoints
        override val timing: LinkTiming get() = this@BikeSensors.timing

        private val link = BikeLink(timing)
        private var transport: Transport? = null
        private val spikes = SpikeFilter()
        private var lastRejectLogNanos = Long.MIN_VALUE
        private var rejectsSinceLog = 0
        private val ticker = object : Runnable {
            override fun run() {
                if (this@BikeSensors.run !== this@Run) return
                perform(link.onTick(System.nanoTime()))
                handler.postDelayed(this, TICK_MILLIS)
            }
        }

        fun begin() {
            if (run !== this) return
            val candidates = ArrayList<BikeRoute>()
            val missing = ArrayList<String>()
            if (installed(Intent(endpoints.affernetAction).setPackage(endpoints.affernetPackage))) {
                candidates += BikeRoute.AFFERNET
            } else {
                missing += missingText(endpoints.affernetPackage)
            }
            val legacy = Intent(endpoints.sensorDataAction).setPackage(endpoints.sensorDataPackage)
            if (installed(legacy)) {
                candidates += BikeRoute.SENSORDATA_POLL
                if (endpoints.allowRepeatingCommands) candidates += BikeRoute.SENSORDATA_REPEATING
            } else {
                missing += missingText(endpoints.sensorDataPackage)
            }
            note("routes available: " + if (candidates.isEmpty()) "none" else candidates.joinToString { it.label })
            val absent = "no Peloton sensor service on this device (" + missing.joinToString("; ") + ")"
            perform(link.start(System.nanoTime(), candidates, absent))
            handler.postDelayed(ticker, TICK_MILLIS)
        }

        fun end() {
            handler.removeCallbacks(ticker)
            for (action in link.stop()) carryOut(action)
            log.add("stopped")
        }

        private fun installed(intent: Intent): Boolean = try {
            appContext.packageManager.resolveService(intent, 0) != null
        } catch (e: RuntimeException) {
            note("could not query ${intent.`package`}: $e")
            false
        }

        private fun missingText(packageName: String): String {
            val present = try {
                appContext.packageManager.getPackageInfo(packageName, 0)
                true
            } catch (_: PackageManager.NameNotFoundException) {
                false
            } catch (_: RuntimeException) {
                false
            }
            return if (present) "$packageName is installed but offers no matching service" else "$packageName is not installed"
        }

        private fun perform(actions: List<LinkAction>) {
            for (action in actions) carryOut(action)
            // Under the lock so a stop() on another thread cannot be overwritten afterwards.
            synchronized(lock) {
                if (run !== this) return
                val current = link.state
                if (current is BikeState.Unavailable || current is BikeState.Error) {
                    if (current != state) lastError = current.description
                }
                setState(current, link.route)
            }
        }

        private fun carryOut(action: LinkAction) {
            when (action) {
                LinkAction.Close -> {
                    try {
                        transport?.close()
                    } catch (e: RuntimeException) {
                        note("close failed: $e")
                    }
                    transport = null
                }
                is LinkAction.Open -> {
                    note("trying ${action.route.label}")
                    spikes.reset()
                    val opened = when (action.route) {
                        BikeRoute.AFFERNET -> AffernetTransport(this)
                        BikeRoute.SENSORDATA_POLL -> SensorDataTransport(this, repeating = false)
                        BikeRoute.SENSORDATA_REPEATING -> SensorDataTransport(this, repeating = true)
                    }
                    transport = opened
                    try {
                        opened.open()
                    } catch (e: RuntimeException) {
                        failed(opened, "could not open: $e")
                    }
                }
            }
        }

        override fun bound(transport: Transport) {
            handler.post {
                if (this.transport === transport) {
                    note("bound")
                    link.onBound()
                }
            }
        }

        override fun reading(transport: Transport, reading: BikeReading) {
            if (transport.closed || run !== this) return
            val published = if (filterResistance) {
                synchronized(spikes) { reading.copy(resistancePercent = spikes.filter(reading.resistancePercent)) }
            } else {
                reading
            }
            latest = published
            accepted.incrementAndGet()
            rate.mark(published.timestampNanos)
            for (listener in listeners) {
                try {
                    listener.onReading(published)
                } catch (e: RuntimeException) {
                    log.add("listener failed: $e")
                }
            }
            handler.post {
                if (this.transport === transport) {
                    link.onFrame(published.timestampNanos)
                    perform(emptyList())
                }
            }
        }

        override fun rejected(transport: Transport, reason: String) {
            if (transport.closed || run !== this) return
            rejected.incrementAndGet()
            lastError = reason
            // A stream of bad frames must not flood the log.
            val now = System.nanoTime()
            synchronized(spikes) {
                rejectsSinceLog++
                if (now - lastRejectLogNanos >= LinkTiming.SECOND || lastRejectLogNanos == Long.MIN_VALUE) {
                    val more = if (rejectsSinceLog > 1) " (and ${rejectsSinceLog - 1} more)" else ""
                    log.add("rejected: $reason$more")
                    lastRejectLogNanos = now
                    rejectsSinceLog = 0
                }
            }
        }

        override fun failed(transport: Transport, message: String) {
            handler.post {
                if (this.transport === transport) {
                    note("failed: $message")
                    lastError = message
                    perform(link.onFailure(System.nanoTime(), message))
                }
            }
        }

        override fun note(message: String) = log.add(message)
    }

    private companion object {
        const val TICK_MILLIS = 100L
    }
}
