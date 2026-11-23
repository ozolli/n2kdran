package oz.n2kdran

import android.app.Activity
import android.content.Intent
import android.graphics.Color
import android.graphics.Typeface
import android.net.Uri
import android.os.Bundle
import android.provider.OpenableColumns
import android.text.InputType
import android.view.Gravity
import android.view.View
import android.view.WindowManager
import android.widget.Button
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.RadioButton
import android.widget.RadioGroup
import android.widget.ScrollView
import android.widget.TextView

/** Réglages : source N2K, navigation qtVlm, polaire, délai de péremption. */
class SettingsActivity : Activity() {

    private lateinit var s: Settings
    private lateinit var host: EditText
    private lateinit var port: EditText
    private lateinit var navPort: EditText
    private lateinit var navRelay: EditText
    private lateinit var tdField: EditText
    private lateinit var cpaField: EditText
    private lateinit var tcpaField: EditText
    private lateinit var rangeField: EditText
    private lateinit var stale: EditText
    private lateinit var dimField: EditText
    private lateinit var refV: RadioButton
    private lateinit var refM: RadioButton
    private lateinit var refC: RadioButton
    private lateinit var sailRoseBox: android.widget.CheckBox
    private lateinit var aisStillBox: android.widget.CheckBox
    private lateinit var polarLabel: TextView
    private lateinit var sailLabel: TextView
    private lateinit var message: TextView

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        s = Settings(this)

        val col = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(48, 32, 48, 32)
            setBackgroundColor(Color.BLACK)
        }
        fun title(t: String) = col.addView(TextView(this).apply {
            text = t; textSize = 22f; setTextColor(0xFFB0B0B0.toInt())
            typeface = Typeface.DEFAULT_BOLD; setPadding(0, 28, 0, 6)
        })
        fun field(hint: String, value: String, number: Boolean) = EditText(this).apply {
            setText(value); this.hint = hint; textSize = 24f
            setTextColor(Color.WHITE); setHintTextColor(0xFF606060.toInt())
            inputType = if (number) InputType.TYPE_CLASS_NUMBER else InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_URI
        }.also { col.addView(it) }

        col.addView(TextView(this).apply {
            text = "Réglages n2kdran"; textSize = 30f; setTextColor(Color.WHITE)
            typeface = Typeface.DEFAULT_BOLD
        })

        title("Source N2K — adresse")
        host = field(Settings.DEFAULT_HOST, s.host, false)
        title("Source N2K — port du flux YDRAW (TCP)")
        port = field("2700", s.port.toString(), true)
        val recent = s.recentSources
        if (recent.isNotEmpty()) {
            title("Dernières connexions (appui = reprendre)")
            val row = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL }
            for ((h, p) in recent) row.addView(Button(this).apply {
                text = "$h:$p"; textSize = 18f; isAllCaps = false
                setOnClickListener { host.setText(h); port.setText(p.toString()) }
            })
            col.addView(row)
        }

        title("Navigation qtVlm (WP) — port UDP d'écoute")
        navPort = field("2800", s.navPort.toString(), true)
        title("Relais vers qtVlm sur cet appareil — port UDP (vide = aucun)")
        navRelay = field("ex. 2801", if (s.navRelayPort > 0) s.navRelayPort.toString() else "", true)

        title("Cap affiché (rose et cadre du cap)")
        val refGroup = RadioGroup(this).apply { orientation = RadioGroup.HORIZONTAL }
        fun radio(t: String) = RadioButton(this).apply {
            text = t; textSize = 22f; setTextColor(Color.WHITE); id = View.generateViewId()
        }.also { refGroup.addView(it) }
        refV = radio("Vrai (V)"); refM = radio("Magnétique (M)"); refC = radio("COG (C)")
        refGroup.check(when (s.headingRef) { 'M' -> refM.id; 'C' -> refC.id; else -> refV.id })
        col.addView(refGroup)

        title("Polaire")
        polarLabel = TextView(this).apply { textSize = 22f; setTextColor(Color.WHITE) }
        col.addView(polarLabel)
        refreshPolar()
        val row = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL }
        row.addView(Button(this).apply {
            text = "Choisir un fichier…"; textSize = 20f
            setOnClickListener {
                startActivityForResult(Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
                    addCategory(Intent.CATEGORY_OPENABLE); type = "*/*"
                }, PICK_POLAR)
            }
        })
        row.addView(Button(this).apply {
            text = "CM50 par défaut"; textSize = 20f
            setOnClickListener { s.polarName = null; s.polarFile.delete(); refreshPolar() }
        })
        col.addView(row)

        title("Sail chart (fichiers qtVlm .saildef et .sailselect)")
        sailLabel = TextView(this).apply { textSize = 22f; setTextColor(Color.WHITE) }
        col.addView(sailLabel)
        refreshSail()
        val sailRow = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL }
        sailRow.addView(Button(this).apply {
            text = "Choisir les 2 fichiers…"; textSize = 20f
            setOnClickListener {
                startActivityForResult(Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
                    addCategory(Intent.CATEGORY_OPENABLE); type = "*/*"
                    putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true)
                }, PICK_SAIL)
            }
        })
        sailRow.addView(Button(this).apply {
            text = "CM50 par défaut"; textSize = 20f
            setOnClickListener { s.sailName = null; s.saildefFile.delete(); s.sailselectFile.delete(); refreshSail() }
        })
        col.addView(sailRow)
        sailRoseBox = android.widget.CheckBox(this).apply {
            text = "Afficher le sail chart au centre de la rose"
            textSize = 22f; setTextColor(Color.WHITE); isChecked = s.sailRose
        }
        col.addView(sailRoseBox)

        title("Délai de péremption (secondes)")
        stale = field("3", s.staleS.toString(), true)

        title("Immersion du transducteur sous la flottaison (m) — si le sondeur n'envoie pas de décalage vers la surface")
        tdField = field("0", if (s.transducerDepth > 0) s.transducerDepth.toString() else "", true).apply {
            inputType = InputType.TYPE_CLASS_NUMBER or InputType.TYPE_NUMBER_FLAG_DECIMAL
        }
        title("AIS — cible dangereuse si CPA sous (NM)")
        cpaField = field("0.5", s.aisCpaNm.toString(), true).apply {
            inputType = InputType.TYPE_CLASS_NUMBER or InputType.TYPE_NUMBER_FLAG_DECIMAL
        }
        title("AIS — … et TCPA sous (minutes)")
        tcpaField = field("20", s.aisTcpaMin.toString(), true)
        title("AIS — cibles surveillées jusqu'à (NM)")
        rangeField = field("15", s.aisRangeNm.let { if (it % 1.0 == 0.0) it.toInt().toString() else it.toString() }, true).apply {
            inputType = InputType.TYPE_CLASS_NUMBER or InputType.TYPE_NUMBER_FLAG_DECIMAL
        }
        aisStillBox = android.widget.CheckBox(this).apply {
            text = "AIS — pas d'alarme pour une cible immobile (moins de 0,5 nœud)"
            textSize = 22f; setTextColor(Color.WHITE); isChecked = s.aisIgnoreStill
        }
        col.addView(aisStillBox)
        title("Atténuation de nuit (%) — voile noir ajouté à la luminosité minimale")
        dimField = field("60", s.nightDimPct.toString(), true)

        message = TextView(this).apply { textSize = 20f; setTextColor(0xFFFF4136.toInt()); setPadding(0, 20, 0, 0) }
        col.addView(message)

        val buttons = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.END; setPadding(0, 24, 0, 0) }
        buttons.addView(Button(this).apply { text = "Annuler"; textSize = 22f; setOnClickListener { finish() } })
        buttons.addView(Button(this).apply { text = "Enregistrer"; textSize = 22f; setOnClickListener { save() } })
        col.addView(buttons)

        setContentView(ScrollView(this).apply { setBackgroundColor(Color.BLACK); addView(col) })
    }

    private fun refreshSail() {
        sailLabel.text = s.sailName ?: "CM50 (défaut, embarqué)"
    }

    /** Lit les deux fichiers choisis (repérés par leur extension), les vérifie et les garde. */
    private fun loadSail(uris: List<Uri>) {
        var def: String? = null; var sel: String? = null; var base = "sail chart"
        for (u in uris) {
            val n = displayName(u)
            val txt = try { contentResolver.openInputStream(u)?.use { it.readBytes().toString(Charsets.UTF_8) } } catch (e: Exception) { null }
            when {
                n.endsWith(".saildef", true) -> { def = txt; base = n.substringBeforeLast('.') }
                n.endsWith(".sailselect", true) -> sel = txt
            }
        }
        if (def == null || sel == null) { message.text = "Choisir ensemble un fichier .saildef et un fichier .sailselect."; return }
        try { SailChart.parse(def!!, sel!!) } catch (e: Exception) { message.text = "Sail chart refusé : ${e.message}"; return }
        s.saildefFile.writeText(def!!); s.sailselectFile.writeText(sel!!)
        s.sailName = base
        message.text = ""
        refreshSail()
    }

    private fun refreshPolar() {
        polarLabel.text = s.polarName ?: "CM50 (défaut, embarquée)"
    }

    private fun save() {
        val h = host.text.toString().trim()
        val p = port.text.toString().toIntOrNull()
        val np = navPort.text.toString().toIntOrNull()
        val rt = navRelay.text.toString().trim()
        val rp = if (rt.isEmpty()) 0 else rt.toIntOrNull()
        val st = stale.text.toString().toIntOrNull()
        val dm = dimField.text.toString().toIntOrNull()
        val tdt = tdField.text.toString().trim().replace(',', '.')
        val td = if (tdt.isEmpty()) 0.0 else tdt.toDoubleOrNull()
        val cpa = cpaField.text.toString().trim().replace(',', '.').toDoubleOrNull()
        val tcpa = tcpaField.text.toString().toIntOrNull()
        val rng = rangeField.text.toString().trim().replace(',', '.').toDoubleOrNull()
        message.text = when {
            h.isEmpty() -> "Adresse de la source N2K vide."
            p == null || p !in 1..65535 -> "Port N2K invalide."
            np == null || np !in 1..65535 -> "Port de navigation invalide."
            rp == null || (rp != 0 && rp !in 1..65535) -> "Port de relais invalide."
            rp == np -> "Le port de relais doit différer du port de navigation."
            st == null || st !in 1..60 -> "Délai de péremption : 1 à 60 s."
            dm == null || dm !in 0..90 -> "Atténuation de nuit : 0 à 90 %."
            td == null || td !in 0.0..5.0 -> "Immersion du transducteur : 0 à 5 m."
            cpa == null || cpa !in 0.05..5.0 -> "CPA AIS : 0,05 à 5 NM."
            tcpa == null || tcpa !in 1..120 -> "TCPA AIS : 1 à 120 min."
            rng == null || rng !in 0.5..50.0 -> "Portée AIS : 0,5 à 50 NM."
            else -> null
        } ?: run {
            s.host = h; s.port = p!!; s.navPort = np!!; s.navRelayPort = rp!!; s.staleS = st!!; s.nightDimPct = dm!!; s.transducerDepth = td!!
            s.aisCpaNm = cpa!!; s.aisTcpaMin = tcpa!!; s.aisRangeNm = rng!!
            s.sailRose = sailRoseBox.isChecked
            s.aisIgnoreStill = aisStillBox.isChecked
            s.headingRef = when { refM.isChecked -> 'M'; refC.isChecked -> 'C'; else -> 'V' }
            setResult(RESULT_OK)
            finish()
            return
        }
    }

    @Deprecated("API Activity simple, sans AndroidX")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (requestCode == PICK_SAIL && resultCode == RESULT_OK) {
            val uris = data?.clipData?.let { cd -> (0 until cd.itemCount).map { cd.getItemAt(it).uri } }
                ?: listOfNotNull(data?.data)
            loadSail(uris)
            return
        }
        if (requestCode != PICK_POLAR || resultCode != RESULT_OK) return
        val uri = data?.data ?: return
        val bytes = try {
            contentResolver.openInputStream(uri)?.use { it.readBytes() }
        } catch (e: Exception) { null }
        if (bytes == null || bytes.size > 1_000_000) { message.text = "Fichier illisible."; return }
        // Vérification par le cœur avant de retenir le fichier.
        val err = N2kCore().use { it.setPolar(bytes) }
        if (err != null) { message.text = "Polaire refusée : $err"; return }
        s.polarFile.writeBytes(bytes)
        s.polarName = displayName(uri)
        message.text = ""
        refreshPolar()
    }

    private fun displayName(uri: Uri): String =
        contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use {
            if (it.moveToFirst()) it.getString(0) else null
        } ?: "polaire"

    companion object {
        private const val PICK_POLAR = 1
        private const val PICK_SAIL = 2
    }
}
