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
import android.text.Editable
import android.text.InputType
import android.text.TextWatcher
import android.util.TypedValue
import android.view.Gravity
import android.view.View
import android.widget.Button
import android.widget.CheckBox
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.TextView
import android.widget.Toast

/**
 * Servers (up to 3) with their cameras (up to 8 each), settings and status
 * (#26, #27). The tunnel itself runs in [TunnelService] (":tunnel" process).
 */
class MainActivity : Activity() {
    private class CameraRow(val name: EditText, val ip: EditText, val ports: TextView)

    private lateinit var serverButtons: List<Button>
    private lateinit var serverName: EditText
    private lateinit var host: EditText
    private lateinit var port: EditText
    private lateinit var deviceId: EditText
    private lateinit var password: EditText
    private lateinit var svcRtsp: CheckBox
    private lateinit var svcHttp: CheckBox
    private lateinit var svcSsh: CheckBox
    private lateinit var svcSocks: CheckBox
    private lateinit var svcUdp: CheckBox
    private lateinit var cameraRows: List<CameraRow>
    private lateinit var forwards: EditText
    private lateinit var toggle: Button
    private lateinit var state: TextView
    private lateinit var details: TextView

    private val handler = Handler(Looper.getMainLooper())
    private var server = 1              // shown in the form
    private var pendingSwitch = 0       // server to connect once the tunnel has stopped
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
        serverButtons = listOf(R.id.profile_1, R.id.profile_2, R.id.profile_3).map { findViewById(it) }
        serverName = findViewById(R.id.profile_name)
        host = findViewById(R.id.host)
        port = findViewById(R.id.port)
        deviceId = findViewById(R.id.device_id)
        password = findViewById(R.id.password)
        svcRtsp = findViewById(R.id.svc_rtsp)
        svcHttp = findViewById(R.id.svc_http)
        svcSsh = findViewById(R.id.svc_ssh)
        svcSocks = findViewById(R.id.svc_socks)
        svcUdp = findViewById(R.id.svc_udp)
        forwards = findViewById(R.id.forwards)
        toggle = findViewById(R.id.toggle)
        state = findViewById(R.id.state)
        details = findViewById(R.id.details)
        cameraRows = buildCameraRows(findViewById(R.id.cameras))
        title = "${getString(R.string.app_name)} ${Native.version()}"

        server = Settings.active(this)
        loadForm(server)
        serverButtons.forEachIndexed { i, b -> b.setOnClickListener { selectServer(i + 1) } }
        listOf(svcRtsp, svcHttp, svcSsh, svcSocks).forEach { it.setOnCheckedChangeListener { _, _ -> refreshPorts() } }
        // the server button shows the name (or host) as it is typed
        val relabel = object : TextWatcher {
            override fun beforeTextChanged(s: CharSequence?, a: Int, b: Int, c: Int) {}
            override fun onTextChanged(s: CharSequence?, a: Int, b: Int, c: Int) {}
            override fun afterTextChanged(s: Editable?) { refreshServerButtons() }
        }
        serverName.addTextChangedListener(relabel)
        host.addTextChangedListener(relabel)

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
        if (!running) formSettings().save(this, server)     // keep edits of the shown server
        super.onPause()
    }

    // ---- form ----

    private fun dp(v: Int) = TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v.toFloat(), resources.displayMetrics).toInt()

    /** Camera rows: number, name, IP, local ports. */
    private fun buildCameraRows(parent: LinearLayout): List<CameraRow> = (1..Settings.CAMERAS).map { n ->
        val row = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
        }
        val num = TextView(this).apply {
            text = "$n"
            setTypeface(null, Typeface.BOLD)
            width = dp(28)
        }
        val name = EditText(this).apply {
            hint = getString(R.string.camera_name_hint, n)
            contentDescription = getString(R.string.camera_name_desc, n)
            inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_CAP_SENTENCES
            setSingleLine()
        }
        val ip = EditText(this).apply {
            hint = getString(R.string.camera_ip_hint)
            contentDescription = getString(R.string.camera_ip_desc, n)
            inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS
            typeface = Typeface.MONOSPACE
            setSingleLine()
            setOnFocusChangeListener { _, _ -> refreshPorts() }
        }
        val ports = TextView(this).apply {
            typeface = Typeface.MONOSPACE
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 12f)
            setPadding(dp(8), 0, 0, 0)
        }
        row.addView(num)
        row.addView(name, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 2f))
        row.addView(ip, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 2f))
        row.addView(ports, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 3f))
        parent.addView(row)
        CameraRow(name, ip, ports)
    }

    private fun loadForm(n: Int) {
        val s = Settings.load(this, n)
        serverName.setText(s.name)
        host.setText(s.host)
        port.setText(s.port.toString())
        deviceId.setText(s.deviceId)
        password.setText(s.password)
        svcRtsp.isChecked = s.rtsp
        svcHttp.isChecked = s.http
        svcSsh.isChecked = s.ssh
        svcSocks.isChecked = s.socks
        svcUdp.isChecked = s.udp
        s.cameras.forEachIndexed { i, c ->
            cameraRows[i].name.setText(c.name)
            cameraRows[i].ip.setText(c.ip)
        }
        forwards.setText(s.extraForwards.joinToString("\n"))
        refreshServerButtons()
        refreshPorts()
    }

    /** Service labels and the local ports of every camera for the shown server. */
    private fun refreshPorts() {
        svcRtsp.text = "RTSP ${server}55N"
        svcHttp.text = "HTTP ${server}80N"
        svcSsh.text = "SSH ${server}22N"
        svcSocks.text = getString(R.string.svc_socks, Settings.socksPort(server))
        svcUdp.text = getString(R.string.svc_udp, Settings.udpPort(server))
        cameraRows.forEachIndexed { i, r ->
            val n = i + 1
            val list = buildList {
                if (svcRtsp.isChecked) add("rtsp ${Settings.port(server, Settings.RTSP, n)}")
                if (svcHttp.isChecked) add("http ${Settings.port(server, Settings.HTTP, n)}")
                if (svcSsh.isChecked) add("ssh ${Settings.port(server, Settings.SSH, n)}")
            }
            r.ports.text = list.joinToString(" · ")
            r.ports.alpha = if (r.ip.text.isBlank()) 0.35f else 1f
        }
    }

    private fun refreshServerButtons() {
        serverButtons.forEachIndexed { i, b ->
            val n = i + 1
            b.text = if (n == server) formSettings().label(this, n) else Settings.load(this, n).label(this, n)
            b.setTypeface(null, if (n == server) Typeface.BOLD else Typeface.NORMAL)
            b.alpha = if (n == server) 1f else 0.6f
        }
    }

    /** Switch the form to server [n]; while connected, reconnect with it. */
    private fun selectServer(n: Int) {
        if (n == server && pendingSwitch == 0) return
        if (running) {
            pendingSwitch = n
            state.text = getString(R.string.switching, n.toString())
            TunnelService.stop(this)
            return
        }
        formSettings().save(this, server)
        server = n
        Settings.setActive(this, n)
        loadForm(n)
    }

    /** The form as settings, lenient (drafts are saved as typed). */
    private fun formSettings(): Settings = Settings(
        name = serverName.text.toString().trim(),
        host = host.text.toString().trim(),
        port = port.text.toString().toIntOrNull() ?: 5000,
        deviceId = deviceId.text.toString().trim().uppercase(),
        password = password.text.toString(),
        cameras = cameraRows.map { Camera(it.name.text.toString().trim(), it.ip.text.toString().trim()) },
        rtsp = svcRtsp.isChecked,
        http = svcHttp.isChecked,
        ssh = svcSsh.isChecked,
        socks = svcSocks.isChecked,
        udp = svcUdp.isChecked,
        extraForwards = forwards.text.toString().lines().map { it.trim() }.filter { it.isNotEmpty() },
    )

    // ---- connect ----

    private fun connect() {
        val id = deviceId.text.toString().trim().uppercase()
        Native.checkDeviceId(id)?.let { deviceId.error = it; return }
        val p = port.text.toString().toIntOrNull()
        if (p == null || p !in 1..65535) { port.error = getString(R.string.bad_port); return }
        cameraRows.forEach { r ->
            val ip = r.ip.text.toString().trim()
            if (ip.isNotEmpty() && !Settings.isIpv4(ip)) { r.ip.error = getString(R.string.bad_ip); r.ip.requestFocus(); return }
        }
        val s = formSettings().copy(deviceId = id, port = p)
        s.save(this, server)
        Settings.setActive(this, server)
        refreshServerButtons()
        show(Status.parse("state=connecting\n"))
        val n = server
        // the previous tunnel process must be gone: lwIP starts once per process
        whenTunnelProcessGone { TunnelService.start(this, s.toConfig(n), s.label(this, n)) }
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
            server = n
            Settings.setActive(this, n)
            loadForm(n)
            if (host.text.isNotBlank()) {       // an empty server: just disconnect
                connect()
                return
            }
        }
        state.text = if (pendingSwitch != 0) getString(R.string.switching, pendingSwitch.toString()) else st.summary(this)
        toggle.setText(if (running) R.string.stop else R.string.start)
        val editable = listOf<View>(serverName, host, port, deviceId, password, svcRtsp, svcHttp, svcSsh, svcSocks, svcUdp, forwards) +
            cameraRows.flatMap { listOf(it.name, it.ip) }
        editable.forEach { it.isEnabled = !running }
        details.text = buildString {
            if (st.bound) {
                append("address ").append(st.addr).append(" / ").append(st.mask).append('\n')
                st.routes.forEach { append("route ").append(it).append('\n') }
            }
            st.flags?.let { append("flags ").append(it).append('\n') }
            st.udp.forEach {
                append(getString(R.string.udp_hint, st.addr ?: "?", it.port, it.local, it.datagrams, it.dropped)).append('\n')
            }
            if (running) append("connections ").append(st.active)
                .append(", up ").append(st.bytesUp).append(" B, down ").append(st.bytesDown).append(" B\n")
            if (st.log.isNotEmpty()) {
                append('\n')
                st.log.forEach { append(it).append('\n') }
            }
        }
    }
}
