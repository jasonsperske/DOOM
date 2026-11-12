package dev.soloton.doom

import android.view.KeyEvent

/** DOOM's key codes (linuxdoom-1.10/doomdef.h) and the mapping from Android keys. */
object DoomKeys {
    const val RIGHT = 0xae
    const val LEFT = 0xac
    const val UP = 0xad
    const val DOWN = 0xaf
    const val ESCAPE = 27
    const val ENTER = 13
    const val TAB = 9
    const val BACKSPACE = 127
    const val PAUSE = 0xff
    const val EQUALS = 0x3d
    const val MINUS = 0x2d
    const val RSHIFT = 0x80 + 0x36
    const val RCTRL = 0x80 + 0x1d
    const val RALT = 0x80 + 0x38
    const val F1 = 0x80 + 0x3b

    const val FIRE = RCTRL
    const val USE = ' '.code
    const val RUN = RSHIFT
    const val STRAFE_LEFT = ','.code
    const val STRAFE_RIGHT = '.'.code

    /** A hardware keyboard key as a DOOM key, or null to let Android handle it. */
    fun fromAndroid(keyCode: Int, event: KeyEvent): Int? = when (keyCode) {
        KeyEvent.KEYCODE_DPAD_UP -> UP
        KeyEvent.KEYCODE_DPAD_DOWN -> DOWN
        KeyEvent.KEYCODE_DPAD_LEFT -> LEFT
        KeyEvent.KEYCODE_DPAD_RIGHT -> RIGHT
        KeyEvent.KEYCODE_ENTER, KeyEvent.KEYCODE_NUMPAD_ENTER, KeyEvent.KEYCODE_DPAD_CENTER -> ENTER
        KeyEvent.KEYCODE_ESCAPE, KeyEvent.KEYCODE_BACK -> ESCAPE
        KeyEvent.KEYCODE_TAB -> TAB
        KeyEvent.KEYCODE_DEL -> BACKSPACE
        KeyEvent.KEYCODE_BREAK -> PAUSE
        KeyEvent.KEYCODE_CTRL_LEFT, KeyEvent.KEYCODE_CTRL_RIGHT -> RCTRL
        KeyEvent.KEYCODE_SHIFT_LEFT, KeyEvent.KEYCODE_SHIFT_RIGHT -> RSHIFT
        KeyEvent.KEYCODE_ALT_LEFT, KeyEvent.KEYCODE_ALT_RIGHT -> RALT
        in KeyEvent.KEYCODE_F1..KeyEvent.KEYCODE_F10 -> F1 + (keyCode - KeyEvent.KEYCODE_F1)
        KeyEvent.KEYCODE_F11 -> 0x80 + 0x57
        KeyEvent.KEYCODE_F12 -> 0x80 + 0x58
        else -> {
            // Letters, digits and punctuation: DOOM uses lower-case ASCII.
            val ch = event.getUnicodeChar(0)
            if (ch in 0x20..0x7e) Character.toLowerCase(ch) else null
        }
    }
}
