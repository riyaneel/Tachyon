package dev.tachyon_ipc

public class Message(
    public val typeId: Int,
    public val data: ByteArray
) {
    public val size: Int get() = data.size

    override fun equals(other: Any?): Boolean {
        if (this === other)
            return true
        if (javaClass != other?.javaClass)
            return false

        other as Message
        if (typeId != other.typeId)
            return false
        if (!data.contentEquals(other.data))
            return false

        return true
    }

    override fun hashCode(): Int {
        var result = typeId
        result = 31 * result + data.contentHashCode()
        return result
    }

    override fun toString(): String {
        return "Message(typeId=$typeId, size=$size, data=${data.contentToString()})"
    }
}
