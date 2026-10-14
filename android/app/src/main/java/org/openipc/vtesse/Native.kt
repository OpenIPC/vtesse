package org.openipc.vtesse

/** JNI bridge (src/main/cpp/vtesse_jni.c). One tunnel per process. */
object Native {
    init {
        System.loadLibrary("vtesse")
    }

    /** Starts the tunnel from key=value lines; null on success, else an error message. */
    @JvmStatic external fun start(config: String): String?

    /** Stops the tunnel (sends CONN_CLOSE) and waits for the network thread. */
    @JvmStatic external fun stop()

    /** The device's network changed: reconnect at once. */
    @JvmStatic external fun networkChanged()

    /** Status snapshot: key=value lines, "route" and "log" repeat. */
    @JvmStatic external fun status(): String

    @JvmStatic external fun version(): String

    /** null if [id] is a valid device ID, else a message. */
    @JvmStatic external fun checkDeviceId(id: String): String?
}
