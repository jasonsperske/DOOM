package dev.soloton.doom

import android.annotation.SuppressLint
import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.PointF
import android.view.MotionEvent
import android.view.View
import kotlin.math.abs
import kotlin.math.hypot
import kotlin.math.max
import kotlin.math.min

/**
 * Touch controls drawn over the game, laid out for the gen 1 Peloton's 1920x1080 panel at arm's
 * length (targets of 96 px or more), and scaled for other screens.
 *
 * - Left side: a floating stick. Up/down walks, left/right strafes, pushing to the rim runs.
 * - Anywhere else: drag sideways to turn.
 * - FIRE and USE at the bottom right; dragging a finger on FIRE also turns.
 * - MENU and MAP at the top left, weapons 1-7 down the right edge.
 *
 * In menus the stick sends arrow keys, FIRE becomes OK (Enter; also "yes" in prompts) and
 * USE becomes BACK.
 */
@SuppressLint("ClickableViewAccessibility")
class TouchControls(context: Context) : View(context) {

    private enum class Kind { FIRE, USE, MENU, MAP, WEAPON }

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
    private val weapons = (1..7).map { Button(Kind.WEAPON, it) }
    private val buttons = listOf(fire, use, menu, map) + weapons

    // Floating stick
    private var stickPointer = -1
    private val stickOrigin = PointF()
    private val stickKnob = PointF()
    private val stickRest = PointF()
    private var stickRadius = 0f
    private val stickKeys = mutableSetOf<Int>()
    private var menuDirection = 0         // arrow key currently repeating in menus, or 0

    // Turning
    private var turnPointer = -1
    private var lastTurnX = 0f
    private var turnRemainder = 0f

    private var unit = 1f                 // 1 at 1080 px tall
    private var stickZoneRight = 0f
    private var uiState = 0

    private val fillPaint = Paint(Paint.ANTI_ALIAS_FLAG)
    private val strokePaint = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.STROKE }
    private val textPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.WHITE
        textAlign = Paint.Align.CENTER
        isFakeBoldText = true
    }

    private val menuMode get() = uiState and (DoomNative.UI_MENU or DoomNative.UI_PROMPT) != 0

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
        val gameWidth = h * 4f / 3f
        val pillar = max(0f, (w - gameWidth) / 2f)
        val sideColumn = max(pillar / 2f, 130f * unit)

        place(menu, sideColumn, 110f * unit, 75f * unit)
        place(map, sideColumn, 290f * unit, 75f * unit)

        val weaponStep = (h - 120f * unit) / weapons.size
        weapons.forEachIndexed { i, b ->
            place(b, w - sideColumn, 90f * unit + i * weaponStep, min(58f * unit, weaponStep * 0.45f))
        }

        val right = w - pillar
        place(fire, right - 210f * unit, h - 230f * unit, 135f * unit)
        place(use, right - 490f * unit, h - 160f * unit, 95f * unit)

        stickRadius = 160f * unit
        stickRest.set(pillar + 260f * unit, h - 280f * unit)
        stickZoneRight = w * 0.42f

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
        uiState = state
        if (wasMenu != menuMode) releaseAll()
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

    private fun visibleButtons(): List<Button> = if (menuMode) listOf(fire, use, menu) else buttons

    private fun pointerDown(id: Int, x: Float, y: Float) {
        val button = visibleButtons().firstOrNull { it.pointerId == -1 && it.contains(x, y) }
        if (button != null) {
            button.pointerId = id
            pressButton(button)
            if (button === fire && turnPointer == -1) startTurn(id, x)
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
        if (id == stickPointer) {
            stickPointer = -1
            setStickKeys(emptySet())
            setMenuDirection(0)
        }
        if (id == turnPointer) turnPointer = -1
    }

    private fun startTurn(id: Int, x: Float) {
        turnPointer = id
        lastTurnX = x
        turnRemainder = 0f
    }

    private fun releaseAll() {
        buttons.forEach { if (it.pointerId != -1) releaseButton(it) }
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
        }
        b.heldKey = key
        DoomNative.nativeKey(key, true)
    }

    private fun releaseButton(b: Button) {
        if (b.heldKey != 0) DoomNative.nativeKey(b.heldKey, false)
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
        for (b in visibleButtons()) {
            val label = when (b.kind) {
                Kind.FIRE -> if (inMenu) "OK" else "FIRE"
                Kind.USE -> if (inMenu) "BACK" else "USE"
                Kind.MENU -> "MENU"
                Kind.MAP -> "MAP"
                Kind.WEAPON -> b.weapon.toString()
            }
            drawControl(canvas, b.cx, b.cy, b.radius, b.pointerId != -1, label)
        }

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

        /** DOOM mouse units per pixel at 1080 px tall: about 180 degrees across the game view. */
        const val TURN_PER_PIXEL = 5.5f
    }
}
