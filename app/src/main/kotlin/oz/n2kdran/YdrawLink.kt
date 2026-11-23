package oz.n2kdran

import android.os.SystemClock
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import java.io.BufferedReader
import java.io.InputStreamReader
import java.net.InetSocketAddress
import java.net.Socket

/**
 * Connexion TCP au flux YDRAW de n2k-mux (lecture seule), reconnexion automatique.
 * Un flux muet plus de READ_TIMEOUT_MS est considéré comme coupé.
 * Aucune exception ne sort de la boucle : ligne invalide = rejetée par le cœur.
 */
class YdrawLink(
    private val core: N2kCore, private val host: String, private val port: Int,
    private val onConnected: (() -> Unit)? = null,   // appelé à chaque connexion établie
) {

    enum class State { CONNECTING, CONNECTED, DISCONNECTED }

    @Volatile var state = State.DISCONNECTED
        private set
    @Volatile var lastError: String? = null
        private set
    @Volatile var lastLineMs = 0L          // horloge elapsedRealtime
        private set

    private var job: Job? = null

    fun start(scope: CoroutineScope) {
        if (job?.isActive == true) return
        job = scope.launch(Dispatchers.IO) {
            while (isActive) {
                state = State.CONNECTING
                try {
                    Socket().also { LanNetwork.bind(it, host) }.use { s ->
                        s.connect(InetSocketAddress(host, port), CONNECT_TIMEOUT_MS)
                        s.soTimeout = READ_TIMEOUT_MS
                        s.tcpNoDelay = true
                        val r = BufferedReader(InputStreamReader(s.getInputStream(), Charsets.US_ASCII))
                        state = State.CONNECTED
                        lastError = null
                        onConnected?.invoke()
                        while (isActive) {
                            val line = r.readLine() ?: break        // fermé par le serveur
                            if (!isActive) break                    // annulé pendant la lecture
                            val now = SystemClock.elapsedRealtime()
                            lastLineMs = now
                            core.feed(line, now)
                        }
                    }
                } catch (e: kotlinx.coroutines.CancellationException) {
                    throw e
                } catch (e: Exception) {                    // réseau ou autre : on réessaie
                    lastError = e.message ?: e.javaClass.simpleName
                }
                state = State.DISCONNECTED
                if (isActive) delay(RETRY_DELAY_MS)
            }
        }
    }

    fun stop() {
        job?.cancel()
        job = null
    }

    companion object {
        const val CONNECT_TIMEOUT_MS = 3000
        const val READ_TIMEOUT_MS = 5000
        const val RETRY_DELAY_MS = 2000L
    }
}
