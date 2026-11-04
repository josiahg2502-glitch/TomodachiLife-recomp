/* Android: SDL's Java activity (org.libsdl.app.SDLActivity, scripts/android) loads libmain.so and calls SDL_main on a thread
 * of its own. The game's files are in one of two places, laid out the same way:
 *   - a "TomodachiLife" folder at the top of the internal storage or of an SD card: copied there from the PC by cable or
 *     card, or by scripts/android/build_apk.py --push. The app reads it with the "all files access" permission (Android 11
 *     and newer; "storage" on Android 10), which it asks for at its first start;
 *   - the app's own folder on the shared storage, Android/data/<package>/files (no permission needed; adb cannot make
 *     folders there on Android 11 and newer, a cable copy can).
 *     game/      main.elf, sdk.elf, main.npdm (the converted dump, build/binaries/<version>) and romfs/
 *     fonts/     the console's fonts or the open-source ones (runtime/fonts_open)
 *     userdata/  the saves (made beside the game's files: they stay when the app is removed)
 * The log of the last start is Android/data/<package>/files/last_run.log; env.txt beside it sets environment variables for
 * a start. The runtime's working folder and its shader caches are in the app's private folder (files/work, files/cache). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <jni.h>
#include <SDL3/SDL.h>

int tomo_main(int argc, char **argv);                 /* boot.c's main, compiled under that name for Android (build.py) */
extern int __argc;                                    /* plat_posix.c */
extern char **__argv;

static int stop(const char *msg) {
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Tomodachi Life", msg, NULL);
    printf("android: %s\n", msg);
    fflush(stdout);
    return 1;
}
static int has_game(const char *base) {
    char p[1024];
    snprintf(p, sizeof p, "%s/game/main.elf", base);
    return access(p, R_OK) == 0;
}

/* ---- the shared storage: "all files access" (Android 11+) or WRITE_EXTERNAL_STORAGE (Android 10: the saves are written there) */
static int sdk_int(JNIEnv *e) {
    jclass c = (*e)->FindClass(e, "android/os/Build$VERSION");
    int n = (*e)->GetStaticIntField(e, c, (*e)->GetStaticFieldID(e, c, "SDK_INT", "I"));
    (*e)->DeleteLocalRef(e, c);
    return n;
}
static int storage_access(JNIEnv *e) {
    if (sdk_int(e) >= 30) {
        jclass c = (*e)->FindClass(e, "android/os/Environment");
        jboolean ok = (*e)->CallStaticBooleanMethod(e, c, (*e)->GetStaticMethodID(e, c, "isExternalStorageManager", "()Z"));
        (*e)->DeleteLocalRef(e, c);
        return ok;
    }
    jobject act = (jobject)SDL_GetAndroidActivity();
    jclass c = (*e)->GetObjectClass(e, act);
    jstring perm = (*e)->NewStringUTF(e, "android.permission.WRITE_EXTERNAL_STORAGE");
    jint r = (*e)->CallIntMethod(e, act, (*e)->GetMethodID(e, c, "checkSelfPermission", "(Ljava/lang/String;)I"), perm);
    (*e)->DeleteLocalRef(e, perm); (*e)->DeleteLocalRef(e, c); (*e)->DeleteLocalRef(e, act);
    return r == 0;                                     /* PackageManager.PERMISSION_GRANTED */
}
static void SDLCALL permission_answer(void *u, const char *perm, bool granted) { (void)u; printf("android: %s %s\n", perm, granted ? "granted" : "refused"); }
static void ask_storage_access(JNIEnv *e) {           /* the system's screen for it; the answer is polled (storage_access) */
    if (sdk_int(e) < 30) { SDL_RequestAndroidPermission("android.permission.WRITE_EXTERNAL_STORAGE", permission_answer, NULL); return; }
    jobject act = (jobject)SDL_GetAndroidActivity();
    jclass ac = (*e)->GetObjectClass(e, act), uc = (*e)->FindClass(e, "android/net/Uri"), ic = (*e)->FindClass(e, "android/content/Intent");
    jstring pkg = (jstring)(*e)->CallObjectMethod(e, act, (*e)->GetMethodID(e, ac, "getPackageName", "()Ljava/lang/String;"));
    const char *ps = (*e)->GetStringUTFChars(e, pkg, NULL);
    char spec[300];
    snprintf(spec, sizeof spec, "package:%s", ps);
    (*e)->ReleaseStringUTFChars(e, pkg, ps);
    jstring js = (*e)->NewStringUTF(e, spec), action = (*e)->NewStringUTF(e, "android.settings.MANAGE_APP_ALL_FILES_ACCESS_PERMISSION");
    jobject uri = (*e)->CallStaticObjectMethod(e, uc, (*e)->GetStaticMethodID(e, uc, "parse", "(Ljava/lang/String;)Landroid/net/Uri;"), js);
    jobject in = (*e)->NewObject(e, ic, (*e)->GetMethodID(e, ic, "<init>", "(Ljava/lang/String;Landroid/net/Uri;)V"), action, uri);
    jmethodID start = (*e)->GetMethodID(e, ac, "startActivity", "(Landroid/content/Intent;)V");
    (*e)->CallVoidMethod(e, act, start, in);
    if ((*e)->ExceptionCheck(e)) {                    /* (a system without the per-app screen: the list of all apps) */
        (*e)->ExceptionClear(e);
        jstring all = (*e)->NewStringUTF(e, "android.settings.MANAGE_ALL_FILES_ACCESS_PERMISSION");
        jobject in2 = (*e)->NewObject(e, ic, (*e)->GetMethodID(e, ic, "<init>", "(Ljava/lang/String;)V"), all);
        (*e)->CallVoidMethod(e, act, start, in2);
        if ((*e)->ExceptionCheck(e)) (*e)->ExceptionClear(e);
        (*e)->DeleteLocalRef(e, in2); (*e)->DeleteLocalRef(e, all);
    }
    (*e)->DeleteLocalRef(e, in); (*e)->DeleteLocalRef(e, uri); (*e)->DeleteLocalRef(e, action); (*e)->DeleteLocalRef(e, js);
    (*e)->DeleteLocalRef(e, pkg); (*e)->DeleteLocalRef(e, ic); (*e)->DeleteLocalRef(e, uc); (*e)->DeleteLocalRef(e, ac); (*e)->DeleteLocalRef(e, act);
}
static int find_shared(char *out, size_t n) {          /* /storage/emulated/0/TomodachiLife, then /storage/<SD card>/TomodachiLife */
    DIR *d;
    struct dirent *de;
    snprintf(out, n, "/storage/emulated/0/TomodachiLife");
    if (has_game(out)) return 1;
    if (!(d = opendir("/storage"))) return 0;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.' || !strcmp(de->d_name, "emulated") || !strcmp(de->d_name, "self")) continue;
        snprintf(out, n, "/storage/%s/TomodachiLife", de->d_name);
        if (has_game(out)) { closedir(d); return 1; }
    }
    closedir(d);
    return 0;
}

__attribute__((visibility("default"))) int SDL_main(int argc, char *argv[]) {
    const char *ext = SDL_GetAndroidExternalStoragePath(), *in = SDL_GetAndroidInternalStoragePath();
    char p[1024], base[1024], elf[1100], msg[1600];
    (void)argc; (void)argv;
    if (!ext || !in) return stop("The app's storage folders are not available.");
    snprintf(p, sizeof p, "%s/last_run.log", ext);
    if (freopen(p, "w", stdout)) dup2(fileno(stdout), 2);
    setvbuf(stdout, NULL, _IOLBF, 0);
    {   /* env.txt beside the log: NAME=value lines set for this start (a desktop build's environment variables, e.g.
         * SSBU_VK_NO_BC=1 or SSBU_VK_VALIDATE=1, which an Android app cannot be given otherwise); # starts a comment */
        char line[512];
        FILE *f;
        snprintf(p, sizeof p, "%s/env.txt", ext);
        if ((f = fopen(p, "r")) != NULL) {
            while (fgets(line, sizeof line, f)) {
                char *eq = strchr(line, '='), *end = line + strcspn(line, "\r\n");
                *end = 0;
                if (line[0] == '#' || !eq || eq == line) continue;
                *eq = 0;
                setenv(line, eq + 1, 1);
                printf("android: env.txt: %s=%s\n", line, eq + 1);
            }
            fclose(f);
        }
    }
    snprintf(base, sizeof base, "%s", ext);
    if (!has_game(base)) {                            /* not in the app's folder: the TomodachiLife folder */
        JNIEnv *e = (JNIEnv *)SDL_GetAndroidJNIEnv();
        if (!e) return stop("The app cannot reach Android's interfaces (JNI).");
        if (!storage_access(e)) {
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "Tomodachi Life",
                                     "To read the game's files from the TomodachiLife folder on your storage or SD card, the app needs "
                                     "access to your files. Allow it on the next screen, then come back.", NULL);
            ask_storage_access(e);
            for (int i = 0; i < 600 && !storage_access(e); i++) SDL_Delay(500);      /* 5 minutes for the switch */
        }
        if (!find_shared(base, sizeof base)) {
            snprintf(msg, sizeof msg, "The game's files were not found. Copy the TomodachiLife folder your PC made (docs/ANDROID.md) to the "
                     "top of this device's storage or SD card, so that TomodachiLife/game/main.elf is there%s.",
                     storage_access(e) ? "" : ", and allow the app to access your files");
            return stop(msg);
        }
    }
    printf("android: the game's files: %s\n", base);
    snprintf(p, sizeof p, "%s/game/romfs", base); setenv("SSBU_ROMFS", p, 1);
    snprintf(p, sizeof p, "%s/fonts", base); setenv("SSBU_FONTS", p, 1);
    snprintf(p, sizeof p, "%s/userdata", base); mkdir(p, 0755); setenv("SSBU_USERDATA", p, 1);
    snprintf(p, sizeof p, "%s/mods", base); mkdir(p, 0755); setenv("TOMO_MODS", p, 0);      /* (where a player can put them) */
    setenv("SSBU_RENDERER", "vk", 1);                 /* (the OpenGL renderer is Windows only) */
    setenv("SSBU_OS_CONSOLE_CORES", "1", 0);
    snprintf(p, sizeof p, "%s/cache", in); mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/config", in); mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/work", in); mkdir(p, 0755);
    if (chdir(p)) return stop("The app's working folder cannot be used.");
    snprintf(elf, sizeof elf, "%s/game/main.elf", base);
    char prog[] = "tomodachi";
    static char *args[3];
    args[0] = prog; args[1] = elf;
    __argc = 2; __argv = args;                       /* plat_posix.c's: what Windows' CRT gives (sdk.elf, the shader store find main.elf by it); the process is Java's */
    return tomo_main(2, args);
}
