package dev.tachyon_ipc

import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.cancellable
import kotlinx.coroutines.flow.flow
import kotlinx.coroutines.flow.flowOn
import kotlinx.coroutines.yield

/**
 * Bidirectional RPC bus backed by two independent SPSC arenas.
 *
 * Use [listen] on the callee side and [connect] on the caller side.
 * Always call [close] when done, or wrap in a `use {}` block.
 *
 * Not thread-safe. Caller side ([call], [waitResponse]) and callee side
 * ([serve], [reply]) must each be accessed from a single coroutine.
 */
class RpcBus private constructor(private val inner: TachyonRpcBus) : AutoCloseable {

    companion object {
        /**
         * Creates two SHM arenas and blocks until a caller connects via [path].
         * Interrupted syscalls (EINTR) are retried transparently.
         *
         * @param path    UNIX socket path.
         * @param capFwd  Arena capacity for incoming requests. Must be a power of two.
         * @param capRev  Arena capacity for outgoing replies. Must be a power of two.
         */
        fun listen(path: String, capFwd: Long, capRev: Long): RpcBus =
            RpcBus(TachyonRpcBus.rpcListen(path, capFwd, capRev))

        /**
         * Attaches to existing SHM arenas via the UNIX socket at [path].
         *
         * @throws TachyonException with code 14 on ABI mismatch.
         */
        fun connect(path: String): RpcBus =
            RpcBus(TachyonRpcBus.rpcConnect(path))
    }

    /**
     * Configures the consumer polling back-off strategy for both arenas.
     *
     * [pureSpin] = 1 enables pure-spin mode; 0 restores hybrid futex-sleep mode.
     * Call before the first message.
     */
    fun setPollingMode(pureSpin: Int) {
        inner.setPollingMode(pureSpin)
    }

    /**
     * Copies [payload] into arena_fwd and suspends until the response matching
     * the assigned correlation ID arrives in arena_rev.
     *
     * Yields the coroutine if the TX ring buffer is full.
     * Returns the response as an [RpcMessage] with the correlation ID echoed.
     *
     * @param payload       Request bytes.
     * @param msgType       Application-level message type (uint16 range).
     * @param spinThreshold Spin iterations before futex sleep on the RX side.
     */
    suspend fun call(
        payload: ByteArray,
        msgType: Int,
        spinThreshold: Int = 10_000,
    ): RpcMessage {
        var cid: Long
        while (true) {
            try {
                cid = inner.call(payload, msgType)
                break
            } catch (e: BufferFullException) {
                yield()
            }
        }
        return waitResponse(cid, spinThreshold)
    }

    /**
     * Sends [payload] into arena_fwd without waiting for a response.
     * Returns the assigned correlation ID for use with [waitResponse].
     *
     * Yields the coroutine if the TX ring buffer is full.
     *
     * @param payload Request bytes.
     * @param msgType Application-level message type (uint16 range).
     */
    suspend fun send(payload: ByteArray, msgType: Int): Long {
        while (true) {
            try {
                return inner.call(payload, msgType)
            } catch (e: BufferFullException) {
                yield()
            }
        }
    }

    /**
     * Suspends until the response matching [correlationId] arrives in arena_rev.
     * Returns the response as an [RpcMessage].
     *
     * Yields the coroutine while spinning.
     *
     * @param correlationId The ID returned by [send].
     * @param spinThreshold Spin iterations before futex sleep.
     * @throws PeerDeadException on correlation mismatch or fatal arena error.
     */
    suspend fun waitResponse(correlationId: Long, spinThreshold: Int = 10_000): RpcMessage {
        while (true) {
            inner.waitResponse(correlationId, spinThreshold).use { rx ->
                if (rx != null) {
                    return RpcMessage(
                        correlationId = rx.correlationId,
                        msgType = rx.msgType,
                        data = rx.getData(),
                    )
                }
            }
            yield()
        }
    }

    /**
     * Returns a [Flow] of inbound [RpcMessage] values from arena_fwd.
     *
     * Each emission carries the [RpcMessage.correlationId] required by [reply].
     * The flow runs on [Dispatchers.IO], yields between spin iterations,
     * and is cancellable.
     *
     * @param spinThreshold Spin iterations before yielding on each empty poll.
     */
    fun serve(spinThreshold: Int = 10_000): Flow<RpcMessage> = flow {
        while (true) {
            inner.serve(spinThreshold).use { rx ->
                if (rx != null) {
                    emit(
                        RpcMessage(
                            correlationId = rx.correlationId,
                            msgType = rx.msgType,
                            data = rx.getData(),
                        )
                    )
                } else {
                    yield()
                }
            }
        }
    }.flowOn(Dispatchers.IO).cancellable()

    /**
     * Copies [payload] into arena_rev as a response to [correlationId].
     *
     * The [RpcRxGuard] from [serve] must be committed (via `use {}` exit)
     * before calling this method.
     * Yields the coroutine if the TX ring buffer is full.
     *
     * @param correlationId The ID from the [RpcMessage] emitted by [serve].
     *                      Must be greater than zero.
     * @param payload       Response bytes.
     * @param msgType       Application-level response type (uint16 range).
     * @throws TachyonException if [correlationId] is zero or commit fails.
     */
    suspend fun reply(correlationId: Long, payload: ByteArray, msgType: Int) {
        while (true) {
            try {
                inner.reply(correlationId, payload, msgType)
                return
            } catch (e: BufferFullException) {
                yield()
            }
        }
    }

    override fun close() {
        inner.close()
    }
}
