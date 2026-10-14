package org.openipc.vtesse

import android.Manifest
import android.app.Activity
import android.content.BroadcastReceiver
import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.widget.Button
import android.widget.EditText
import android.widget.TextView
import android.widget.Toast

/** Settings and status. The tunnel itself runs in [TunnelService] (":tunnel" process). */
class MainActivity : Activity() {
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

        val s = Settings.load(this)
        host.setText(s.host)
        port.setText(s.port.toString())
        deviceId.setText(s.deviceId)
        password.setText(s.password)
        socksPort.setText(s.socksPort.toString())
        forwards.setText(s.forwards.joinToString("\n"))

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

    private fun connect() {
        val id = deviceId.text.toString().trim().uppercase()
        Native.checkDeviceId(id)?.let { deviceId.error = it; return }
        val p = port.text.toString().toIntOrNull()
        if (p == null || p !in 1..65535) { port.error = getString(R.string.bad_port); return }
        val sp = socksPort.text.toString().ifBlank { "0" }.toIntOrNull()
        if (sp == null || sp !in 0..65535) { socksPort.error = getString(R.string.bad_port); return }
        val s = Settings(
            host = host.text.toString().trim(),
            port = p,
            deviceId = id,
            password = password.text.toString(),
            socksPort = sp,
            forwards = forwards.text.toString().lines().map { it.trim() }.filter { it.isNotEmpty() },
        )
        s.save(this)
        TunnelService.start(this, s.toConfig())
        show(Status.parse("state=connecting\n"))
    }

    private fun show(st: Status) {
        lastStatusAt = System.currentTimeMillis()
        running = st.running
        state.text = st.summary(this)
        toggle.setText(if (running) R.string.stop else R.string.start)
        listOf(host, port, deviceId, password, socksPort, forwards).forEach { it.isEnabled = !running }
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
