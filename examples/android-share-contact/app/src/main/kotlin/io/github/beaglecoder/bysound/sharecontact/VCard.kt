package io.github.beaglecoder.bysound.sharecontact

/** A contact with only what the example shares. Phone and email are optional: the sender can strip them. */
data class Contact(val name: String, val phone: String = "", val email: String = "")

private fun esc(s: String) = s.replace("\\", "\\\\").replace(";", "\\" + ";").replace(",", "\\" + ",").replace("\r", "").replace("\n", "\\n")
private fun unesc(s: String): String {
    val sb = StringBuilder()
    var i = 0
    while (i < s.length) {
        val c = s[i]
        if (c == '\\' && i + 1 < s.length) { i++; sb.append(if (s[i] == 'n' || s[i] == 'N') '\n' else s[i]) } else sb.append(c)
        i++
    }
    return sb.toString()
}

/** Compact vCard 3.0 (no VERSION-less shortcuts, no photo): about 70 to 110 characters. */
fun Contact.toVCard(): String = buildString {
    append("BEGIN:VCARD\nVERSION:3.0\nFN:").append(esc(name))
    if (phone.isNotEmpty()) append("\nTEL:").append(esc(phone))
    if (email.isNotEmpty()) append("\nEMAIL:").append(esc(email))
    append("\nEND:VCARD")
}

private const val MAX_FIELD = 100

/** Parses what was heard. Returns null unless it is one well-formed vCard with a name: sound is not authenticated, so be strict. */
fun parseVCard(text: String): Contact? {
    val lines = text.replace("\r\n", "\n").trim().split("\n")
    if (lines.size < 4 || lines.first().uppercase() != "BEGIN:VCARD" || lines.last().uppercase() != "END:VCARD") return null
    var name = ""; var phone = ""; var email = ""
    for (line in lines.subList(1, lines.size - 1)) {
        val i = line.indexOf(':')
        if (i <= 0) continue
        val key = line.substring(0, i).substringBefore(';').uppercase()   // TEL;TYPE=CELL -> TEL
        val value = unesc(line.substring(i + 1)).trim()
        when (key) {
            "FN" -> if (name.isEmpty()) name = value
            "TEL" -> if (phone.isEmpty()) phone = value
            "EMAIL" -> if (email.isEmpty()) email = value
        }
    }
    if (name.isEmpty() || listOf(name, phone, email).any { it.length > MAX_FIELD }) return null
    if (phone.any { !(it.isDigit() || it in "+-() .") }) return null
    return Contact(name, phone, email)
}
