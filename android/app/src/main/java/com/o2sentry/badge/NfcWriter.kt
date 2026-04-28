package com.o2sentry.badge

import android.nfc.Tag
import android.nfc.tech.MifareClassic
import android.nfc.tech.MifareUltralight

object NfcWriter {

    private val KEY_A = byteArrayOf(
        0xFF.toByte(), 0xFF.toByte(), 0xFF.toByte(),
        0xFF.toByte(), 0xFF.toByte(), 0xFF.toByte()
    )

    /**
     * Écrit le nom en "bloc 4" (16 octets à partir de l'index 4).
     * Gère Mifare Classic et Mifare Ultralight.
     */
    fun writeBlock4(tag: Tag, name: String): String? {
        val bytes = ByteArray(16) { 0 }
        val nameBytes = name.uppercase().toByteArray(Charsets.US_ASCII)
        nameBytes.copyInto(bytes, 0, 0, minOf(nameBytes.size, 14))

        // 1. Essayer Mifare Ultralight (Pages de 4 octets)
        val mul = MifareUltralight.get(tag)
        if (mul != null) {
            return try {
                mul.connect()
                // On écrit 4 pages (4, 5, 6, 7) pour faire 16 octets
                for (i in 0 until 4) {
                    val pageData = bytes.sliceArray(i * 4 until (i + 1) * 4)
                    mul.writePage(4 + i, pageData)
                }
                null
            } catch (e: Exception) {
                "Erreur Ultralight : ${e.message}"
            } finally {
                try { mul.close() } catch (e: Exception) {}
            }
        }

        // 2. Essayer Mifare Classic (Blocs de 16 octets)
        val mfc = MifareClassic.get(tag)
        if (mfc != null) {
            return try {
                mfc.connect()
                val sector = 1
                if (mfc.authenticateSectorWithKeyA(sector, KEY_A)) {
                    val block = mfc.sectorToBlock(sector) // bloc 4
                    mfc.writeBlock(block, bytes)
                    null
                } else {
                    "Authentification échouée (Clé A)."
                }
            } catch (e: Exception) {
                "Erreur Classic : ${e.message}"
            } finally {
                try { mfc.close() } catch (e: Exception) {}
            }
        }

        return "Carte incompatible (Mifare Classic ou Ultralight requis)."
    }

    /**
     * Lit le nom en bloc 4.
     */
    fun readBlock4(tag: Tag): String? {
        val mul = MifareUltralight.get(tag)
        if (mul != null) {
            return try {
                mul.connect()
                val data = mul.readPages(4) // Lit 4 pages d'un coup (16 octets)
                bytesToString(data)
            } catch (e: Exception) {
                null
            } finally {
                try { mul.close() } catch (e: Exception) {}
            }
        }

        val mfc = MifareClassic.get(tag)
        if (mfc != null) {
            return try {
                mfc.connect()
                val sector = 1
                if (mfc.authenticateSectorWithKeyA(sector, KEY_A)) {
                    val block = mfc.sectorToBlock(sector)
                    val data = mfc.readBlock(block)
                    bytesToString(data)
                } else null
            } catch (e: Exception) {
                null
            } finally {
                try { mfc.close() } catch (e: Exception) {}
            }
        }
        return null
    }

    private fun bytesToString(data: ByteArray): String {
        val end = data.indexOfFirst { it == 0.toByte() }.let { if (it == -1) data.size else it }
        return String(data, 0, end, Charsets.US_ASCII).trim()
    }
}
