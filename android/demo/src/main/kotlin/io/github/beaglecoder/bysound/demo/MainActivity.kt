package io.github.beaglecoder.bysound.demo

import android.Manifest
import android.content.pm.PackageManager
import android.os.Bundle
import android.widget.ArrayAdapter
import android.widget.EditText
import android.widget.ListView
import android.widget.ProgressBar
import android.widget.Spinner
import android.widget.TextView
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import com.google.android.material.button.MaterialButton
import io.github.beaglecoder.bysound.audio.ReceiveSession
import io.github.beaglecoder.bysound.audio.SendSession
import io.github.beaglecoder.bysound.audio.SoundListener
import io.github.beaglecoder.bysound.audio.SoundSender
import io.github.beaglecoder.bysound.audio.receiveSession
import io.github.beaglecoder.bysound.audio.sendSession
import io.github.beaglecoder.bysound.modem.Band
import io.github.beaglecoder.bysound.modem.Level
import io.github.beaglecoder.bysound.modem.Profile
import io.github.beaglecoder.bysound.session.Alert
import io.github.beaglecoder.bysound.session.ReceiverOptions
import io.github.beaglecoder.bysound.session.SenderOptions
import io.github.beaglecoder.bysound.session.SessionError
import io.github.beaglecoder.bysound.session.SessionErrorCode
import io.github.beaglecoder.bysound.session.sessionError

/**
 * One screen: type a text, pick a band, send it by sound, or listen and see what arrives.
 * With the band "auto" (the default) both sides run a session: the sender adapts its channel and waits for the receiver's
 * acknowledgement, the receiver acknowledges and warns when two senders play at once.
 */
class MainActivity : AppCompatActivity() {
    private lateinit var sender: SoundSender
    private lateinit var listener: SoundListener
    private lateinit var send: MaterialButton
    private lateinit var listen: MaterialButton
    private lateinit var status: TextView
    private lateinit var level: ProgressBar
    private lateinit var bandSpinner: Spinner
    private lateinit var textField: EditText
    private val received = ArrayList<String>()
    private lateinit var adapter: ArrayAdapter<String>
    private var sending: SendSession? = null
    private var receiving: ReceiveSession? = null
    private var pendingAction: (() -> Unit)? = null

    private val isAuto get() = bandSpinner.selectedItemPosition == 0
    private fun fixedBand() = Band.entries[bandSpinner.selectedItemPosition - 1]

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)
        textField = findViewById(R.id.text)
        bandSpinner = findViewById(R.id.band)
        level = findViewById(R.id.level)
        status = findViewById(R.id.status)
        send = findViewById(R.id.send)
        listen = findViewById(R.id.listen)
        adapter = ArrayAdapter(this, android.R.layout.simple_list_item_1, received)
        findViewById<ListView>(R.id.received).adapter = adapter
        bandSpinner.adapter = ArrayAdapter(this, android.R.layout.simple_spinner_dropdown_item, listOf("auto") + Band.entries.map { it.id })

        sender = SoundSender(this)
        listener = SoundListener(
            this,
            onFrame = { h -> runOnUiThread { addReceived(h.band, h.profile, h.correctedBytes, h.text) } },
            onLevel = { l -> showLevel(l) },
        )

        send.setOnClickListener {
            if (sender.isSending || sending != null) { stopSending(); refresh(); return@setOnClickListener }
            if (isAuto) withMicrophone { startSendSession() }
            else try { sender.send(textField.text.toString(), fixedBand()) } catch (e: Exception) { toast(e.message ?: "Cannot send") }
            refresh()
        }
        listen.setOnClickListener {
            if (listener.isListening || receiving != null) { stopListening(); refresh() }
            else withMicrophone { if (isAuto) startReceiveSession() else startListening() }
        }
    }

    private fun startSendSession() {
        status.text = "Listening before sending..."
        val s = sendSession(this, textField.text.toString(), SenderOptions(onAlert = { a -> showAlert(a) }), onLevel = { l -> showLevel(l) })
        sending = s
        s.delivered.whenComplete { d, e ->
            runOnUiThread {
                status.text = if (d != null) "✓ Received on ${d.band.id} by the other device" else failure(e)
            }
        }
        s.finished.whenComplete { _, _ -> runOnUiThread { if (sending === s) sending = null; refresh() } }
    }

    private fun startReceiveSession() {
        status.text = "Waiting for a sender..."
        val r = receiveSession(this, ReceiverOptions(onAlert = { a -> showAlert(a) }), onLevel = { l -> showLevel(l) })
        receiving = r
        r.result.whenComplete { got, e ->
            runOnUiThread {
                if (got != null) {
                    addReceived(got.band, Profile.NORMAL, 0, got.text)
                    status.text = "Received on ${got.band.id}, checking for a second sender..."
                } else status.text = failure(e)
            }
            // The confirm gate: act on the data only once it says nobody else was playing.
            if (got != null) r.confirmed().whenComplete { c, e2 -> runOnUiThread { status.text = if (c != null) "✓ Confirmed (${c.watch.name.lowercase()})" else failure(e2) } }
        }
        r.finished.whenComplete { _, _ -> runOnUiThread { if (receiving === r) receiving = null; refresh() } }
    }

    private fun failure(e: Throwable?): String {
        val err: SessionError? = e?.sessionError()
        return when (err?.code) {
            SessionErrorCode.MULTIPLE_TRANSMISSIONS -> "⚠ Two senders at once"
            SessionErrorCode.RISK_SIGNAL -> "⚠ Alert: another device reported a second sender"
            SessionErrorCode.OTHER_EMITTER, SessionErrorCode.OTHER_TRANSMISSION -> "⚠ Another transmission is sounding"
            SessionErrorCode.CLONED_TRANSMISSION -> "⚠ Someone is copying this data"
            null -> "✗ ${e?.message}"
            else -> "✗ ${err.code.name.lowercase().replace('_', ' ')}"
        }
    }

    private fun showAlert(a: Alert) = runOnUiThread {
        status.text = "⚠ ${if (a.direction == Alert.Direction.HEARD) "Alert heard" else "Alert sent"}: ${a.reason.name.lowercase()}"
    }

    private fun showLevel(l: Level) = runOnUiThread { level.progress = (Math.sqrt(l.rms.toDouble()) * 200).toInt().coerceIn(0, 100) }

    private fun addReceived(band: Band, profile: Profile, corrected: Int, text: String) {
        val robust = if (profile == Profile.ROBUST) ", robust" else ""
        received.add(0, "[${band.id}$robust, $corrected corrected] $text")
        adapter.notifyDataSetChanged()
    }

    private fun stopSending() { sender.stop(); sending?.stop(); sending = null }
    private fun stopListening() { listener.stop(); receiving?.cancel(); receiving = null; level.progress = 0 }

    private fun withMicrophone(action: () -> Unit) {
        if (checkSelfPermission(Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED) {
            try { action() } catch (e: Exception) { toast(e.message ?: "Cannot open the microphone") }
            refresh()
        } else {
            pendingAction = action
            requestPermissions(arrayOf(Manifest.permission.RECORD_AUDIO), 1)
        }
    }

    private fun startListening() {
        try { listener.start() } catch (e: Exception) { toast(e.message ?: "Cannot listen") }
        refresh()
    }

    override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<out String>, grantResults: IntArray) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        val action = pendingAction
        pendingAction = null
        if (grantResults.firstOrNull() == PackageManager.PERMISSION_GRANTED && action != null) {
            try { action() } catch (e: Exception) { toast(e.message ?: "Cannot open the microphone") }
            refresh()
        } else toast("Microphone permission is needed to listen")
    }

    private fun refresh() {
        send.text = if (sender.isSending || sending != null) "Stop" else "Send"
        listen.text = if (listener.isListening || receiving != null) "Stop" else "Listen"
    }

    private fun toast(m: String) = Toast.makeText(this, m, Toast.LENGTH_SHORT).show()

    override fun onStop() {
        stopSending()
        stopListening()
        refresh()
        super.onStop()
    }

    override fun onDestroy() {
        stopSending()
        stopListening()
        super.onDestroy()
    }
}
