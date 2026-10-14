package org.openipc.vtesse

import android.content.Context
import java.security.SecureRandom

/** A camera behind a server (#27). */
data class Camera(val name: String, val ip: String)

/**
 * One VTun server (#26, #27) with up to [CAMERAS] cameras behind it, kept in the
 * main process (SharedPreferences, keys prefixed "p<S>_"). Local ports follow a
 * fixed scheme, see [port].
 */
data class Settings(
    val name: String,
    val host: String,
    val port: Int,
    val deviceId: String,
    val password: String,
    val cameras: List<Camera>,          // always CAMERAS entries; ip "" = unused
    val rtsp: Boolean,
    val http: Boolean,
    val ssh: Boolean,
    val socks: Boolean,
    val extraForwards: List<String>,    // free-form LPORT:IP:RPORT
) {
    /** key=value text for [Native.start] (same keys as the vtesse-cli config). */
    fun toConfig(server: Int): String = buildString {
        append("host=").append(host).append('\n')
        append("port=").append(port).append('\n')
        append("session=").append(deviceId).append('\n')
        append("password=").append(password).append('\n')
        append("socks_port=").append(if (socks) socksPort(server) else 0).append('\n')
        cameras.forEachIndexed { i, c ->
            if (c.ip.isBlank()) return@forEachIndexed
            val n = i + 1
            if (rtsp) append("forward=${port(server, RTSP, n)}:${c.ip}:554\n")
            if (http) append("forward=${port(server, HTTP, n)}:${c.ip}:80\n")
            if (ssh) append("forward=${port(server, SSH, n)}:${c.ip}:22\n")
        }
        extraForwards.forEach { append("forward=").append(it).append('\n') }
    }

    /** "Server 2 · Office" (or the host) for buttons and the notification. */
    fun label(ctx: Context, server: Int): String {
        val what = name.ifBlank { host }
        val s = ctx.getString(R.string.server_n, server)
        return if (what.isBlank()) s else "$s · $what"
    }

    fun save(ctx: Context, server: Int) {
        val p = "p${server}_"
        val e = prefs(ctx).edit()
            .putString(p + K_NAME, name)
            .putString(p + K_HOST, host)
            .putInt(p + K_PORT, port)
            .putString(p + K_ID, deviceId)
            .putString(p + K_PASSWORD, password)
            .putBoolean(p + K_RTSP, rtsp)
            .putBoolean(p + K_HTTP, http)
            .putBoolean(p + K_SSH, ssh)
            .putBoolean(p + K_SOCKS_ON, socks)
            .putString(p + K_FORWARDS, extraForwards.joinToString("\n"))
        cameras.forEachIndexed { i, c ->
            e.putString("${p}cam${i + 1}_name", c.name).putString("${p}cam${i + 1}_ip", c.ip)
        }
        e.apply()
    }

    companion object {
        const val SERVERS = 3
        const val CAMERAS = 8

        /** Service codes of the port scheme. */
        const val SSH = 22
        const val RTSP = 55
        const val HTTP = 80

        /**
         * Local port of camera [camera] (1-8) of server [server] (1-3) for a service
         * code: server*1000 + code*10 + camera, e.g. RTSP of camera 3 on server 1 =
         * 1553, HTTP = 1803, SSH = 1223. 9 as the camera number is reserved (RTP/UDP, #28).
         */
        fun port(server: Int, code: Int, camera: Int) = server * 1000 + code * 10 + camera

        /** SOCKS5 port of a server: 1080, 2080, 3080. */
        fun socksPort(server: Int) = server * 1000 + 80

        private const val K_NAME = "name"
        private const val K_HOST = "host"
        private const val K_PORT = "port"
        private const val K_ID = "device_id"
        private const val K_PASSWORD = "password"
        private const val K_SOCKS = "socks_port"        // 1.05: a port number
        private const val K_SOCKS_ON = "socks"
        private const val K_RTSP = "rtsp"
        private const val K_HTTP = "http"
        private const val K_SSH = "ssh"
        private const val K_FORWARDS = "forwards"       // 1.05 forwards become extra forwards
        private const val K_ACTIVE = "active_profile"

        private fun prefs(ctx: Context) = ctx.getSharedPreferences("vtesse", Context.MODE_PRIVATE)

        /** The selected server, 1..[SERVERS]. */
        fun active(ctx: Context): Int = prefs(ctx).getInt(K_ACTIVE, 1).coerceIn(1, SERVERS)

        fun setActive(ctx: Context, server: Int) {
            prefs(ctx).edit().putInt(K_ACTIVE, server.coerceIn(1, SERVERS)).apply()
        }

        fun load(ctx: Context, server: Int): Settings {
            val sp = prefs(ctx)
            migrate(ctx)
            val p = "p${server}_"
            var id = sp.getString(p + K_ID, null)
            if (id.isNullOrEmpty()) {
                // the same device on every server: start from server 1's ID
                id = if (server == 1) generateDeviceId() else load(ctx, 1).deviceId
                sp.edit().putString(p + K_ID, id).apply()
            }
            return Settings(
                name = sp.getString(p + K_NAME, "") ?: "",
                host = sp.getString(p + K_HOST, "") ?: "",
                port = sp.getInt(p + K_PORT, 5000),
                deviceId = id,
                password = sp.getString(p + K_PASSWORD, "") ?: "",
                cameras = (1..CAMERAS).map {
                    Camera(sp.getString("${p}cam${it}_name", "") ?: "", sp.getString("${p}cam${it}_ip", "") ?: "")
                },
                rtsp = sp.getBoolean(p + K_RTSP, true),
                http = sp.getBoolean(p + K_HTTP, true),
                ssh = sp.getBoolean(p + K_SSH, false),
                socks = if (sp.contains(p + K_SOCKS_ON)) sp.getBoolean(p + K_SOCKS_ON, true)
                        else sp.getInt(p + K_SOCKS, 1080) != 0,
                extraForwards = (sp.getString(p + K_FORWARDS, "") ?: "").lines().map { it.trim() }.filter { it.isNotEmpty() },
            )
        }

        /** Single-server settings of 1.03 and earlier become server 1. */
        private fun migrate(ctx: Context) {
            val sp = prefs(ctx)
            if (!sp.contains(K_HOST) && !sp.contains(K_ID)) return
            val e = sp.edit()
            if (!sp.contains("p1_$K_ID")) {
                sp.getString(K_HOST, null)?.let { e.putString("p1_$K_HOST", it) }
                if (sp.contains(K_PORT)) e.putInt("p1_$K_PORT", sp.getInt(K_PORT, 5000))
                sp.getString(K_ID, null)?.let { e.putString("p1_$K_ID", it) }
                sp.getString(K_PASSWORD, null)?.let { e.putString("p1_$K_PASSWORD", it) }
                if (sp.contains(K_SOCKS)) e.putInt("p1_$K_SOCKS", sp.getInt(K_SOCKS, 1080))
                sp.getString(K_FORWARDS, null)?.let { e.putString("p1_$K_FORWARDS", it) }
            }
            e.remove(K_HOST).remove(K_PORT).remove(K_ID).remove(K_PASSWORD).remove(K_SOCKS).remove(K_FORWARDS)
            e.commit()
        }

        private val IPV4 = Regex("^((25[0-5]|2[0-4]\\d|1?\\d?\\d)\\.){3}(25[0-5]|2[0-4]\\d|1?\\d?\\d)$")
        fun isIpv4(s: String) = IPV4.matches(s)

        /**
         * Random locally administered unicast MAC as a device ID: bit 1 of the
         * first byte set, bit 0 clear, so the second character is 2, 6, A or E.
         */
        fun generateDeviceId(): String {
            val b = ByteArray(6).also { SecureRandom().nextBytes(it) }
            b[0] = ((b[0].toInt() and 0xFC) or 0x02).toByte()
            return b.joinToString("") { "%02X".format(it.toInt() and 0xFF) }
        }
    }
}
