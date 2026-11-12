// Shared state between the JNI bridge and the Android i_* backends.
#ifndef DOOM_ANDROID_H
#define DOOM_ANDROID_H

#include <android/log.h>
#include <android/native_window.h>
#include <pthread.h>

#define LOG_TAG "DOOM"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// Input from the UI thread, drained by I_StartTic on the game thread.
void DA_PostKey(int doomkey, int down);
void DA_AddMouse(int dx);
void DA_DrainInput(void);

// Surface handed over from the SurfaceView (NULL while it does not exist).
void DA_SetWindow(ANativeWindow* window);
ANativeWindow* DA_LockWindow(void);   // returns the window with its lock held, or NULL
void DA_UnlockWindow(void);

// Lifecycle: the game thread blocks in DA_WaitWhilePaused while the activity is paused.
void DA_SetPaused(int paused);
void DA_WaitWhilePaused(void);
long long DA_PausedMillis(void);       // total time spent paused, excluded from I_GetTime

// Sound output follows the activity lifecycle.
void DA_SoundPause(int paused);

// Calls back into Java on the game thread; neither returns.
void DA_Quit(void) __attribute__((noreturn));
void DA_FatalError(const char* message) __attribute__((noreturn));

#endif
