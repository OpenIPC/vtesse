package org.openipc.vtesse

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.net.ConnectivityManager
import android.net.Network
import android.net.NetworkCapabilities
import android.net.NetworkRequest
import android.os.Build
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.os.PowerManager
import android.os.Process

/**
 * Foreground service that runs the tunnel (no VpnService). It lives in its own
 * process (":tunnel", see the manifest): the native stack can be started once
 * per process, so stopping ends the process. Status goes to the UI as a
 * package-local broadcast once a second.
 */
class TunnelService : Service() {
    private val handler = Handler(Looper.getMainLooper())
    private var running = false
    private var wakeLock: PowerManager.WakeLock? = null
    private var netCallback: ConnectivityManager.NetworkCallback? = null
    private var label = ""      // active profile, for the notification title

    private val tick = object : Runnable {
        override fun run() {
            if (!running) return
            publish(Native.status())
            handler.postDelayed(this, 1000)
        }
    }

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            ACTION_STOP -> shutdown()
            ACTION_START -> if (!running) {
                if (sessionUsed) {
                    // this process is about to end (lwIP starts once per process)
                    publish("state=failed\nerror=${getString(R.string.err_previous_closing)}\n")
                } else {
                    label = intent.getStringExtra(EXTRA_LABEL) ?: ""
                    start(intent.getStringExtra(EXTRA_CONFIG) ?: "")
                }
            }
        }
        return START_REDELIVER_INTENT
    }

    private fun start(config: String) {
        startInForeground(getString(R.string.notify_connecting))
        sessionUsed = true
        val err = Native.start(config)
        if (err != null) {
            publish("state=failed\nerror=$err\n")
            shutdown()
            return
        }
        running = true
        wakeLock = (getSystemService(Context.POWER_SERVICE) as PowerManager)
            .newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "vtesse:tunnel").apply { acquire() }
        watchNetwork()
        handler.post(tick)
    }

    private fun shutdown() {
        handler.removeCallbacks(tick)
        if (running) {
            running = false
            stopNative()
        }
        netCallback?.let {
            runCatching { connectivity().unregisterNetworkCallback(it) }
            netCallback = null
        }
        wakeLock?.let { if (it.isHeld) it.release() }
        wakeLock = null
        publish("state=stopped\n")
        stopForegroundCompat()
        stopSelf()
        // lwIP can be initialised once per process: start the next session fresh
        handler.postDelayed({ Process.killProcess(Process.myPid()) }, 300)
    }

    override fun onDestroy() {
        if (running) {
            running = false
            stopNative()
        }
        super.onDestroy()
    }

    /** Native.stop() waits for the network thread; never block the main thread long
     *  for it: the process ends right after anyway. */
    private fun stopNative() {
        val t = Thread { Native.stop() }
        t.start()
        t.join(2000)
    }

    private fun publish(status: String) {
        sendBroadcast(Intent(ACTION_STATUS).setPackage(packageName).putExtra(EXTRA_STATUS, status))
        if (running) {
            val s = Status.parse(status)
            getSystemService(NotificationManager::class.java)
                ?.notify(NOTIFY_ID, notification(s.summary(this)))
        }
    }

    // ---- network changes: reconnect at once instead of waiting for keepalive ----

    private fun connectivity() = getSystemService(Context.CONNECTIVITY_SERVICE) as ConnectivityManager

    private fun watchNetwork() {
        val cb = object : ConnectivityManager.NetworkCallback() {
            private var last: Network? = null
            private var lost = false

            override fun onAvailable(network: Network) {
                handler.post {
                    if (running && last != null && (network != last || lost)) Native.networkChanged()
                    last = network
                    lost = false
                }
            }

            override fun onLost(network: Network) {
                handler.post { if (network == last) lost = true }
            }
        }
        if (Build.VERSION.SDK_INT >= 24) {
            connectivity().registerDefaultNetworkCallback(cb)
        } else {
            val req = NetworkRequest.Builder().addCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET).build()
            connectivity().registerNetworkCallback(req, cb)
        }
        netCallback = cb
    }

    // ---- notification ----

    private fun startInForeground(text: String) {
        val n = notification(text)
        if (Build.VERSION.SDK_INT >= 34)
            startForeground(NOTIFY_ID, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE)
        else
            startForeground(NOTIFY_ID, n)
    }

    @Suppress("DEPRECATION")
    private fun stopForegroundCompat() {
        if (Build.VERSION.SDK_INT >= 24) stopForeground(STOP_FOREGROUND_REMOVE) else stopForeground(true)
    }

    @Suppress("DEPRECATION")
    private fun notification(text: String): Notification {
        val piFlags = if (Build.VERSION.SDK_INT >= 23) PendingIntent.FLAG_IMMUTABLE else 0
        val open = PendingIntent.getActivity(this, 0, Intent(this, MainActivity::class.java), piFlags)
        val stop = PendingIntent.getService(
            this, 1, Intent(this, TunnelService::class.java).setAction(ACTION_STOP), piFlags
        )
        val b = if (Build.VERSION.SDK_INT >= 26) {
            val nm = getSystemService(NotificationManager::class.java)
            if (nm.getNotificationChannel(CHANNEL) == null)
                nm.createNotificationChannel(
                    NotificationChannel(CHANNEL, getString(R.string.channel_name), NotificationManager.IMPORTANCE_LOW)
                )
            Notification.Builder(this, CHANNEL)
        } else {
            Notification.Builder(this).setPriority(Notification.PRIORITY_LOW)
        }
        return b.setSmallIcon(R.drawable.ic_notify)
            .setContentTitle(if (label.isEmpty()) getString(R.string.app_name) else "${getString(R.string.app_name)} · $label")
            .setContentText(text)
            .setOngoing(true)
            .setContentIntent(open)
            .addAction(Notification.Action.Builder(0, getString(R.string.stop), stop).build())
            .build()
    }

    companion object {
        const val ACTION_START = "org.openipc.vtesse.START"
        const val ACTION_STOP = "org.openipc.vtesse.STOP"
        const val ACTION_STATUS = "org.openipc.vtesse.STATUS"
        const val EXTRA_CONFIG = "config"
        const val EXTRA_LABEL = "label"
        const val EXTRA_STATUS = "status"
        private const val CHANNEL = "tunnel"
        private const val NOTIFY_ID = 1

        /** A session ran in this process: it cannot start another one. */
        @Volatile private var sessionUsed = false

        /** Name of the process the tunnel runs in (manifest: android:process=":tunnel"). */
        fun processName(ctx: Context) = ctx.packageName + ":tunnel"

        fun start(ctx: Context, config: String, label: String) {
            val i = Intent(ctx, TunnelService::class.java).setAction(ACTION_START)
                .putExtra(EXTRA_CONFIG, config).putExtra(EXTRA_LABEL, label)
            if (Build.VERSION.SDK_INT >= 26) ctx.startForegroundService(i) else ctx.startService(i)
        }

        fun stop(ctx: Context) {
            ctx.startService(Intent(ctx, TunnelService::class.java).setAction(ACTION_STOP))
        }
    }
}
