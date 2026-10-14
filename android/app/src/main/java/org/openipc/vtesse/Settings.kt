package org.openipc.vtesse

import android.content.Context
import java.security.SecureRandom

/**
 * Settings of one server profile (#26), kept in the main process
 * (SharedPreferences, keys prefixed "p<N>_"). Up to [PROFILES] profiles.
 */
data class Settings(
    val name: String,
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

    /** "2 · Office" or "2 · vtun.example.org" for buttons and the notification. */
    fun label(profile: Int): String {
        val what = name.ifBlank { host }
        return if (what.isBlank()) "$profile" else "$profile · $what"
    }

    fun save(ctx: Context, profile: Int) {
        val p = "p${profile}_"
        prefs(ctx).edit()
            .putString(p + K_NAME, name)
            .putString(p + K_HOST, host)
            .putInt(p + K_PORT, port)
            .putString(p + K_ID, deviceId)
            .putString(p + K_PASSWORD, password)
            .putInt(p + K_SOCKS, socksPort)
            .putString(p + K_FORWARDS, forwards.joinToString("\n"))
            .apply()
    }

    companion object {
        const val PROFILES = 3

        private const val K_NAME = "name"
        private const val K_HOST = "host"
        private const val K_PORT = "port"
        private const val K_ID = "device_id"
        private const val K_PASSWORD = "password"
        private const val K_SOCKS = "socks_port"
        private const val K_FORWARDS = "forwards"
        private const val K_ACTIVE = "active_profile"

        private fun prefs(ctx: Context) = ctx.getSharedPreferences("vtesse", Context.MODE_PRIVATE)

        /** The selected profile, 1..[PROFILES]. */
        fun active(ctx: Context): Int = prefs(ctx).getInt(K_ACTIVE, 1).coerceIn(1, PROFILES)

        fun setActive(ctx: Context, profile: Int) {
            prefs(ctx).edit().putInt(K_ACTIVE, profile.coerceIn(1, PROFILES)).apply()
        }

        fun load(ctx: Context, profile: Int): Settings {
            val sp = prefs(ctx)
            migrate(ctx)
            val p = "p${profile}_"
            var id = sp.getString(p + K_ID, null)
            if (id.isNullOrEmpty()) {
                // the same device on every server: start from profile 1's ID
                id = if (profile == 1) generateDeviceId() else load(ctx, 1).deviceId
                sp.edit().putString(p + K_ID, id).apply()
            }
            return Settings(
                name = sp.getString(p + K_NAME, "") ?: "",
                host = sp.getString(p + K_HOST, "") ?: "",
                port = sp.getInt(p + K_PORT, 5000),
                deviceId = id,
                password = sp.getString(p + K_PASSWORD, "") ?: "",
                socksPort = sp.getInt(p + K_SOCKS, 1080),
                forwards = (sp.getString(p + K_FORWARDS, "") ?: "").lines().map { it.trim() }.filter { it.isNotEmpty() },
            )
        }

        /** Single-server settings of 1.03 and earlier become profile 1. */
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
