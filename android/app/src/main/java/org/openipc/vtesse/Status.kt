package org.openipc.vtesse

import android.content.Context

/** Parsed [Native.status] snapshot. */
class Status(
    val state: String,
    val bound: Boolean,
    val addr: String?,
    val mask: String?,
    val routes: List<String>,
    val flags: String?,
    val error: String?,
    val active: Long,
    val bytesUp: Long,
    val bytesDown: Long,
    val log: List<String>,
) {
    val running get() = state != "stopped" && state != "failed" && state != "idle"

    /** One line for the notification and the status header. */
    fun summary(ctx: Context): String = when {
        state == "up" && bound -> ctx.getString(R.string.state_up_addr, addr)
        state == "up" -> ctx.getString(R.string.state_up_dhcp)
        state == "connecting" -> ctx.getString(R.string.state_connecting)
        state == "down" -> ctx.getString(R.string.state_down, error ?: "")
        state == "failed" -> ctx.getString(R.string.state_failed, error ?: "")
        else -> ctx.getString(R.string.state_stopped)
    }

    companion object {
        fun parse(text: String): Status {
            val single = HashMap<String, String>()
            val routes = ArrayList<String>()
            val log = ArrayList<String>()
            text.lineSequence().forEach { line ->
                val eq = line.indexOf('=')
                if (eq <= 0) return@forEach
                val k = line.substring(0, eq)
                val v = line.substring(eq + 1)
                when (k) {
                    "route" -> routes += v
                    "log" -> log += v
                    else -> single[k] = v
                }
            }
            return Status(
                state = single["state"] ?: "idle",
                bound = single["bound"] == "1",
                addr = single["addr"],
                mask = single["mask"],
                routes = routes,
                flags = single["flags"],
                error = single["error"],
                active = single["active"]?.toLongOrNull() ?: 0,
                bytesUp = single["up"]?.toLongOrNull() ?: 0,
                bytesDown = single["down"]?.toLongOrNull() ?: 0,
                log = log,
            )
        }
    }
}
