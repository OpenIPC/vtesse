package org.openipc.vtesse

import android.content.Context
import java.security.SecureRandom

/** Connection settings, kept in the main process (SharedPreferences). */
data class Settings(
    val host: String,
    val port: Int,
    val deviceId: String,
    val password: String,
    val socksPort: Int,
    val forwards: List<String>,
) {
    /** key=value text for [Native.start] (same keys as the vtesse-cli config). */
    fun toConfig(): String = buildString {
        append("host=").append(host).append('\n')
        append("port=").append(port).append('\n')
        append("session=").append(deviceId).append('\n')
        append("password=").append(password).append('\n')
        append("socks_port=").append(socksPort).append('\n')
        forwards.forEach { append("forward=").append(it).append('\n') }
    }

    fun save(ctx: Context) {
        prefs(ctx).edit()
            .putString(K_HOST, host)
            .putInt(K_PORT, port)
            .putString(K_ID, deviceId)
            .putString(K_PASSWORD, password)
            .putInt(K_SOCKS, socksPort)
            .putString(K_FORWARDS, forwards.joinToString("\n"))
            .apply()
    }

    companion object {
        private const val K_HOST = "host"
        private const val K_PORT = "port"
        private const val K_ID = "device_id"
        private const val K_PASSWORD = "password"
        private const val K_SOCKS = "socks_port"
        private const val K_FORWARDS = "forwards"

        private fun prefs(ctx: Context) = ctx.getSharedPreferences("vtesse", Context.MODE_PRIVATE)

        fun load(ctx: Context): Settings {
            val p = prefs(ctx)
            var id = p.getString(K_ID, null)
            if (id.isNullOrEmpty()) {
                id = generateDeviceId()                 // once, then persisted
                p.edit().putString(K_ID, id).apply()
            }
            return Settings(
                host = p.getString(K_HOST, "") ?: "",
                port = p.getInt(K_PORT, 5000),
                deviceId = id,
                password = p.getString(K_PASSWORD, "") ?: "",
                socksPort = p.getInt(K_SOCKS, 1080),
                forwards = (p.getString(K_FORWARDS, "") ?: "").lines().map { it.trim() }.filter { it.isNotEmpty() },
            )
        }

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
