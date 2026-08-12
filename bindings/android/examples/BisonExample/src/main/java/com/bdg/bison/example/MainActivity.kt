// MIT License © 2025 Binary Dice Games
// Permission is hereby granted, free of charge, to use, copy, modify, and
// distribute this file. See the LICENSE file or
// https://opensource.org/licenses/MIT for details.

package com.bdg.bison.example

import android.os.Bundle
import android.widget.Button
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import com.bdg.bison.Dynamic
import com.bdg.bison.rmi.Client
import kotlin.concurrent.thread

/**
 * Exercises the `com.bdg.bison` Android binding end to end on a real
 * device/emulator: the [Dynamic] quick start (matching README.md), a
 * locally registered method (`addMethod`/`call`), and an in-process RMI
 * round trip (`rmi::standalone`, matching README.md's RMI section) --
 * see docs/examples.md for how this app is meant to be run and read as a
 * validation step, not just a demo.
 */
class MainActivity : AppCompatActivity() {

    private lateinit var resultText: TextView

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        resultText = findViewById(R.id.resultText)
        findViewById<Button>(R.id.runDemoButton).setOnClickListener { runDemo() }
        runDemo()
    }

    private fun runDemo() {
        resultText.text = "Running...\n"
        // bison_c.h calls block on native I/O in the RMI case; keep the
        // demo off the main thread the way any real usage should.
        thread {
            val log = StringBuilder()
            try {
                dynamicQuickStart(log)
                localMethodCall(log)
                rmiStandaloneRoundTrip(log)
                log.append("\nAll steps completed successfully.\n")
            } catch (e: Exception) {
                log.append("\nFAILED: ").append(e).append('\n')
            }
            runOnUiThread { resultText.text = log.toString() }
        }
    }

    private fun dynamicQuickStart(log: StringBuilder) {
        log.append("== Dynamic quick start ==\n")
        Dynamic("Player").use { player ->
            player.setInt("hp", 100)
            player.setString("name", "hero")
            player.setFloat("speed", 3.5f)
            log.append("created Player{hp=100, name=hero, speed=3.5}\n")

            player.setInt("hp", 142)
            log.append("hp after mutation: ${player.getInt("hp")}\n")

            val bytes = player.serialize()
            log.append("serialize() -> ${bytes.size} bytes\n")
            Dynamic.deserialize(bytes).use { restored ->
                log.append("deserialize().hp -> ${restored.getInt("hp")}\n")
            }
            log.append("toJson() -> ${player.toJson()}\n")
        }
    }

    private fun localMethodCall(log: StringBuilder) {
        log.append("\n== addMethod / call ==\n")
        Dynamic("Calculator").use { calc ->
            calc.addMethod("add") { _, params, result ->
                result.setInt("value", params.getInt("a") + params.getInt("b"))
            }
            Dynamic().use { args ->
                args.setInt("a", 3)
                args.setInt("b", 4)
                calc.call("add", args).use { result ->
                    log.append("calc.add(3, 4) -> ${result.getInt("value")}\n")
                }
            }
        }
    }

    private fun rmiStandaloneRoundTrip(log: StringBuilder) {
        log.append("\n== RMI (rmi::standalone) ==\n")
        Dynamic("ExampleCalculator").use { proto ->
            proto.setInt("result", 0)
            proto.addMethod("add") { self, params, result ->
                val sum = params.getInt("a") + params.getInt("b")
                self.setInt("result", sum)
                result.setInt("value", sum)
            }
            Dynamic.registerClass(proto)
        }
        log.append("registered class ExampleCalculator\n")

        Client.standalone().use { client ->
            client.connect()
            log.append("client connected (in-process)\n")
            client.instantiate("ExampleCalculator", null).use { proxy ->
                Dynamic().use { args ->
                    args.setInt("a", 10)
                    args.setInt("b", 32)
                    proxy.call("add", args).use { result ->
                        log.append("proxy.call(\"add\", a=10, b=32) -> ${result.getInt("value")}\n")
                    }
                }
                proxy.get().use { snapshot ->
                    log.append("proxy.get().result -> ${snapshot.getInt("result")}\n")
                }
            }
            client.disconnect()
        }
    }
}
