/* Android-only calls into the Java side (tv.stremioorigin.app.SbUpdater). */
#ifndef SB_ANDROID_BRIDGE_H
#define SB_ANDROID_BRIDGE_H
/* downloads the APK, checks its SHA-256 (lower-case hex) and hands it to the system's installer; 0 = could not start */
int andr_install_start(const char *url, const char *sha256, const char *version);
/* where it is: 0 = on its way (msg: progress, may be ""), 1 = with the system's installer (msg says so), -1 = failed (msg: why) */
int andr_install_poll(char *msg, int cap);
#endif
