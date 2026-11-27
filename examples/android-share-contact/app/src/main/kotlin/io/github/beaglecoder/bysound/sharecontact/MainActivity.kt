package io.github.beaglecoder.bysound.sharecontact

import android.Manifest
import android.app.Activity
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Bundle
import android.provider.ContactsContract
import android.text.InputType
import android.view.View
import android.widget.Button
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.TextView
import io.github.beaglecoder.bysound.audio.ReceiveSession
import io.github.beaglecoder.bysound.audio.SendSession
import io.github.beaglecoder.bysound.audio.receiveSession
import io.github.beaglecoder.bysound.audio.sendSession
import io.github.beaglecoder.bysound.session.ReceiverOptions
import io.github.beaglecoder.bysound.session.SenderOptions

/** One screen, plain Views: send a contact as a vCard by sound, or listen for one and add it with a tap. */
class MainActivity : Activity() {
    private lateinit var name: EditText
    private lateinit var phone: EditText
    private lateinit var email: EditText
    private lateinit var send: Button
    private lateinit var listen: Button
    private lateinit var status: TextView
    private lateinit var heard: TextView
    private lateinit var add: Button
    private var sending: SendSession? = null
    private var receiving: ReceiveSession? = null
    private var afterPermission: (() -> Unit)? = null
    private var heardContact: Contact? = null

    private fun field(hint: String, type: Int) = EditText(this).apply { this.hint = hint; inputType = type }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val pad = (16 * resources.displayMetrics.density).toInt()
        val root = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL; setPadding(pad, pad * 2, pad, pad) }
        name = field("Name", InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_CAP_WORDS)
        phone = field("Phone (optional)", InputType.TYPE_CLASS_PHONE)
        email = field("Email (optional)", InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_EMAIL_ADDRESS)
        send = Button(this).apply { text = "Send by sound"; setOnClickListener { if (sending == null) withMic(::startSend) else sending?.stop() } }
        listen = Button(this).apply { text = "Listen"; setOnClickListener { if (receiving == null) withMic(::startListen) else receiving?.cancel() } }
        status = TextView(this)
        heard = TextView(this).apply { textSize = 18f }
        add = Button(this).apply { text = "Add to contacts"; visibility = View.GONE; setOnClickListener { addToContacts() } }
        listOf(name, phone, email, send, listen, status, heard, add).forEach { root.addView(it) }
        setContentView(root)
    }

    private fun withMic(then: () -> Unit) {
        if (checkSelfPermission(Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED) then()
        else { afterPermission = then; requestPermissions(arrayOf(Manifest.permission.RECORD_AUDIO), 1) }
    }

    override fun onRequestPermissionsResult(code: Int, perms: Array<out String>, results: IntArray) {
        if (results.firstOrNull() == PackageManager.PERMISSION_GRANTED) afterPermission?.invoke() else status.text = "Microphone permission denied"
        afterPermission = null
    }

    private fun ui(f: () -> Unit) = runOnUiThread(f)

    private fun startSend() {
        val n = name.text.toString().trim()
        if (n.isEmpty()) { status.text = "A name is required"; return }
        val vcard = Contact(n, phone.text.toString().trim(), email.text.toString().trim()).toVCard()
        try {
            val s = sendSession(this, vcard, SenderOptions())      // band automatic: the ladder picks it
            sending = s
            send.text = "Stop"
            status.text = "Sending ${vcard.length} characters..."
            s.delivered.whenComplete { d, e -> ui { status.text = if (e == null) "Delivered on ${d.band.id} in ${d.seconds.toInt()} s" else "Send failed: ${e.cause ?: e}" } }
            s.finished.whenComplete { _, _ -> ui { sending = null; send.text = "Send by sound" } }
        } catch (e: Exception) { status.text = "Cannot send: ${e.message}" }
    }

    private fun startListen() {
        heardContact = null; heard.text = ""; add.visibility = View.GONE
        try {
            val r = receiveSession(this, ReceiverOptions())
            receiving = r
            listen.text = "Cancel"
            status.text = "Listening..."
            // result, then the confirm gate: confirmed() fails if another emitter or a risk frame appeared meanwhile
            r.result.thenCompose { got -> r.confirmed().thenApply { got } }.whenComplete { got, e ->
                ui {
                    if (e != null) { status.text = "Receive failed: ${e.cause ?: e}"; return@ui }
                    val c = parseVCard(got.text)
                    if (c == null) { status.text = "Heard something that is not a valid contact card; ignored."; return@ui }
                    heardContact = c
                    heard.text = "${c.name}\n${c.phone}\n${c.email}".trim()
                    status.text = "Heard on ${got.band.id}. Check it, then add."
                    add.visibility = View.VISIBLE
                }
            }
            r.finished.whenComplete { _, _ -> ui { receiving = null; listen.text = "Listen" } }
        } catch (e: Exception) { status.text = "Cannot listen: ${e.message}" }
    }

    /** Leaving the screen releases the microphone and the speaker. */
    override fun onDestroy() {
        sending?.stop()
        receiving?.cancel()
        super.onDestroy()
    }

    /** ACTION_INSERT opens the Contacts app prefilled; nothing is saved until the person taps Save there. */
    private fun addToContacts() {
        val c = heardContact ?: return
        val i = Intent(ContactsContract.Intents.Insert.ACTION).apply {
            type = ContactsContract.RawContacts.CONTENT_TYPE
            putExtra(ContactsContract.Intents.Insert.NAME, c.name)
            if (c.phone.isNotEmpty()) putExtra(ContactsContract.Intents.Insert.PHONE, c.phone)
            if (c.email.isNotEmpty()) putExtra(ContactsContract.Intents.Insert.EMAIL, c.email)
        }
        try { startActivity(i) } catch (e: Exception) { status.text = "No contacts app available" }
    }
}
