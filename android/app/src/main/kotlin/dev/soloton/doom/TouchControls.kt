package dev.soloton.doom

import android.annotation.SuppressLint
import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.PointF
import android.view.MotionEvent
import android.view.View
import java.util.Locale
import kotlin.math.abs
import kotlin.math.hypot
import kotlin.math.max
import kotlin.math.min

/**
 * Touch controls drawn over the game, laid out for the gen 1 Peloton's 1920x1080 panel at arm's
 * length (targets of 96 px or more), and scaled for other screens.
 *
 * **Ride mode** (the default while the level has a route, see ride.c): the bike moves the player.
 * - Hold a finger on a monster to target it and fire. Pedal power picks the weapon, so the weapon
 *   buttons are hidden.
 * - GO (bottom left) stands in for pedalling when no bike is connected.
 * - FREE switches to the touch controls below; RIDE switches back.
 *
 * **Free mode:**
 * - Bottom left: a stick, started anywhere in the left strip. Up/down walks, left/right strafes,
 *   pushing to the rim runs.
 * - Anywhere on the game: drag sideways to turn.
 * - FIRE and USE at the bottom right; dragging a finger on FIRE also turns.
 *
 * In a level, tapping the face in the status bar opens the menu, as MENU does.
 *
 * Controls sit in the strips beside the 4:3 game so they barely cover it: MENU, MAP, RIDE/FREE and
 * the bike readout down the left, weapons 1-7 (not in ride mode), USE and FIRE down the right.
 * In menus the stick sends arrow keys, FIRE becomes OK (Enter; also "yes" in prompts) and
 * USE becomes BACK.
 */
@SuppressLint("ClickableViewAccessibility")
class TouchControls(context: Context) : View(context) {

    /** What the bike is doing, for the readout. Set by the activity. */
    data class BikeStatus(val live: Boolean, val cadenceRpm: Float, val resistance: Float)

    @Volatile var bikeStatus = BikeStatus(false, 0f, 0f)

    /** True while GO is held: the activity turns it into pace when no bike is live. */
    @Volatile var goHeld = false
        private set

    private enum class Kind { FIRE, USE, MENU, MAP, WEAPON, RIDE, GO }

    private class Button(val kind: Kind, val weapon: Int = 0) {
        var cx = 0f
        var cy = 0f
        var radius = 0f
        var pointerId = -1
        var heldKey = 0
        fun contains(x: Float, y: Float) = hypot(x - cx, y - cy) <= radius * 1.15f
    }

    private val fire = Button(Kind.FIRE)
    private val use = Button(Kind.USE)
    private val menu = Button(Kind.MENU)
    private val map = Button(Kind.MAP)
    private val ride = Button(Kind.RIDE)
    private val go = Button(Kind.GO)
    private val weapons = (1..7).map { Button(Kind.WEAPON, it) }
    private val buttons = listOf(fire, use, menu, map, ride, go) + weapons

    private var rideEnabled = true

    // Floating stick (free mode)
    private var stickPointer = -1
    private val stickOrigin = PointF()
    private val stickKnob = PointF()
    private val stickRest = PointF()
    private var stickRadius = 0f
    private val stickKeys = mutableSetOf<Int>()
    private var menuDirection = 0         // arrow key currently repeating in menus, or 0

    // Turning (free mode)
    private var turnPointer = -1
    private var lastTurnX = 0f
    private var turnRemainder = 0f

    // Aiming (ride mode)
    private var aimPointer = -1

    private var unit = 1f                 // 1 at 1080 px tall
    private var stickZoneRight = 0f
    private var gameLeft = 0f
    private var gameTop = 0f
    private var gameWidth = 1f
    private var gameHeight = 1f
    private var sideColumn = 0f
    private var uiState = 0

    private val fillPaint = Paint(Paint.ANTI_ALIAS_FLAG)
    private val strokePaint = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.STROKE }
    private val textPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.WHITE
        textAlign = Paint.Align.CENTER
        isFakeBoldText = true
    }

    private val menuMode get() = uiState and (DoomNative.UI_MENU or DoomNative.UI_PROMPT) != 0
    private val rideMode get() = !menuMode && uiState and DoomNative.UI_RIDE != 0
    private val inLevel get() = uiState and DoomNative.UI_LEVEL != 0

    private val repeatArrow = object : Runnable {
        override fun run() {
            if (menuDirection != 0) {
                DoomNative.tapKey(menuDirection)
                postDelayed(this, MENU_REPEAT_MS)
            }
        }
    }

    private val pollState = object : Runnable {
        override fun run() {
            refreshUiState()
            invalidate()   // the bike readout changes continuously
            postDelayed(this, STATE_POLL_MS)
        }
    }

    override fun onAttachedToWindow() {
        super.onAttachedToWindow()
        post(pollState)
    }

    override fun onDetachedFromWindow() {
        removeCallbacks(pollState)
        removeCallbacks(repeatArrow)
        super.onDetachedFromWindow()
    }

    override fun onSizeChanged(w: Int, h: Int, oldw: Int, oldh: Int) {
        unit = h / 1080f
        gameWidth = min(w.toFloat(), h * 4f / 3f)
        gameHeight = gameWidth * 3f / 4f
        gameLeft = (w - gameWidth) / 2f
        gameTop = (h - gameHeight) / 2f
        // Everything sits in the black strips beside the 4:3 game (240 px each on the bike). Where
        // there are no strips, the same columns overlay the edges of the game.
        val column = max(gameLeft, 240f * unit)
        val half = column / 2f
        sideColumn = half
        val rightX = w - half

        // Left: MENU, MAP, RIDE/FREE, the readout, then the stick (free) or GO (ride) at the bottom.
        place(menu, half, 90f * unit, min(70f * unit, half - 10f * unit))
        place(map, half, 240f * unit, min(70f * unit, half - 10f * unit))
        place(ride, half, 390f * unit, min(70f * unit, half - 10f * unit))
        place(go, half, h - 200f * unit, min(105f * unit, half - 10f * unit))
        stickRadius = min(110f * unit, half - 10f * unit)
        stickRest.set(half, h - 200f * unit)
        stickZoneRight = column

        // Right: weapons in two columns, then USE, then FIRE at the bottom.
        val weaponRadius = min(50f * unit, column / 4f - 6f * unit)
        weapons.forEachIndexed { i, b ->
            val x = if (i % 2 == 0) rightX - column / 4f else rightX + column / 4f
            place(b, x, 70f * unit + (i / 2) * 112f * unit, weaponRadius)
        }
        place(use, rightX, 620f * unit, min(85f * unit, half - 10f * unit))
        place(fire, rightX, h - 190f * unit, min(115f * unit, half - 8f * unit))

        textPaint.textSize = 34f * unit
        strokePaint.strokeWidth = 4f * unit
    }

    private fun place(b: Button, x: Float, y: Float, r: Float) {
        b.cx = x
        b.cy = y
        b.radius = r
    }

    private fun refreshUiState() {
        val state = DoomNative.nativeUiState()
        if (state == uiState) return
        val wasMenu = menuMode
        val wasRide = rideMode
        uiState = state
        if (wasMenu != menuMode || wasRide != rideMode) releaseAll()
        invalidate()
    }

    // -----------------------------------------------------------------------------------------
    // Touch

    override fun onTouchEvent(event: MotionEvent): Boolean {
        refreshUiState()
        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN, MotionEvent.ACTION_POINTER_DOWN -> {
                val i = event.actionIndex
                pointerDown(event.getPointerId(i), event.getX(i), event.getY(i))
            }
            MotionEvent.ACTION_MOVE -> {
                for (i in 0 until event.pointerCount) {
                    pointerMove(event.getPointerId(i), event.getX(i), event.getY(i))
                }
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_POINTER_UP -> pointerUp(event.getPointerId(event.actionIndex))
            MotionEvent.ACTION_CANCEL -> releaseAll()
        }
        invalidate()
        return true
    }

    private fun visibleButtons(): List<Button> = when {
        menuMode -> listOf(fire, use, menu)
        rideMode -> listOfNotNull(menu, map, ride, go.takeIf { !bikeStatus.live })
        inLevel -> listOf(fire, use, menu, map, ride) + weapons
        else -> listOf(fire, use, menu, map) + weapons
    }

    /** Is (x, y) on the status bar's face? */
    private fun onFace(x: Float, y: Float): Boolean {
        if (menuMode || uiState and DoomNative.UI_STATUSBAR == 0) return false
        val fx = (x - gameLeft) / gameWidth * 320f
        val fy = (y - gameTop) / gameHeight * 200f
        return fx in FACE_LEFT..FACE_RIGHT && fy >= FACE_TOP
    }

    private fun pointerDown(id: Int, x: Float, y: Float) {
        if (onFace(x, y)) {
            DoomNative.tapKey(DoomKeys.ESCAPE)
            return
        }
        val button = visibleButtons().firstOrNull { it.pointerId == -1 && it.contains(x, y) }
        if (button != null) {
            button.pointerId = id
            pressButton(button)
            if (button === fire && turnPointer == -1) startTurn(id, x)
            return
        }
        if (rideMode) {
            if (aimPointer == -1) {
                aimPointer = id
                sendAim(true, x, y)
            }
            return
        }
        if (x < stickZoneRight && stickPointer == -1) {
            stickPointer = id
            stickOrigin.set(x, y)
            stickKnob.set(x, y)
            return
        }
        if (turnPointer == -1) startTurn(id, x)
    }

    private fun pointerMove(id: Int, x: Float, y: Float) {
        if (id == aimPointer) sendAim(true, x, y)
        if (id == stickPointer) {
            stickKnob.set(x, y)
            updateStick()
        }
        if (id == turnPointer && !menuMode) {
            turnRemainder += (x - lastTurnX) * TURN_PER_PIXEL / unit
            lastTurnX = x
            val whole = turnRemainder.toInt()
            if (whole != 0) {
                DoomNative.nativeMouse(whole)
                turnRemainder -= whole
            }
        }
    }

    private fun pointerUp(id: Int) {
        buttons.firstOrNull { it.pointerId == id }?.let { releaseButton(it) }
        if (id == aimPointer) {
            aimPointer = -1
            sendAim(false, 0f, 0f)
        }
        if (id == stickPointer) {
            stickPointer = -1
            setStickKeys(emptySet())
            setMenuDirection(0)
        }
        if (id == turnPointer) turnPointer = -1
    }

    private fun sendAim(down: Boolean, x: Float, y: Float) {
        DoomNative.nativeRideTouch(down, (x - gameLeft) / gameWidth, (y - gameTop) / gameHeight)
    }

    private fun startTurn(id: Int, x: Float) {
        turnPointer = id
        lastTurnX = x
        turnRemainder = 0f
    }

    private fun releaseAll() {
        buttons.forEach { if (it.pointerId != -1) releaseButton(it) }
        if (aimPointer != -1) sendAim(false, 0f, 0f)
        aimPointer = -1
        stickPointer = -1
        turnPointer = -1
        setStickKeys(emptySet())
        setMenuDirection(0)
    }

    // -----------------------------------------------------------------------------------------
    // Buttons

    private fun pressButton(b: Button) {
        val key = when (b.kind) {
            Kind.FIRE -> if (menuMode) DoomKeys.ENTER else DoomKeys.FIRE
            Kind.USE -> if (menuMode) DoomKeys.BACKSPACE else DoomKeys.USE
            Kind.MENU -> DoomKeys.ESCAPE
            Kind.MAP -> DoomKeys.TAB
            Kind.WEAPON -> '0'.code + b.weapon
            Kind.RIDE -> {
                rideEnabled = !rideEnabled
                DoomNative.nativeSetRide(rideEnabled)
                0
            }
            Kind.GO -> {
                goHeld = true
                0
            }
        }
        b.heldKey = key
        if (key != 0) DoomNative.nativeKey(key, true)
    }

    private fun releaseButton(b: Button) {
        if (b.heldKey != 0) DoomNative.nativeKey(b.heldKey, false)
        if (b.kind == Kind.GO) goHeld = false
        b.heldKey = 0
        b.pointerId = -1
    }

    // -----------------------------------------------------------------------------------------
    // Stick

    private fun updateStick() {
        var dx = stickKnob.x - stickOrigin.x
        var dy = stickKnob.y - stickOrigin.y
        val dist = hypot(dx, dy)
        if (dist > stickRadius) {
            // Clamp the knob to the rim.
            dx = dx / dist * stickRadius
            dy = dy / dist * stickRadius
            stickKnob.set(stickOrigin.x + dx, stickOrigin.y + dy)
        }
        val nx = dx / stickRadius
        val ny = dy / stickRadius

        if (menuMode) {
            setMenuDirection(
                when {
                    max(abs(nx), abs(ny)) < MENU_DEAD_ZONE -> 0
                    abs(ny) >= abs(nx) -> if (ny < 0) DoomKeys.UP else DoomKeys.DOWN
                    else -> if (nx < 0) DoomKeys.LEFT else DoomKeys.RIGHT
                }
            )
            return
        }

        val keys = mutableSetOf<Int>()
        if (ny < -DEAD_ZONE) keys += DoomKeys.UP
        if (ny > DEAD_ZONE) keys += DoomKeys.DOWN
        if (nx < -DEAD_ZONE) keys += DoomKeys.STRAFE_LEFT
        if (nx > DEAD_ZONE) keys += DoomKeys.STRAFE_RIGHT
        if (hypot(nx, ny) > RUN_THRESHOLD) keys += DoomKeys.RUN
        setStickKeys(keys)
    }

    private fun setStickKeys(keys: Set<Int>) {
        (stickKeys - keys).forEach { DoomNative.nativeKey(it, false) }
        (keys - stickKeys).forEach { DoomNative.nativeKey(it, true) }
        stickKeys.clear()
        stickKeys += keys
    }

    private fun setMenuDirection(key: Int) {
        if (key == menuDirection) return
        removeCallbacks(repeatArrow)
        menuDirection = key
        if (key != 0) {
            DoomNative.tapKey(key)
            postDelayed(repeatArrow, MENU_FIRST_REPEAT_MS)
        }
    }

    // -----------------------------------------------------------------------------------------
    // Drawing

    override fun onDraw(canvas: Canvas) {
        val inMenu = menuMode
        val riding = rideMode
        for (b in visibleButtons()) {
            val label = when (b.kind) {
                Kind.FIRE -> if (inMenu) "OK" else "FIRE"
                Kind.USE -> if (inMenu) "BACK" else "USE"
                Kind.MENU -> "MENU"
                Kind.MAP -> "MAP"
                Kind.WEAPON -> b.weapon.toString()
                Kind.RIDE -> if (rideEnabled) "FREE" else "RIDE"
                Kind.GO -> "GO"
            }
            drawControl(canvas, b.cx, b.cy, b.radius, b.pointerId != -1, label)
        }

        if (!riding) {
            if (stickPointer != -1) {
                drawRing(canvas, stickOrigin.x, stickOrigin.y, stickRadius, false)
                drawControl(canvas, stickKnob.x, stickKnob.y, stickRadius * 0.4f, true, null)
            } else {
                drawRing(canvas, stickRest.x, stickRest.y, stickRadius, false)
                textPaint.alpha = 110
                canvas.drawText(if (inMenu) "SELECT" else "MOVE", stickRest.x, stickRest.y + textPaint.textSize / 3, textPaint)
                textPaint.alpha = 255
            }
        }

        if (inLevel && !inMenu) drawBikeReadout(canvas, riding)
    }

    private fun drawBikeReadout(canvas: Canvas, riding: Boolean) {
        val bike = bikeStatus
        val lines = mutableListOf<String>()
        if (bike.live) {
            lines += String.format(Locale.US, "%.0f RPM", bike.cadenceRpm)
            lines += String.format(Locale.US, "RES %.0f%%", bike.resistance * 100f)
        } else {
            lines += "NO BIKE"
        }
        when {
            riding -> {
                lines += String.format(Locale.US, "POWER %.0f%%", DoomNative.nativePower() * 100f)
                DoomNative.WEAPON_NAMES.getOrNull(DoomNative.nativeRideWeapon())?.let { lines += it }
            }
            rideEnabled -> lines += "NO ROUTE"
        }
        val size = textPaint.textSize
        textPaint.textSize = 28f * unit
        var y = 510f * unit
        for (line in lines) {
            canvas.drawText(line, sideColumn, y, textPaint)
            y += 42f * unit
        }
        textPaint.textSize = size
    }

    private fun drawControl(canvas: Canvas, x: Float, y: Float, r: Float, pressed: Boolean, label: String?) {
        fillPaint.color = if (pressed) Color.argb(120, 255, 255, 255) else Color.argb(45, 255, 255, 255)
        canvas.drawCircle(x, y, r, fillPaint)
        drawRing(canvas, x, y, r, pressed)
        if (label != null) {
            textPaint.alpha = if (pressed) 255 else 200
            canvas.drawText(label, x, y + textPaint.textSize / 3, textPaint)
            textPaint.alpha = 255
        }
    }

    private fun drawRing(canvas: Canvas, x: Float, y: Float, r: Float, bright: Boolean) {
        strokePaint.color = if (bright) Color.argb(220, 255, 255, 255) else Color.argb(110, 255, 255, 255)
        canvas.drawCircle(x, y, r, strokePaint)
    }

    private companion object {
        const val DEAD_ZONE = 0.3f
        const val RUN_THRESHOLD = 0.85f
        const val MENU_DEAD_ZONE = 0.45f
        const val MENU_FIRST_REPEAT_MS = 400L
        const val MENU_REPEAT_MS = 160L
        const val STATE_POLL_MS = 200L

        // The face in the 320x200 frame (st_stuff.c draws it at 143,168), with a little slack.
        const val FACE_LEFT = 138f
        const val FACE_RIGHT = 182f
        const val FACE_TOP = 166f

        /** DOOM mouse units per pixel at 1080 px tall: about 180 degrees across the game view. */
        const val TURN_PER_PIXEL = 5.5f
    }
}
