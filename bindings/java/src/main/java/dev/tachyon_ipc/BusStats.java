package dev.tachyon_ipc;

/**
 * Read-only snapshot of bus state, returned by {@link TachyonBus#stats()}.
 *
 * <p>Cheap (relaxed atomic loads only) and safe to call from either side of the bus.
 * Per-field consistent, not struct-consistent — fine for monitoring, not for synchronization.
 *
 * @param ringCapacity     Total ring buffer size in bytes.
 * @param ringOccupancy    Bytes currently in flight (head − tail).
 * @param consumerSleeping 0 = awake, 1 = sleeping on futex, 2 = pure-spin mode.
 * @param state            Same numeric values as {@code tachyon_state_t}.
 */
public record BusStats(long ringCapacity, long ringOccupancy, int consumerSleeping, int state) {
}
