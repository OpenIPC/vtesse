package org.openipc.vtesse

import android.Manifest
import android.app.Activity
import android.app.ActivityManager
import android.content.BroadcastReceiver
import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageManager
import android.graphics.Typeface
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.widget.Button
import android.widget.EditText
import android.widget.TextView
import android.widget.Toast

/**
 * Server profiles (#26), settings and status. The tunnel itself runs in
 * [TunnelService] (":tunnel" process).
 */
class MainActivity : Activity() {
    private lateinit var profileButtons: List<Button>
    private lateinit var profileName: EditText
    private lateinit var host: EditText
    private lateinit var port: EditText
    private lateinit var deviceId: EditText
    private lateinit var password: EditText
    private lateinit var socksPort: EditText
    private lateinit var forwards: EditText
    private lateinit var toggle: Button
    private lateinit var state: TextView
    private lateinit var details: TextView

    private val handler = Handler(Looper.getMainLooper())
    private var profile = 1             // shown in the form
    private var pendingSwitch = 0       // profile to connect once the tunnel has stopped
    private var running = false
    private var lastStatusAt = 0L

    private val receiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            intent.getStringExtra(TunnelService.EXTRA_STATUS)?.let { show(Status.parse(it)) }
        }
    }

    /** No status for a while: the service is not running. */
    private val staleCheck = object : Runnable {
        override fun run() {
            if (running && System.currentTimeMillis() - lastStatusAt > 3000) show(Status.parse("state=stopped\n"))
            handler.postDelayed(this, 1000)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)
        profileButtons = listOf(R.id.profile_1, R.id.profile_2, R.id.profile_3).map { findViewById(it) }
        profileName = findViewById(R.id.profile_name)
        host = findViewById(R.id.host)
        port = findViewById(R.id.port)
        deviceId = findViewById(R.id.device_id)
        password = findViewById(R.id.password)
        socksPort = findViewById(R.id.socks_port)
        forwards = findViewById(R.id.forwards)
        toggle = findViewById(R.id.toggle)
        state = findViewById(R.id.state)
        details = findViewById(R.id.details)
        title = "${getString(R.string.app_name)} ${Native.version()}"

        profile = Settings.active(this)
        loadForm(profile)
        profileButtons.forEachIndexed { i, b -> b.setOnClickListener { selectProfile(i + 1) } }

        findViewById<Button>(R.id.generate).setOnClickListener {
            deviceId.setText(Settings.generateDeviceId())
            Toast.makeText(this, R.string.id_generated, Toast.LENGTH_LONG).show()
        }
        deviceId.setOnLongClickListener {
            (getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager)
                .setPrimaryClip(ClipData.newPlainText("device ID", deviceId.text))
            Toast.makeText(this, R.string.id_copied, Toast.LENGTH_SHORT).show()
            true
        }
        toggle.setOnClickListener { if (running) TunnelService.stop(this) else connect() }
        show(Status.parse("state=stopped\n"))

        if (Build.VERSION.SDK_INT >= 33 &&
            checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED
        ) requestPermissions(arrayOf(Manifest.permission.POST_NOTIFICATIONS), 1)
    }

    override fun onStart() {
        super.onStart()
        val filter = IntentFilter(TunnelService.ACTION_STATUS)
        if (Build.VERSION.SDK_INT >= 33) registerReceiver(receiver, filter, Context.RECEIVER_NOT_EXPORTED)
        else registerReceiver(receiver, filter)
        handler.post(staleCheck)
    }

    override fun onStop() {
        unregisterReceiver(receiver)
        handler.removeCallbacks(staleCheck)
        super.onStop()
    }

    override fun onPause() {
        if (!running) formSettings()?.save(this, profile)   // keep edits of the shown profile
        super.onPause()
    }

    // ---- profiles ----

    private fun loadForm(n: Int) {
        val s = Settings.load(this, n)
        profileName.setText(s.name)
        host.setText(s.host)
        port.setText(s.port.toString())
        deviceId.setText(s.deviceId)
        password.setText(s.password)
        socksPort.setText(s.socksPort.toString())
        forwards.setText(s.forwards.joinToString("\n"))
        refreshProfileButtons()
    }

    private fun refreshProfileButtons() {
        profileButtons.forEachIndexed { i, b ->
            val n = i + 1
            b.text = if (n == profile) formSettings()?.label(n) ?: "$n" else Settings.load(this, n).label(n)
            b.setTypeface(null, if (n == profile) Typeface.BOLD else Typeface.NORMAL)
            b.alpha = if (n == profile) 1f else 0.6f
        }
    }

    /** Switch the form to profile [n]; while connected, reconnect with it. */
    private fun selectProfile(n: Int) {
        if (n == profile && pendingSwitch == 0) return
        if (running) {
            pendingSwitch = n
            state.text = getString(R.string.switching, n.toString())
            TunnelService.stop(this)
            return
        }
        formSettings()?.save(this, profile)
        profile = n
        Settings.setActive(this, n)
        loadForm(n)
    }

    // ---- connect ----

    /** The form as settings, lenient (for saving drafts); null if nothing sensible to save. */
    private fun formSettings(): Settings? {
        if (!::host.isInitialized) return null
        return Settings(
            name = profileName.text.toString().trim(),
            host = host.text.toString().trim(),
            port = port.text.toString().toIntOrNull() ?: 5000,
            deviceId = deviceId.text.toString().trim().uppercase(),
            password = password.text.toString(),
            socksPort = socksPort.text.toString().ifBlank { "0" }.toIntOrNull() ?: 0,
            forwards = forwards.text.toString().lines().map { it.trim() }.filter { it.isNotEmpty() },
        )
    }

    private fun connect() {
        val id = deviceId.text.toString().trim().uppercase()
        Native.checkDeviceId(id)?.let { deviceId.error = it; return }
        val p = port.text.toString().toIntOrNull()
        if (p == null || p !in 1..65535) { port.error = getString(R.string.bad_port); return }
        val sp = socksPort.text.toString().ifBlank { "0" }.toIntOrNull()
        if (sp == null || sp !in 0..65535) { socksPort.error = getString(R.string.bad_port); return }
        val s = formSettings()!!.copy(deviceId = id, port = p, socksPort = sp)
        s.save(this, profile)
        Settings.setActive(this, profile)
        refreshProfileButtons()
        show(Status.parse("state=connecting\n"))
        // the previous tunnel process must be gone: lwIP starts once per process
        whenTunnelProcessGone { TunnelService.start(this, s.toConfig(), s.label(profile)) }
    }

    /** Runs [action] once the previous tunnel process has ended (up to 30 s). */
    private fun whenTunnelProcessGone(action: () -> Unit) {
        val am = getSystemService(Context.ACTIVITY_SERVICE) as ActivityManager
        val name = TunnelService.processName(this)
        val deadline = System.currentTimeMillis() + 30000
        val check = object : Runnable {
            override fun run() {
                val alive = am.runningAppProcesses?.any { it.processName == name } == true
                when {
                    !alive -> action()
                    System.currentTimeMillis() < deadline -> {
                        state.text = getString(R.string.waiting_previous)
                        lastStatusAt = System.currentTimeMillis()   // not stale while waiting
                        handler.postDelayed(this, 200)
                    }
                    else -> show(Status.parse("state=failed\nerror=${getString(R.string.err_previous_closing)}\n"))
                }
            }
        }
        check.run()
    }

    // ---- status ----

    private fun show(st: Status) {
        lastStatusAt = System.currentTimeMillis()
        running = st.running
        if (!running && pendingSwitch != 0) {
            val n = pendingSwitch
            pendingSwitch = 0
            profile = n
            Settings.setActive(this, n)
            loadForm(n)
            connect()
            return
        }
        state.text = if (pendingSwitch != 0) getString(R.string.switching, pendingSwitch.toString()) else st.summary(this)
        toggle.setText(if (running) R.string.stop else R.string.start)
        listOf(profileName, host, port, deviceId, password, socksPort, forwards).forEach { it.isEnabled = !running }
        details.text = buildString {
            if (st.bound) {
                append("address ").append(st.addr).append(" / ").append(st.mask).append('\n')
                st.routes.forEach { append("route ").append(it).append('\n') }
            }
            st.flags?.let { append("flags ").append(it).append('\n') }
            if (running) append("connections ").append(st.active)
                .append(", up ").append(st.bytesUp).append(" B, down ").append(st.bytesDown).append(" B\n")
            if (st.log.isNotEmpty()) {
                append('\n')
                st.log.forEach { append(it).append('\n') }
            }
        }
    }
}
