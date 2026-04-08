package dev.tachyon_ipc

import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.cancellable
import kotlinx.coroutines.flow.flow
import kotlinx.coroutines.flow.flowOn
import kotlinx.coroutines.yield
import java.lang.foreign.MemorySegment

public class Bus private constructor(private val inner: TachyonBus) : AutoCloseable {
    public companion object {
        public fun listen(path: String, capacity: Long): Bus = Bus(TachyonBus.listen(path, capacity))
        public fun connect(path: String): Bus = Bus(TachyonBus.connect(path))
    }

    public fun setPollingMode(spinMode: Int) {
        inner.setPollingMode(spinMode)
    }

    public fun setNumaNode(nodeId: Int) {
        inner.setNumaNode(nodeId)
    }

    public suspend fun send(data: ByteArray, typeId: Int = 0) {
        val size = data.size.toLong()
        val srcSegment = MemorySegment.ofArray(data)

        while (true) {
            try {
                inner.acquireTx(size).use { guard ->
                    MemorySegment.copy(srcSegment, 0L, guard.data, 0L, size)
                    guard.commit(size, typeId)
                }
                break
            } catch (e: BufferFullException) {
                yield()
            }
        }
    }

    public fun receive(
        spinThreshold: Int = 10_000,
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

    public fun acquireTx(maxSize: Long): TxGuard = inner.acquireTx(maxSize)

    public fun acquireRx(spinThreshold: Int): RxGuard? = inner.acquireRx(spinThreshold)

    public fun drainBatch(maxMsgs: Int, spinThreshold: Int): RxBatchGuard = inner.drainBatch(maxMsgs, spinThreshold)

    public fun flush() {
        inner.flush()
    }

    override fun close() {
        inner.close()
    }
}
