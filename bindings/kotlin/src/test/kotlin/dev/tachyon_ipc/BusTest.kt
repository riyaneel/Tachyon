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

    private suspend fun connectWithRetry(path: String, maxRetries: Int = 50): Bus {
        var retries = 0
        while (true) {
            try {
                return Bus.connect(path)
            } catch (e: Exception) {
                if (++retries >= maxRetries) throw e
                delay(50)
            }
        }
    }

    @Test
    fun `should send and receive a single message via high-level flow`() = runBlocking {
        val capacity = 1024L * 16L // 16 KB

        val consumerJob = async(Dispatchers.IO) {
            Bus.listen(socketPath, capacity).use { bus ->
                bus.receive().first()
            }
        }

        val producerJob = launch(Dispatchers.IO) {
            connectWithRetry(socketPath).use { bus ->
                val payload = "Tachyon Flow".encodeToByteArray()
                bus.send(payload, typeId = 42)
            }
        }

        val receivedMessage = consumerJob.await()
        producerJob.join()

        assertEquals(42, receivedMessage.typeId)
        assertEquals("Tachyon Flow", String(receivedMessage.data))
    }

    @Test
    fun `should handle batch receiving`() = runBlocking {
        val capacity = 1024L * 16L

        val consumerJob = async(Dispatchers.IO) {
            Bus.listen(socketPath, capacity).use { bus ->
                val messages = mutableListOf<String>()
                while (messages.size < 5) {
                    bus.drainBatch(10, 1000).use { batch ->
                        for (view in batch) {
                            val actualSize = view.actualSize
                            val bytes = ByteArray(actualSize.toInt())
                            val dstSegment = MemorySegment.ofArray(bytes)

                            MemorySegment.copy(view.data, 0L, dstSegment, 0L, actualSize)
                            val str = String(bytes)
                            messages.add(str)
                        }
                    }
                    if (messages.size < 5)
                        yield()
                }

                messages
            }
        }

        val producerJob = launch(Dispatchers.IO) {
            connectWithRetry(socketPath).use { bus ->
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

        val connectorJob = launch(Dispatchers.IO) {
            connectWithRetry(socketPath).use { bus ->
                bus.setPollingMode(0)
            }
        }

        listenerJob.join()
        connectorJob.join()
    }

    @Test
    fun `should encode and decode type_id`() {
        assertEquals(42, TypeId.of(0, 42))
        assertEquals(0,  TypeId.routeId(42))
        assertEquals(42, TypeId.msgType(42))

        val id = TypeId.of(1, 99)
        assertEquals(1,  TypeId.routeId(id))
        assertEquals(99, TypeId.msgType(id))

        assertEquals(0, TypeId.of(0, 0))
    }

    @Test
    fun `should round-trip type_id over bus`() = runBlocking {
        val capacity = 1024L * 16L

        val consumerJob = async(Dispatchers.IO) {
            Bus.listen(socketPath, capacity).use { bus ->
                bus.acquireRx(10_000)?.use { rx ->
                    Pair(TypeId.routeId(rx.getTypeId()), TypeId.msgType(rx.getTypeId()))
                }
            }
        }

        launch(Dispatchers.IO) {
            connectWithRetry(socketPath).use { bus ->
                bus.send(byteArrayOf(0), typeId = TypeId.of(0, 42))
            }
        }.join()

        val result = consumerJob.await()
        assertEquals(0,  result?.first)
        assertEquals(42, result?.second)
    }
}
