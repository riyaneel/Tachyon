package dev.tachyon_ipc

import kotlinx.coroutines.*
import kotlinx.coroutines.flow.first
import org.junit.jupiter.api.AfterEach
import org.junit.jupiter.api.BeforeEach
import org.junit.jupiter.api.Test
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.file.Files
import java.nio.file.Paths
import java.util.UUID
import kotlin.test.assertContentEquals
import kotlin.test.assertEquals
import kotlin.test.assertTrue

class RpcBusTest {
    private lateinit var socketPath: String

    @BeforeEach
    fun setup() {
        socketPath = "/tmp/tachyon_rpc_kt_${UUID.randomUUID()}.sock"
    }

    @AfterEach
    fun teardown() {
        Files.deleteIfExists(Paths.get(socketPath))
    }

    private suspend fun connectWithRetry(path: String, maxRetries: Int = 50): RpcBus {
        var retries = 0
        while (true) {
            try {
                return RpcBus.connect(path)
            } catch (e: TachyonException) {
                if (e.code != 11 || ++retries >= maxRetries) throw e
                delay(50)
            }
        }
    }

    @Test
    fun `should complete a call and receive response`() = runBlocking {
        val cap = 1L shl 16

        val calleeJob = async(Dispatchers.IO) {
            RpcBus.listen(socketPath, cap, cap).use { callee ->
                callee.serve().first().let { req ->
                    callee.reply(req.correlationId, req.data.reversed().toByteArray(), msgType = 2)
                }
            }
        }

        delay(20)

        val callerJob = async(Dispatchers.IO) {
            connectWithRetry(socketPath).use { caller ->
                caller.call("hello_rpc_kt".encodeToByteArray(), msgType = 1)
            }
        }

        val response = callerJob.await()
        calleeJob.await()

        assertEquals(2, response.msgType)
        assertContentEquals("hello_rpc_kt".reversed().encodeToByteArray(), response.data)
    }

    @Test
    fun `should preserve msg type on serve`() = runBlocking {
        val cap = 1L shl 16

        val receivedMsgType = CompletableDeferred<Int>()

        val calleeJob = launch(Dispatchers.IO) {
            RpcBus.listen(socketPath, cap, cap).use { callee ->
                callee.serve().first().let { req ->
                    receivedMsgType.complete(req.msgType)
                    callee.reply(req.correlationId, byteArrayOf(0x01), msgType = 99)
                }
            }
        }

        delay(20)

        val callerJob = async(Dispatchers.IO) {
            connectWithRetry(socketPath).use { caller ->
                caller.call(byteArrayOf(0x01), msgType = 42)
            }
        }

        val response = callerJob.await()
        calleeJob.join()

        assertEquals(42, receivedMsgType.await())
        assertEquals(99, response.msgType)
    }

    @Test
    fun `correlation id should be monotonically increasing`() = runBlocking {
        val cap = 1L shl 16
        val n = 4

        val calleeJob = launch(Dispatchers.IO) {
            RpcBus.listen(socketPath, cap, cap).use { callee ->
                repeat(n) {
                    callee.serve().first().let { req ->
                        callee.reply(req.correlationId, byteArrayOf(0x01), msgType = 0)
                    }
                }
            }
        }

        delay(20)

        val cids = mutableListOf<Long>()

        connectWithRetry(socketPath).use { caller ->
            repeat(n) {
                val cid = caller.send(byteArrayOf(0x01), msgType = 0)
                cids.add(cid)
                caller.waitResponse(cid)
            }
        }

        calleeJob.join()

        assertTrue(cids.all { it > 0 }, "all correlation IDs must be > 0")
        for (i in 0 until n - 1)
            assertEquals(cids[i] + 1, cids[i + 1], "correlation IDs must be monotonically increasing")
    }

    @Test
    fun `should match responses to n in-flight requests`() = runBlocking {
        val cap = 1L shl 18
        val n = 8

        val sent = IntArray(n) { it * 100 }

        val calleeJob = launch(Dispatchers.IO) {
            RpcBus.listen(socketPath, cap, cap).use { callee ->
                callee.setPollingMode(1)
                repeat(n) {
                    callee.serve().first().let { req ->
                        callee.reply(req.correlationId, req.data, msgType = 0)
                    }
                }
            }
        }

        delay(20)

        connectWithRetry(socketPath).use { caller ->
            caller.setPollingMode(1)
            val cids = sent.map { v ->
                caller.send(ByteBuffer.allocate(4).order(ByteOrder.LITTLE_ENDIAN).putInt(v).array(), msgType = 0)
            }
            cids.forEachIndexed { i, cid ->
                val resp = caller.waitResponse(cid)
                val val_ = ByteBuffer.wrap(resp.data).order(ByteOrder.LITTLE_ENDIAN).int
                assertEquals(sent[i], val_)
            }
        }

        calleeJob.join()
    }

    @Test
    fun `should configure polling mode on both directions`() = runBlocking {
        val cap = 1L shl 16

        val calleeJob = launch(Dispatchers.IO) {
            RpcBus.listen(socketPath, cap, cap).use { callee ->
                callee.setPollingMode(1)
                callee.setPollingMode(0)
                callee.serve().first().let { req ->
                    callee.reply(req.correlationId, byteArrayOf(0x01), msgType = 0)
                }
            }
        }

        delay(20)

        connectWithRetry(socketPath).use { caller ->
            caller.setPollingMode(1)
            caller.setPollingMode(0)
            val resp = caller.call(byteArrayOf(0x01), msgType = 0)
            assertContentEquals(byteArrayOf(0x01), resp.data)
        }

        calleeJob.join()
    }

    @Test
    fun `serve flow should emit multiple requests`() = runBlocking {
        val cap = 1L shl 16
        val n = 3

        val calleeJob = launch(Dispatchers.IO) {
            RpcBus.listen(socketPath, cap, cap).use { callee ->
                repeat(n) {
                    callee.serve().first().let { req ->
                        callee.reply(req.correlationId, req.data, msgType = 0)
                    }
                }
            }
        }

        delay(20)

        val responses = mutableListOf<RpcMessage>()

        connectWithRetry(socketPath).use { caller ->
            repeat(n) { i ->
                val resp = caller.call("msg_$i".encodeToByteArray(), msgType = i)
                responses.add(resp)
            }
        }

        calleeJob.join()

        assertEquals(n, responses.size)
        responses.forEachIndexed { i, resp ->
            assertContentEquals("msg_$i".encodeToByteArray(), resp.data)
        }
    }

    @Test
    fun `use block should close bus`(): Unit = runBlocking {
        val cap = 1L shl 16

        val calleeJob = launch(Dispatchers.IO) {
            RpcBus.listen(socketPath, cap, cap).use { callee ->
                callee.serve().first().let { req ->
                    callee.reply(req.correlationId, byteArrayOf(0x01), msgType = 0)
                }
            }
        }

        delay(20)

        var busRef: RpcBus? = null
        connectWithRetry(socketPath).use { caller ->
            busRef = caller
            caller.call(byteArrayOf(0x01), msgType = 0)
        }

        calleeJob.join()
        busRef?.close()
    }

    @Test
    fun `should handle struct payload`() = runBlocking {
        val cap = 1L shl 16
        val a = 0xDEAD
        val b = 0xBEEF

        val req = ByteBuffer.allocate(8).order(ByteOrder.LITTLE_ENDIAN).putInt(a).putInt(b).array()

        val calleeJob = launch(Dispatchers.IO) {
            RpcBus.listen(socketPath, cap, cap).use { callee ->
                callee.serve().first().let { msg ->
                    val buf = ByteBuffer.wrap(msg.data).order(ByteOrder.LITTLE_ENDIAN)
                    val x = buf.int;
                    val y = buf.int
                    val resp = ByteBuffer.allocate(8).order(ByteOrder.LITTLE_ENDIAN)
                        .putInt(x + y).putInt(x xor y).array()
                    callee.reply(msg.correlationId, resp, msgType = 1)
                }
            }
        }

        delay(20)

        connectWithRetry(socketPath).use { caller ->
            val resp = caller.call(req, msgType = 1)
            val buf = ByteBuffer.wrap(resp.data).order(ByteOrder.LITTLE_ENDIAN)
            assertEquals(a + b, buf.int)
            assertEquals(a xor b, buf.int)
        }

        calleeJob.join()
    }
}
