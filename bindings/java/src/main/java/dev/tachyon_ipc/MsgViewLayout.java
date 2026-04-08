package dev.tachyon_ipc;

import java.lang.foreign.MemoryLayout;
import java.lang.foreign.MemoryLayout.PathElement;
import java.lang.foreign.StructLayout;
import java.lang.foreign.ValueLayout;
import java.lang.invoke.VarHandle;

/**
 * FFM (Foreign Function & Memory) memory layout definition for the native {@code tachyon_msg_view_t} C struct.
 *
 * @apiNote Internal utility. Exposes {@link VarHandle} accessors for zero-copy memory read
 * directly from the shared memory arena.
 * @implSpec The layout MUST be strictly isomorphic to the C++ ABI definition.
 * Includes explicit 4-byte padding after the 32-bit {@code type_id} to ensure 64-bit word alignment
 * on the host architecture.
 */
final class MsgViewLayout {
	private MsgViewLayout() {
	}

	/**
	 * The structured memory layout mirroring the native C struct sequence.
	 */
	static final StructLayout layout = MemoryLayout.structLayout(
			ValueLayout.ADDRESS.withName("ptr"),
			ValueLayout.JAVA_LONG.withName("actual_size"),
			ValueLayout.JAVA_LONG.withName("reserved_"),
			ValueLayout.JAVA_INT.withName("type_id"),
			MemoryLayout.paddingLayout(4)
	).withName("tachyon_msg_view_t");

	/**
	 * Memory accessor for the {@code ptr} field. Maps to the raw payload memory address.
	 */
	static final VarHandle ptrHandle = layout.varHandle(PathElement.groupElement("ptr"));

	/**
	 * Memory accessor for the {@code actual_size} field. Maps to the number of bytes written.
	 */
	static final VarHandle sizeHandle = layout.varHandle(PathElement.groupElement("actual_size"));

	/**
	 * Memory accessor for the {@code type_id} field. Maps to the user-defined protocol identifier.
	 */
	static final VarHandle typeHandle = layout.varHandle(PathElement.groupElement("type_id"));

	/**
	 * The exact byte size of the struct layout, used for pointer arithmetic during batch iteration.
	 */
	static final long sizeBytes = layout.byteSize();
}
