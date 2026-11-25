package io.github.beaglecoder.bysound.demo

import android.Manifest
import android.content.pm.PackageManager
import android.os.Bundle
import android.widget.ArrayAdapter
import android.widget.EditText
import android.widget.ListView
import android.widget.ProgressBar
import android.widget.Spinner
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import com.google.android.material.button.MaterialButton
import io.github.beaglecoder.bysound.audio.SoundListener
import io.github.beaglecoder.bysound.audio.SoundSender
import io.github.beaglecoder.bysound.modem.Band
import io.github.beaglecoder.bysound.modem.Profile

/** One screen: type a text, pick a band, send it by sound, or listen and see what arrives. */
class MainActivity : AppCompatActivity() {
    private lateinit var sender: SoundSender
    private lateinit var listener: SoundListener
    private lateinit var send: MaterialButton
    private lateinit var listen: MaterialButton
    private val received = ArrayList<String>()
    private lateinit var adapter: ArrayAdapter<String>

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)
        val text = findViewById<EditText>(R.id.text)
        val band = findViewById<Spinner>(R.id.band)
        val level = findViewById<ProgressBar>(R.id.level)
        send = findViewById(R.id.send)
        listen = findViewById(R.id.listen)
        adapter = ArrayAdapter(this, android.R.layout.simple_list_item_1, received)
        findViewById<ListView>(R.id.received).adapter = adapter
        band.adapter = ArrayAdapter(this, android.R.layout.simple_spinner_dropdown_item, Band.entries.map { it.id })

        sender = SoundSender(this)
        listener = SoundListener(
            this,
            onFrame = { h ->
                runOnUiThread {
                    val robust = if (h.profile == Profile.ROBUST) ", robust" else ""
                    received.add(0, "[${h.band.id}$robust, ${h.correctedBytes} corrected] ${h.text}")
                    adapter.notifyDataSetChanged()
                }
            },
            onLevel = { l -> runOnUiThread { level.progress = (Math.sqrt(l.rms.toDouble()) * 200).toInt().coerceIn(0, 100) } },
        )

        send.setOnClickListener {
            if (sender.isSending) sender.stop() else {
                val b = Band.entries[band.selectedItemPosition]
                try { sender.send(text.text.toString(), b) } catch (e: Exception) { toast(e.message ?: "Cannot send") }
            }
            refresh()
        }
        listen.setOnClickListener {
            if (listener.isListening) { listener.stop(); level.progress = 0; refresh() }
            else if (checkSelfPermission(Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED) startListening()
            else requestPermissions(arrayOf(Manifest.permission.RECORD_AUDIO), 1)
        }
    }

    private fun startListening() {
        try { listener.start() } catch (e: Exception) { toast(e.message ?: "Cannot listen") }
        refresh()
    }

    override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<out String>, grantResults: IntArray) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (grantResults.firstOrNull() == PackageManager.PERMISSION_GRANTED) startListening() else toast("Microphone permission is needed to listen")
    }

    private fun refresh() {
        send.text = if (sender.isSending) "Stop" else "Send"
        listen.text = if (listener.isListening) "Stop" else "Listen"
    }

    private fun toast(m: String) = Toast.makeText(this, m, Toast.LENGTH_SHORT).show()

    override fun onDestroy() {
        sender.stop()
        listener.stop()
        super.onDestroy()
    }
}
