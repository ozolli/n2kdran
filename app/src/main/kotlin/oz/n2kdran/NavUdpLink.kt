package oz.n2kdran

import android.content.Context
import android.net.wifi.WifiManager
import android.os.SystemClock
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.SocketTimeoutException

/**
 * Écoute UDP du flux de navigation de qtVlm (trames N2K au format passerelle,
 * diffusées sur le réseau). Seules 129283/129284 sont retenues par le cœur.
 * Un datagramme peut contenir une ou plusieurs lignes.
 * Relais (relayPort > 0) : chaque datagramme reçu est renvoyé tel quel vers
 * 127.0.0.1:relayPort, pour qtVlm sur le même appareil (un datagramme adressé à
 * l'appareil n'est remis qu'à un seul des programmes qui écoutent le même port).
 */
class NavUdpLink(private val context: Context, private val core: N2kCore, private val port: Int,
                 private val relayPort: Int = 0) {

    @Volatile var lastPacketMs = 0L          // horloge elapsedRealtime
        private set
    @Volatile var packets = 0L               // datagrammes reçus (mesure des pertes)
        private set
    @Volatile var lastError: String? = null
        private set
    @Volatile var relayed = 0L               // datagrammes renvoyés vers 127.0.0.1:relayPort
        private set

    private var job: Job? = null

    fun start(scope: CoroutineScope) {
        if (job?.isActive == true) return
        job = scope.launch(Dispatchers.IO) {
            // Certaines puces Wi-Fi filtrent la diffusion en économie d'énergie.
            val wifi = context.applicationContext.getSystemService(Context.WIFI_SERVICE) as WifiManager
            val lock = wifi.createMulticastLock("n2kdran-nav").apply { setReferenceCounted(false) }
            lock.acquire()
            try {
                while (isActive) {
                    try {
                        // Socket de relais séparée : la socket d'écoute est liée au Wi-Fi.
                        val relay = if (relayPort > 0) DatagramSocket() else null
                        // 127.0.0.1 explicite : getLoopbackAddress() peut renvoyer ::1 (constaté sous
                        // Android 17), que qtVlm, à l'écoute en IPv4, ne reçoit pas.
                        val local = InetAddress.getByAddress(byteArrayOf(127, 0, 0, 1))
                        relay.use { DatagramSocket(null).use { s ->
                            LanNetwork.bind(s)
                            s.reuseAddress = true
                            s.broadcast = true
                            s.bind(InetSocketAddress(port))
                            s.soTimeout = 1000
                            val buf = ByteArray(2048)
                            val pkt = DatagramPacket(buf, buf.size)
                            lastError = null
                            while (isActive) {
                                try {
                                    s.receive(pkt)
                                } catch (e: SocketTimeoutException) {
                                    continue
                                }
                                if (!isActive) break                // annulé pendant la réception
                                val now = SystemClock.elapsedRealtime()
                                lastPacketMs = now
                                packets++
                                val lines = String(buf, 0, pkt.length, Charsets.US_ASCII)
                                    .split('\n').map { it.trim() }.filter { it.isNotEmpty() }
                                lines.forEach { core.feed(it, now, navOnly = true) }
                                relay?.let {
                                    try { it.send(DatagramPacket(buf, pkt.length, local, relayPort)); relayed++ }
                                    catch (e: Exception) { lastError = "relais : ${e.message}" }
                                }
                            }
                        } }
                    } catch (e: kotlinx.coroutines.CancellationException) {
                        throw e
                    } catch (e: Exception) {                // réseau ou autre : on réessaie
                        lastError = e.message ?: e.javaClass.simpleName
                        delay(2000)
                    }
                }
            } finally {
                lock.release()
            }
        }
    }

    fun stop() {
        job?.cancel()
        job = null
    }
}
