package dev.tachyon_ipc;

/**
 * Read-only snapshot of bus state, returned by {@link TachyonBus#stats()}.
 *
 * @param ringCapacity     Total ring buffer size in bytes.
 * @param ringOccupancy    Bytes currently in flight (head − tail).
 * @param consumerSleeping 0 = awake, 1 = sleeping on futex, 2 = pure-spin mode.
 * @param state            Same numeric values as {@code tachyon_state_t}.
 */
public record BusStats(long ringCapacity, long ringOccupancy, int consumerSleeping, int state) {
}
