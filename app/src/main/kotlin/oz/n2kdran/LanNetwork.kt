package oz.n2kdran

import android.content.Context
import android.net.ConnectivityManager
import android.net.Network
import android.net.NetworkCapabilities
import android.net.NetworkRequest
import java.net.DatagramSocket
import java.net.InetAddress
import java.net.Socket

/**
 * Réseau Wi-Fi du bord. À bord, le Wi-Fi n'a souvent pas d'accès Internet :
 * Android envoie alors le trafic des applications par la 4G (téléphone), et les
 * adresses du réseau local deviennent injoignables. Les connexions vers une
 * adresse privée sont donc forcées sur le Wi-Fi ; les adresses publiques suivent
 * le réseau par défaut.
 */
object LanNetwork {
    @Volatile private var wifi: Network? = null
    private var started = false

    fun start(context: Context) {
        if (started) return
        started = true
        val cm = context.applicationContext.getSystemService(ConnectivityManager::class.java)
        val req = NetworkRequest.Builder()
            .addTransportType(NetworkCapabilities.TRANSPORT_WIFI)
            .removeCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)   // Wi-Fi sans Internet accepté
            .build()
        cm.registerNetworkCallback(req, object : ConnectivityManager.NetworkCallback() {
            override fun onAvailable(network: Network) { wifi = network }
            override fun onLost(network: Network) { if (wifi == network) wifi = null }
        })
    }

    /** Adresse du réseau local (privée, lien local, boucle locale). */
    private fun isLan(a: InetAddress) = a.isSiteLocalAddress || a.isLinkLocalAddress || a.isLoopbackAddress

    /** Force une socket TCP sur le Wi-Fi si `host` est sur le réseau local. */
    fun bind(socket: Socket, host: String) {
        val net = wifi ?: return
        val addr = try { InetAddress.getByName(host) } catch (e: Exception) { return }
        if (isLan(addr) && !addr.isLoopbackAddress) net.bindSocket(socket)
    }

    /** Rattache une socket UDP (écoute de la diffusion locale) au Wi-Fi. */
    fun bind(socket: DatagramSocket) {
        wifi?.bindSocket(socket)
    }
}
