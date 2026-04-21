package dev.tachyon_ipc

/**
 * Helpers for `type_id` encoding
 *
 * ```
 * bits [31:16]: routeId: reserved for RPC, must be 0 for now
 * bits [15:0]:  msgType: application-defined discriminator
 * ```
 */
object TypeId {

    /**
     * Encodes [route] and [msgType] into a single `type_id`. Use `route = 0` to preserve v0.3.x behavior.
     */
    fun of(route: Int, msgType: Int): Int = (route shl 16) or (msgType and 0xFFFF)

    /**
     * Extracts the routeId from bits [31:16] of [typeId].
     */
    fun routeId(typeId: Int): Int = (typeId ushr 16) and 0xFFFF

    /**
     * Extracts the msgType from bits [15:0] of [typeId].
     */
    fun msgType(typeId: Int): Int = typeId and 0xFFFF
}
