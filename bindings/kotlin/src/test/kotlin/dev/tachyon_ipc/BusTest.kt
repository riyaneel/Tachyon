package dev.tachyon_ipc

import kotlinx.coroutines.*
import kotlinx.coroutines.flow.first
import org.junit.jupiter.api.AfterEach
import org.junit.jupiter.api.BeforeEach
import org.junit.jupiter.api.Test
import java.lang.foreign.MemorySegment
import java.nio.file.Files
import java.nio.file.Paths
import java.util.UUID
import kotlin.test.assertEquals
import kotlin.test.assertFailsWith

class BusTest {
    private lateinit var socketPath: String

    @BeforeEach
    fun setup() {
        socketPath = "/tmp/tachyon_kt_${UUID.randomUUID()}.sock"
    }

    @AfterEach
    fun teardown() {
        Files.deleteIfExists(Paths.get(socketPath))
    }

    @Test
    fun `should send and receive a single message via high-level flow`() = runBlocking {
        val capacity = 1024L * 16L // 16 KB

        val consumerJob = async(Dispatchers.IO) {
            Bus.listen(socketPath, capacity).use { bus ->
                bus.receive().first()
            }
        }

        delay(100)

        val producerJob = launch(Dispatchers.IO) {
            Bus.connect(socketPath).use { bus ->
                val payload = "Tachyon Flow".encodeToByteArray()
                bus.send(payload, typeId = 42)
            }
        }

        val receivedMessage = consumerJob.await()
        producerJob.join()

        assertEquals(42, receivedMessage.typeId)
        assertEquals("Tachyon Flow", receivedMessage.data.decodeToString())
    }

    @Test
    fun `should handle zero-copy batch draining`() = runBlocking {
        val capacity = 1024L * 16L

        val consumerJob = async(Dispatchers.IO) {
            Bus.listen(socketPath, capacity).use { bus ->
                val messages = mutableListOf<String>()
                while (messages.size < 5) {
                    bus.drainBatch(maxMsgs = 10, spinThreshold = 10_000).use { batch ->
                        for (i in 0 until batch.count.toInt()) {
                            val view = batch.get(i)
                            val bytes = ByteArray(view.actualSize.toInt())
                            val dstSegment = MemorySegment.ofArray(bytes)
                            MemorySegment.copy(view.data, 0L, dstSegment, 0L, view.actualSize)
                            messages.add(bytes.decodeToString())
                        }
                    }

                    if (messages.size < 5)
                        yield()
                }

                messages
            }
        }

        delay(100)

        val producerJob = launch(Dispatchers.IO) {
            Bus.connect(socketPath).use { bus ->
                for (i in 1..5) {
                    val payload = "Msg $i".encodeToByteArray()
                    val srcSegment = MemorySegment.ofArray(payload)

                    bus.acquireTx(payload.size.toLong()).use { tx ->
                        MemorySegment.copy(srcSegment, 0L, tx.data, 0L, payload.size.toLong())
                        tx.commit(payload.size.toLong(), i)
                    }
                }

                bus.flush()
            }
        }

        val results = consumerJob.await()
        producerJob.join()

        assertEquals(5, results.size)
        assertEquals("Msg 1", results.first())
        assertEquals("Msg 5", results.last())
    }

    @Test
    fun `should apply hardware configurations`() = runBlocking {
        val capacity = 1024L * 16L

        val listenerJob = launch(Dispatchers.IO) {
            Bus.listen(socketPath, capacity).use { bus ->
                bus.setNumaNode(0)
                bus.setPollingMode(1)
            }
        }

        delay(100)

        val connectorJob = launch(Dispatchers.IO) {
            Bus.connect(socketPath).use { bus ->
                bus.setPollingMode(0)
            }
        }

        listenerJob.join()
        connectorJob.join()
    }
}
