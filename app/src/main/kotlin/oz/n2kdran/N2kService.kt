package oz.n2kdran

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.IBinder

/**
 * Service de premier plan : garde le moteur (réception, calculs, historiques)
 * actif quand l'écran n'est pas affiché. Type « connectedDevice » (pas de limite
 * de durée, contrairement à « dataSync »). Notification permanente avec un
 * bouton « Arrêter » pour couper réellement n2kdran.
 */
class N2kService : Service() {

    override fun onCreate() {
        super.onCreate()
        val nm = getSystemService(NotificationManager::class.java)
        nm.createNotificationChannel(
            NotificationChannel(CHANNEL, "n2kdran actif", NotificationManager.IMPORTANCE_LOW).apply {
                description = "Réception des instruments en arrière-plan"
            })
        startForeground(NOTIF_ID, notification(), ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE)
        Engine.start(this)
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (intent?.action == ACTION_STOP) {
            Engine.stop()
            stopForeground(STOP_FOREGROUND_REMOVE)
            stopSelf()
            return START_NOT_STICKY
        }
        return START_STICKY
    }

    override fun onDestroy() {
        if (Engine.running) Engine.stop()
        super.onDestroy()
    }

    override fun onBind(intent: Intent?): IBinder? = null

    private fun notification(): Notification {
        val open = PendingIntent.getActivity(
            this, 0, Intent(this, MainActivity::class.java)
                .addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP or Intent.FLAG_ACTIVITY_REORDER_TO_FRONT),
            PendingIntent.FLAG_IMMUTABLE)
        val stop = PendingIntent.getService(
            this, 1, Intent(this, N2kService::class.java).setAction(ACTION_STOP),
            PendingIntent.FLAG_IMMUTABLE)
        return Notification.Builder(this, CHANNEL)
            .setSmallIcon(android.R.drawable.ic_menu_compass)
            .setContentTitle("n2kdran actif")
            .setContentText("Réception des instruments et historiques en cours")
            .setContentIntent(open)
            .setOngoing(true)
            .addAction(Notification.Action.Builder(null, "Arrêter", stop).build())
            .build()
    }

    companion object {
        private const val CHANNEL = "n2kdran"
        private const val NOTIF_ID = 1
        const val ACTION_STOP = "oz.n2kdran.STOP"
    }
}
