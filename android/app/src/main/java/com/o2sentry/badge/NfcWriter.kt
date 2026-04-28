package com.o2sentry.badge

import android.nfc.Tag
import android.nfc.tech.MifareClassic

object NfcWriter {

    private val KEY_A = byteArrayOf(
        0xFF.toByte(), 0xFF.toByte(), 0xFF.toByte(),
        0xFF.toByte(), 0xFF.toByte(), 0xFF.toByte()
    )

    // Bloc 4 = secteur 1, premier bloc de données (correspond au firmware ESP32)
    private const val SECTOR = 1
    private const val BLOCK_IN_SECTOR = 0  // → bloc absolu 4

    /**
     * Écrit le nom en bloc 4 d'une carte Mifare Classic 1K.
     * Retourne null en cas de succès, un message d'erreur sinon.
     */
    fun writeBlock4(tag: Tag, name: String): String? {
        val mfc = MifareClassic.get(tag)
            ?: return "Carte incompatible — Mifare Classic requis.\n" +
                      "Certains téléphones (Pixel, etc.) ne supportent pas Mifare Classic."
        return try {
            mfc.connect()
            val auth = mfc.authenticateSectorWithKeyA(SECTOR, KEY_A)
            if (!auth) return "Authentification échouée.\nLa clé A de la carte a été modifiée."

            // 16 octets : nom en ASCII (max 14 car.) + zéros de remplissage
            val data = ByteArray(MifareClassic.BLOCK_SIZE) { 0 }
            val bytes = name.uppercase().toByteArray(Charsets.US_ASCII)
            bytes.copyInto(data, 0, 0, minOf(bytes.size, 14))

            val block = mfc.sectorToBlock(SECTOR) + BLOCK_IN_SECTOR
            mfc.writeBlock(block, data)
            null
        } catch (e: Exception) {
            "Erreur NFC : ${e.message ?: "inconnue"}"
        } finally {
            try { mfc.close() } catch (ignored: Exception) {}
        }
    }

    /**
     * Lit le nom en bloc 4. Retourne le nom ou null si echec.
     */
    fun readBlock4(tag: Tag): String? {
        val mfc = MifareClassic.get(tag) ?: return null
        return try {
            mfc.connect()
            val auth = mfc.authenticateSectorWithKeyA(SECTOR, KEY_A)
            if (!auth) return null
            val block = mfc.sectorToBlock(SECTOR) + BLOCK_IN_SECTOR
            val data = mfc.readBlock(block)
            // Trim null bytes et espaces
            val end = data.indexOfFirst { it == 0.toByte() }.let { if (it == -1) data.size else it }
            String(data, 0, end, Charsets.US_ASCII).trim()
        } catch (e: Exception) {
            null
        } finally {
            try { mfc.close() } catch (ignored: Exception) {}
        }
    }
}
