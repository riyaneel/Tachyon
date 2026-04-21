package dev.tachyon_ipc;

/**
 * Helpers for the {@code type_id} encoding.
 */
public final class TypeId {
	private TypeId() {
	}

	/**
	 * Encodes a {@code routeId} and {@code msgType} into a single {@code type_id} value.
	 *
	 * @param route   routing discriminator, bits [31:16], must be in [0, 65535]
	 * @param msgType Application-defined message type, bits [15:0], must be in [0, 65535]
	 * @return encoded {@code type_id}
	 */
	public static int of(int route, int msgType) {
		return (route << 16) | (msgType & 0xFFFF);
	}

	/**
	 * Extracts the {@code routeId} from bits [31:16] of {@code typeId}.
	 *
	 * @param typeId encoded type_id value
	 * @return route discriminator in [0, 65535]
	 */
	public static int routeId(int typeId) {
		return (typeId >> 16) & 0xFFFF;
	}

	/**
	 * Extracts the {@code msgType} from bits [15:0] of {@code typeId}.
	 *
	 * @param typeId encoded type_id value
	 * @return Application message type in [0, 65535]
	 */
	public static int msgType(int typeId) {
		return typeId & 0xFFFF;
	}
}
