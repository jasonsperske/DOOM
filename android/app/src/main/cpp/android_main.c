// JNI bridge: starts DOOM on its own thread and passes input, the surface and lifecycle
// events across from the Android UI thread.

#include <jni.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <android/native_window_jni.h>

#include "doom_android.h"
#include "ride.h"

#include "doomdef.h"
#include "doomstat.h"
#include "d_event.h"
#include "d_main.h"
#include "m_argv.h"

extern boolean menuactive;
extern int messageToPrint;
extern boolean automapactive;
extern int screenblocks;

static JavaVM* g_vm;
static jclass g_bridgeClass;   // dev.soloton.doom.DoomNative
static pthread_t g_gameThread;
static int g_started;

// ---------------------------------------------------------------------------
// stdout/stderr -> logcat. DOOM reports progress and errors with printf.

static int g_logPipe[2];

static void* LogThread(void* unused)
{
    char buf[512];
    FILE* in = fdopen(g_logPipe[0], "r");
    while (in && fgets(buf, sizeof(buf), in))
    {
        size_t n = strlen(buf);
        if (n && buf[n-1] == '\n')
            buf[n-1] = 0;
        __android_log_write(ANDROID_LOG_INFO, LOG_TAG, buf);
    }
    return NULL;
}

static void RedirectStdio(void)
{
    pthread_t t;
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    if (pipe(g_logPipe) != 0)
        return;
    dup2(g_logPipe[1], 1);
    dup2(g_logPipe[1], 2);
    pthread_create(&t, NULL, LogThread, NULL);
    pthread_detach(t);
}

// ---------------------------------------------------------------------------
// Input queue

#define MAX_QUEUED 256

static pthread_mutex_t g_inputLock = PTHREAD_MUTEX_INITIALIZER;
static event_t g_queue[MAX_QUEUED];
static int g_queueHead, g_queueTail;
static int g_mouseDx;

void DA_PostKey(int doomkey, int down)
{
    pthread_mutex_lock(&g_inputLock);
    int next = (g_queueHead + 1) % MAX_QUEUED;
    if (next != g_queueTail)   // drop input rather than overwrite when full
    {
        event_t* ev = &g_queue[g_queueHead];
        memset(ev, 0, sizeof(*ev));
        ev->type = down ? ev_keydown : ev_keyup;
        ev->data1 = doomkey;
        g_queueHead = next;
    }
    pthread_mutex_unlock(&g_inputLock);
}

void DA_AddMouse(int dx)
{
    pthread_mutex_lock(&g_inputLock);
    g_mouseDx += dx;
    pthread_mutex_unlock(&g_inputLock);
}

void DA_DrainInput(void)
{
    event_t events[MAX_QUEUED];
    int count = 0;
    int dx;

    pthread_mutex_lock(&g_inputLock);
    while (g_queueTail != g_queueHead)
    {
        events[count++] = g_queue[g_queueTail];
        g_queueTail = (g_queueTail + 1) % MAX_QUEUED;
    }
    dx = g_mouseDx;
    g_mouseDx = 0;
    pthread_mutex_unlock(&g_inputLock);

    for (int i = 0; i < count; i++)
        D_PostEvent(&events[i]);

    // G_Responder keeps only the latest mouse event per tic, so send one summed event.
    // G_BuildTiccmd turns by dx*8 into a 16-bit angleturn: keep it from wrapping.
    if (dx > 4000)
        dx = 4000;
    else if (dx < -4000)
        dx = -4000;
    if (dx)
    {
        event_t ev;
        memset(&ev, 0, sizeof(ev));
        ev.type = ev_mouse;
        ev.data2 = dx;
        D_PostEvent(&ev);
    }
}

// ---------------------------------------------------------------------------
// Surface

static pthread_mutex_t g_windowLock = PTHREAD_MUTEX_INITIALIZER;
static ANativeWindow* g_window;

void DA_SetWindow(ANativeWindow* window)
{
    pthread_mutex_lock(&g_windowLock);
    if (g_window)
        ANativeWindow_release(g_window);
    g_window = window;
    if (g_window)
        ANativeWindow_setBuffersGeometry(g_window, 0, 0, WINDOW_FORMAT_RGBX_8888);
    pthread_mutex_unlock(&g_windowLock);
}

ANativeWindow* DA_LockWindow(void)
{
    pthread_mutex_lock(&g_windowLock);
    if (!g_window)
    {
        pthread_mutex_unlock(&g_windowLock);
        return NULL;
    }
    return g_window;
}

void DA_UnlockWindow(void)
{
    pthread_mutex_unlock(&g_windowLock);
}

// ---------------------------------------------------------------------------
// Pause

static pthread_mutex_t g_pauseLock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_pauseCond = PTHREAD_COND_INITIALIZER;
static int g_paused;
static long long g_pausedAt;
static long long g_pausedTotal;

static long long NowMillis(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void DA_SetPaused(int paused)
{
    pthread_mutex_lock(&g_pauseLock);
    if (paused && !g_paused)
        g_pausedAt = NowMillis();
    else if (!paused && g_paused)
        g_pausedTotal += NowMillis() - g_pausedAt;
    g_paused = paused;
    pthread_cond_broadcast(&g_pauseCond);
    pthread_mutex_unlock(&g_pauseLock);
    DA_SoundPause(paused);
}

void DA_WaitWhilePaused(void)
{
    pthread_mutex_lock(&g_pauseLock);
    while (g_paused)
        pthread_cond_wait(&g_pauseCond, &g_pauseLock);
    pthread_mutex_unlock(&g_pauseLock);
}

long long DA_PausedMillis(void)
{
    long long total;
    pthread_mutex_lock(&g_pauseLock);
    total = g_pausedTotal;
    if (g_paused)
        total += NowMillis() - g_pausedAt;
    pthread_mutex_unlock(&g_pauseLock);
    return total;
}

// ---------------------------------------------------------------------------
// Callbacks into Java

static JNIEnv* GameThreadEnv(void)
{
    JNIEnv* env = NULL;
    if ((*g_vm)->GetEnv(g_vm, (void**)&env, JNI_VERSION_1_6) != JNI_OK)
        (*g_vm)->AttachCurrentThread(g_vm, &env, NULL);
    return env;
}

static void Hang(void) __attribute__((noreturn));
static void Hang(void)
{
    // The Java side ends the process; the game thread must not touch engine state again.
    for (;;)
        sleep(60);
}

void DA_MusicPlay(const char* midiPath, int looping)
{
    JNIEnv* env = GameThreadEnv();
    jmethodID m = (*env)->GetStaticMethodID(env, g_bridgeClass, "onMusicPlay", "(Ljava/lang/String;Z)V");
    jstring jpath = (*env)->NewStringUTF(env, midiPath);
    (*env)->CallStaticVoidMethod(env, g_bridgeClass, m, jpath, (jboolean)(looping != 0));
    (*env)->DeleteLocalRef(env, jpath);
}

void DA_MusicStop(void)
{
    JNIEnv* env = GameThreadEnv();
    jmethodID m = (*env)->GetStaticMethodID(env, g_bridgeClass, "onMusicStop", "()V");
    (*env)->CallStaticVoidMethod(env, g_bridgeClass, m);
}

void DA_MusicPause(int paused)
{
    JNIEnv* env = GameThreadEnv();
    jmethodID m = (*env)->GetStaticMethodID(env, g_bridgeClass, "onMusicPause", "(Z)V");
    (*env)->CallStaticVoidMethod(env, g_bridgeClass, m, (jboolean)(paused != 0));
}

void DA_MusicVolume(float volume)
{
    JNIEnv* env = GameThreadEnv();
    jmethodID m = (*env)->GetStaticMethodID(env, g_bridgeClass, "onMusicVolume", "(F)V");
    (*env)->CallStaticVoidMethod(env, g_bridgeClass, m, (jfloat)volume);
}

void DA_Quit(void)
{
    JNIEnv* env = GameThreadEnv();
    jmethodID m = (*env)->GetStaticMethodID(env, g_bridgeClass, "onQuit", "()V");
    (*env)->CallStaticVoidMethod(env, g_bridgeClass, m);
    Hang();
}

void DA_FatalError(const char* message)
{
    JNIEnv* env = GameThreadEnv();
    LOGE("I_Error: %s", message);
    jmethodID m = (*env)->GetStaticMethodID(env, g_bridgeClass, "onFatalError", "(Ljava/lang/String;)V");
    jstring jmsg = (*env)->NewStringUTF(env, message);
    (*env)->CallStaticVoidMethod(env, g_bridgeClass, m, jmsg);
    Hang();
}

// ---------------------------------------------------------------------------
// Game thread

static int g_argc;
static char** g_argv;

static void* GameThread(void* unused)
{
    myargc = g_argc;
    myargv = g_argv;
    D_DoomMain();   // never returns: I_Quit and I_Error end in DA_Quit / DA_FatalError
    return NULL;
}

// ---------------------------------------------------------------------------
// JNI entry points: dev.soloton.doom.DoomNative

jint JNI_OnLoad(JavaVM* vm, void* reserved)
{
    JNIEnv* env;
    g_vm = vm;
    if ((*vm)->GetEnv(vm, (void**)&env, JNI_VERSION_1_6) != JNI_OK)
        return JNI_ERR;
    // Look the class up here: FindClass on the game thread sees only the system class loader.
    jclass cls = (*env)->FindClass(env, "dev/soloton/doom/DoomNative");
    if (!cls)
        return JNI_ERR;
    g_bridgeClass = (*env)->NewGlobalRef(env, cls);
    return JNI_VERSION_1_6;
}

static char* CopyJString(JNIEnv* env, jstring s)
{
    const char* utf = (*env)->GetStringUTFChars(env, s, NULL);
    char* copy = strdup(utf);
    (*env)->ReleaseStringUTFChars(env, s, utf);
    return copy;
}

JNIEXPORT jboolean JNICALL
Java_dev_soloton_doom_DoomNative_nativeStart(JNIEnv* env, jclass cls,
                                             jobjectArray args, jstring wadDir, jstring homeDir)
{
    if (g_started)
        return JNI_FALSE;   // DOOM keeps global state and can only start once per process
    g_started = 1;

    RedirectStdio();

    char* wad = CopyJString(env, wadDir);
    char* home = CopyJString(env, homeDir);
    setenv("DOOMWADDIR", wad, 1);
    setenv("HOME", home, 1);
    chdir(home);   // screenshots and savegames go to the working directory

    int n = (*env)->GetArrayLength(env, args);
    g_argc = n + 1;
    g_argv = calloc(g_argc + 1, sizeof(char*));
    g_argv[0] = strdup("doom");
    for (int i = 0; i < n; i++)
        g_argv[i + 1] = CopyJString(env, (jstring)(*env)->GetObjectArrayElement(env, args, i));

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 8 * 1024 * 1024);
    pthread_create(&g_gameThread, &attr, GameThread, NULL);
    pthread_attr_destroy(&attr);
    return JNI_TRUE;
}

JNIEXPORT void JNICALL
Java_dev_soloton_doom_DoomNative_nativeSetSurface(JNIEnv* env, jclass cls, jobject surface)
{
    DA_SetWindow(surface ? ANativeWindow_fromSurface(env, surface) : NULL);
}

JNIEXPORT void JNICALL
Java_dev_soloton_doom_DoomNative_nativeKey(JNIEnv* env, jclass cls, jint key, jboolean down)
{
    DA_PostKey(key, down);
}

JNIEXPORT void JNICALL
Java_dev_soloton_doom_DoomNative_nativeMouse(JNIEnv* env, jclass cls, jint dx)
{
    DA_AddMouse(dx);
}

JNIEXPORT void JNICALL
Java_dev_soloton_doom_DoomNative_nativeSetPaused(JNIEnv* env, jclass cls, jboolean paused)
{
    DA_SetPaused(paused);
}

// Bits for DoomNative.UI_*: lets the touch overlay switch between game and menu controls.
#define UI_MENU      1
#define UI_PROMPT    2
#define UI_LEVEL     4
#define UI_AUTOMAP   8
#define UI_DEMO      16
#define UI_RIDE      32
#define UI_STATUSBAR 64   // the status bar (with the face) is on screen

JNIEXPORT jint JNICALL
Java_dev_soloton_doom_DoomNative_nativeUiState(JNIEnv* env, jclass cls)
{
    int state = 0;
    if (menuactive)
        state |= UI_MENU;
    if (messageToPrint)
        state |= UI_PROMPT;
    if (gamestate == GS_LEVEL)
        state |= UI_LEVEL;
    if (automapactive)
        state |= UI_AUTOMAP;
    if (demoplayback || gamestate == GS_DEMOSCREEN)
        state |= UI_DEMO;
    if (RIDE_IsActive())
        state |= UI_RIDE;
    if (gamestate == GS_LEVEL && (screenblocks < 11 || automapactive))
        state |= UI_STATUSBAR;
    return state;
}

// ---------------------------------------------------------------------------
// Ride mode

JNIEXPORT void JNICALL
Java_dev_soloton_doom_DoomNative_nativeSetRide(JNIEnv* env, jclass cls, jboolean enabled)
{
    RIDE_SetEnabled(enabled);
}

JNIEXPORT void JNICALL
Java_dev_soloton_doom_DoomNative_nativeSetBike(JNIEnv* env, jclass cls,
                                               jboolean live, jfloat pace, jfloat resistance)
{
    RIDE_SetBike(live, pace, resistance);
}

JNIEXPORT void JNICALL
Java_dev_soloton_doom_DoomNative_nativeRideTouch(JNIEnv* env, jclass cls, jboolean down, jfloat u, jfloat v)
{
    RIDE_SetTouch(down, u, v);
}

JNIEXPORT jfloat JNICALL
Java_dev_soloton_doom_DoomNative_nativePower(JNIEnv* env, jclass cls)
{
    return RIDE_Power();
}

JNIEXPORT jint JNICALL
Java_dev_soloton_doom_DoomNative_nativeRideObjective(JNIEnv* env, jclass cls)
{
    return RIDE_Objective();
}

JNIEXPORT jint JNICALL
Java_dev_soloton_doom_DoomNative_nativeRideWeapon(JNIEnv* env, jclass cls)
{
    return RIDE_Weapon();
}
