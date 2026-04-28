package com.o2sentry.badge

import android.app.PendingIntent
import android.content.Intent
import android.content.IntentFilter
import android.nfc.NfcAdapter
import android.nfc.Tag
import android.os.Build
import android.os.Bundle
import android.view.inputmethod.InputMethodManager
import android.widget.EditText
import android.widget.Toast
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.edit
import androidx.core.view.isVisible
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import com.google.gson.Gson
import com.google.gson.reflect.TypeToken
import com.o2sentry.badge.databinding.ActivityMainBinding

class MainActivity : AppCompatActivity() {

    private lateinit var binding: ActivityMainBinding
    private lateinit var adapter: DiversAdapter

    private var nfcAdapter: NfcAdapter? = null
    private var selectedDiver: String? = null
    private var writeDialog: AlertDialog? = null

    private val divers = mutableListOf<String>()
    private val prefs by lazy { getSharedPreferences("divers", MODE_PRIVATE) }
    private val gson = Gson()

    // ── Lifecycle ──────────────────────────────────────────────────────────────

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)
        setSupportActionBar(binding.toolbar)

        nfcAdapter = NfcAdapter.getDefaultAdapter(this)
        if (nfcAdapter == null) {
            Toast.makeText(this, "NFC non disponible sur cet appareil", Toast.LENGTH_LONG).show()
        } else if (!nfcAdapter!!.isEnabled) {
            Toast.makeText(this, "Activer le NFC dans les paramètres", Toast.LENGTH_LONG).show()
        }

        loadDivers()

        adapter = DiversAdapter(divers, onWrite = ::showWriteDialog, onDelete = ::deleteDiver)
        binding.recyclerView.adapter = adapter

        binding.fab.setOnClickListener { showAddDiverDialog() }
        refreshEmpty()
    }

    override fun onResume() {
        super.onResume()
        enableForegroundDispatch()
    }

    override fun onPause() {
        super.onPause()
        nfcAdapter?.disableForegroundDispatch(this)
    }

    // singleTop : même activité reçoit le tag quand elle est au premier plan
    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        val tag: Tag? = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            intent.getParcelableExtra(NfcAdapter.EXTRA_TAG, Tag::class.java)
        } else {
            @Suppress("DEPRECATION")
            intent.getParcelableExtra(NfcAdapter.EXTRA_TAG)
        }

        tag?.let {
            val diver = selectedDiver
            if (diver != null) {
                handleTag(it, diver)
            } else {
                identifyTag(it)
            }
        }
    }

    // ── NFC ────────────────────────────────────────────────────────────────────

    private fun enableForegroundDispatch() {
        val adapter = nfcAdapter ?: return
        val intent = Intent(this, javaClass).addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP)
        val pi = PendingIntent.getActivity(this, 0, intent, PendingIntent.FLAG_MUTABLE)
        val filters = arrayOf(IntentFilter(NfcAdapter.ACTION_TAG_DISCOVERED))
        adapter.enableForegroundDispatch(this, pi, filters, null)
    }

    private fun identifyTag(tag: Tag) {
        Thread {
            val name = NfcWriter.readBlock4(tag)
            runOnUiThread {
                if (name != null && name.isNotEmpty()) {
                    MaterialAlertDialogBuilder(this)
                        .setTitle("Badge identifié")
                        .setMessage("Plongeur : $name")
                        .setPositiveButton("OK", null)
                        .show()
                } else {
                    MaterialAlertDialogBuilder(this)
                        .setTitle("Badge inconnu")
                        .setMessage("Aucune donnée lisible en bloc 4 ou carte incompatible.")
                        .setPositiveButton("OK", null)
                        .show()
                }
            }
        }.start()
    }

    private fun handleTag(tag: Tag, diver: String) {
        Thread {
            val error = NfcWriter.writeBlock4(tag, diver)
            runOnUiThread {
                writeDialog?.dismiss()
                writeDialog = null
                selectedDiver = null
                if (error == null) {
                    MaterialAlertDialogBuilder(this)
                        .setTitle("Badge écrit ✓")
                        .setMessage("$diver enregistré sur la carte.")
                        .setPositiveButton("OK", null)
                        .show()
                    // Vibration courte pour feedback
                    binding.root.performHapticFeedback(android.view.HapticFeedbackConstants.CONFIRM)
                } else {
                    MaterialAlertDialogBuilder(this)
                        .setTitle("Erreur")
                        .setMessage(error)
                        .setPositiveButton("OK", null)
                        .show()
                }
            }
        }.start()
    }

    // ── UI ─────────────────────────────────────────────────────────────────────

    private fun showWriteDialog(name: String) {
        selectedDiver = name
        writeDialog = MaterialAlertDialogBuilder(this)
            .setTitle("Écrire le badge")
            .setMessage("Plongeur : $name\n\nApprocher la carte Mifare sur le lecteur NFC…")
            .setNegativeButton("Annuler") { _, _ ->
                selectedDiver = null
                writeDialog = null
            }
            .setOnCancelListener {
                selectedDiver = null
                writeDialog = null
            }
            .show()
    }

    private fun showAddDiverDialog() {
        val editText = EditText(this).apply {
            hint = "Nom (14 caractères max)"
            filters = arrayOf(android.text.InputFilter.LengthFilter(14))
            setSingleLine()
            inputType = android.text.InputType.TYPE_CLASS_TEXT or
                        android.text.InputType.TYPE_TEXT_FLAG_CAP_CHARACTERS
        }
        val dialog = MaterialAlertDialogBuilder(this)
            .setTitle("Ajouter un plongeur")
            .setView(editText)
            .setPositiveButton("Ajouter") { _, _ ->
                val name = editText.text.toString().trim().uppercase()
                if (name.isNotEmpty() && !divers.contains(name)) {
                    divers.add(name)
                    divers.sort()
                    saveDivers()
                    adapter.notifyDataSetChanged()
                    refreshEmpty()
                }
            }
            .setNegativeButton("Annuler", null)
            .create()
        dialog.show()
        editText.postDelayed({
            editText.requestFocus()
            val imm = getSystemService(INPUT_METHOD_SERVICE) as InputMethodManager
            imm.showSoftInput(editText, InputMethodManager.SHOW_IMPLICIT)
        }, 100)
    }

    private fun deleteDiver(name: String) {
        MaterialAlertDialogBuilder(this)
            .setMessage("Supprimer $name ?")
            .setPositiveButton("Supprimer") { _, _ ->
                divers.remove(name)
                saveDivers()
                adapter.notifyDataSetChanged()
                refreshEmpty()
            }
            .setNegativeButton("Annuler", null)
            .show()
    }

    // ── Persistence ────────────────────────────────────────────────────────────

    private fun loadDivers() {
        val json = prefs.getString("list", "[]") ?: "[]"
        val type = object : TypeToken<List<String>>() {}.type
        divers.clear()
        divers.addAll(gson.fromJson(json, type))
    }

    private fun saveDivers() {
        prefs.edit { putString("list", gson.toJson(divers)) }
    }

    private fun refreshEmpty() {
        binding.emptyText.isVisible = divers.isEmpty()
    }
}
