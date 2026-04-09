package dev.tachyon_ipc

import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.cancellable
import kotlinx.coroutines.flow.flow
import kotlinx.coroutines.flow.flowOn
import kotlinx.coroutines.yield
import java.lang.foreign.MemorySegment

/**
 * Lock-free SPSC IPC bus.
 * Backed by shared memory (memfd/shm_open) and cross-process futexes/ulocks.
 * * Not thread-safe. Must be accessed sequentially to preserve SPSC invariants.
 */
public class Bus private constructor(private val inner: TachyonBus) : AutoCloseable {
    public companion object {
        /**
         * Initializes a new IPC arena and binds a UDS listener.
         */
        public fun listen(path: String, capacity: Long): Bus = Bus(TachyonBus.listen(path, capacity))

        /**
         * Connects to an existing IPC arena via UDS handshake.
         */
        public fun connect(path: String): Bus = Bus(TachyonBus.connect(path))
    }

    /**
     * Controls futex wait behavior.
     * Modes:
     * 0: CONSUMER_AWAKE
     * 1: CONSUMER_SLEEPING
     * 2: CONSUMER_PURE_SPIN
     */
    public fun setPollingMode(spinMode: Int) {
        inner.setPollingMode(spinMode)
    }

    /**
     * Pins the shared memory arena to a specific NUMA node.
     * Fails silently if MPOL_PREFERRED is unsupported by the OS.
     */
    public fun setNumaNode(nodeId: Int) {
        inner.setNumaNode(nodeId)
    }

    /**
     * High-level suspendable send.
     * Yields the coroutine if the bus is temporarily full.
     */
    public suspend fun send(data: ByteArray, typeId: Int = 0) {
        val srcSegment = MemorySegment.ofArray(data)

        while (true) {
            try {
                inner.acquireTx(data.size.toLong()).use { tx ->
                    MemorySegment.copy(srcSegment, 0L, tx.data, 0L, data.size.toLong())
                    tx.commit(data.size.toLong(), typeId)
                }
                yield()
                break
            } catch (e: BufferFullException) {
                yield()
            }
        }
    }

    /**
     * Converts the C-style blocking receiver into an asynchronous Kotlin Flow.
     */
    public fun receive(
        spinThreshold: Int = 1000,
        onEmpty: suspend () -> Unit = { yield() }
    ): Flow<Message> = flow {
        while (true) {
            val guard = inner.acquireRx(spinThreshold)

            if (guard != null) {
                val msg = guard.use { g ->
                    val actualSize = g.actualSize
                    val bytes = ByteArray(actualSize.toInt())
                    val dstSegment = MemorySegment.ofArray(bytes)

                    MemorySegment.copy(g.data, 0L, dstSegment, 0L, actualSize)
                    Message(g.typeId, bytes)
                }
                emit(msg)
            } else {
                onEmpty()
            }
        }
    }.flowOn(Dispatchers.IO).cancellable()

    /**
     * Acquires a zero-copy transmission guard.
     * The exposed MemorySegment is only valid until commit() or close().
     */
    public fun acquireTx(maxSize: Long): TxGuard = inner.acquireTx(maxSize)

    /**
     * Acquires a zero-copy reception guard.
     * The exposed MemorySegment is spatially invalidated once the guard is closed.
     */
    public fun acquireRx(spinThreshold: Int): RxGuard? = inner.acquireRx(spinThreshold)

    /**
     * Drains multiple messages continuously for vectorized processing.
     */
    public fun drainBatch(maxMsgs: Int, spinThreshold: Int): RxBatchGuard = inner.drainBatch(maxMsgs, spinThreshold)

    /**
     * Wakes up the remote peer if it is sleeping on the futex.
     * Required manually only after batched TxGuard commits without explicit flush.
     */
    public fun flush() {
        inner.flush()
    }

    override fun close() {
        inner.close()
    }
}
