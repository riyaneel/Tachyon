package dev.tachyon_ipc

/**
 * Heap-allocated representation of an inbound RPC message.
 *
 * [correlationId] identifies which call this message belongs to.
 * On the callee side, pass it unchanged to [RpcBus.reply].
 */
class RpcMessage(val correlationId: Long, val msgType: Int, val data: ByteArray) {
    val size: Int get() = data.size

    override fun equals(other: Any?): Boolean {
        if (this === other)
            return true
        if (javaClass != other?.javaClass)
            return false

        other as RpcMessage
        if (correlationId != other.correlationId)
            return false
        if (msgType != other.msgType)
            return false
        if (!data.contentEquals(other.data))
            return false

        return true
    }

    override fun hashCode(): Int {
        var result = correlationId.hashCode()
        result = 31 * result + msgType
        result = 31 * result + data.contentHashCode()
        return result
    }

    override fun toString(): String = "RpcMessage(correlationId=$correlationId, msgType=$msgType, size=$size)"
}