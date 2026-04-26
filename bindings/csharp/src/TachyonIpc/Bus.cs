using System.Runtime.CompilerServices;
using TachyonIpc.Native;

namespace TachyonIpc;

/// <summary>
/// Managed wrapper around a native <c>tachyon_bus_t</c>.
/// Not thread-safe: use one instance per thread, same contract as the C API.
/// </summary>
public sealed unsafe class Bus : IDisposable
{
    private nint _bus;

    static Bus() => NativeLoader.Register();

    private Bus(nint bus) => _bus = bus;

    /// <summary>Creates the listener side of the bus (server). Blocks until a producer connects.</summary>
    /// <param name="socketPath">UDS socket path. Unlinked automatically after handshake.</param>
    /// <param name="capacity">Ring buffer size in bytes. Must be a power of two.</param>
    /// <exception cref="TachyonException">Native error during SHM creation or handshake.</exception>
    public static Bus Listen(string socketPath, nuint capacity)
    {
        nint bus;
        TachyonException.ThrowIfError(
            TachyonNative.tachyon_bus_listen(socketPath, capacity, &bus), nameof(TachyonNative.tachyon_bus_listen));
        return new Bus(bus);
    }

    /// <summary>Connects to an existing listener (client).</summary>
    /// <param name="socketPath">UDS socket path of the listener.</param>
    /// <exception cref="TachyonException">Native error during connection or handshake.</exception>
    public static Bus Connect(string socketPath)
    {
        nint bus;
        TachyonException.ThrowIfError(
            TachyonNative.tachyon_bus_connect(socketPath, &bus), nameof(TachyonNative.tachyon_bus_connect));
        return new Bus(bus);
    }

    /// <summary>
    /// Pins the SHM arena to a NUMA node. Call immediately after <see cref="Listen"/> or
    /// <see cref="Connect"/>, before the first message.
    /// </summary>
    /// <param name="nodeId">NUMA node index (0-based).</param>
    /// <exception cref="TachyonException">mbind() failure.</exception>
    public void SetNumaNode(int nodeId)
    {
        ThrowIfDisposed();
        TachyonException.ThrowIfError(
            TachyonNative.tachyon_bus_set_numa_node(_bus, nodeId), nameof(TachyonNative.tachyon_bus_set_numa_node));
    }

    /// <summary>
    /// Controls the RX polling strategy. Call before the first message.
    /// When <paramref name="pureSpin"/> is <c>true</c>, the producer skips the futex wake check
    /// on every flush. Only use this when the consumer thread is dedicated and never parks.
    /// </summary>
    /// <param name="pureSpin"><c>true</c> for busy-spin (lowest latency), <c>false</c> for adaptive.</param>
    public void SetPollingMode(bool pureSpin)
    {
        ThrowIfDisposed();
        TachyonNative.tachyon_bus_set_polling_mode(_bus, pureSpin ? 1 : 0);
    }

    /// <summary>Current bus state.</summary>
    public TachyonState State
    {
        get
        {
            ThrowIfDisposed();
            return TachyonNative.tachyon_get_state(_bus);
        }
    }

    /// <summary>
    /// Non-blocking TX slot acquisition. Returns <c>false</c> if the ring is full.
    /// The returned <see cref="TxGuard"/> must be committed or rolled back before the next call.
    /// </summary>
    /// <param name="maxPayloadSize">Maximum payload size in bytes to reserve.</param>
    /// <param name="guard">Write-side guard, valid only if the method returns <c>true</c>.</param>
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public bool TryAcquireTx(nuint maxPayloadSize, out TxGuard guard)
    {
        ThrowIfDisposed();
        var ptr = TachyonNative.tachyon_acquire_tx(_bus, maxPayloadSize);
        if (ptr == null)
        {
            guard = default;
            return false;
        }

        guard = new TxGuard(_bus, ptr, maxPayloadSize);
        return true;
    }

    /// <summary>
    /// Issues a store-release barrier and wakes a blocking receiver if needed.
    /// Call after a sequence of <c>CommitUnflushed</c> writes to publish them atomically.
    /// </summary>
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public void Flush()
    {
        ThrowIfDisposed();
        TachyonNative.tachyon_flush(_bus);
    }

    /// <summary>
    /// Non-blocking receive. Returns <c>false</c> if the ring is empty.
    /// </summary>
    /// <param name="guard">Read-side guard, valid only if the method returns <c>true</c>.</param>
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public bool TryReceive(out RxGuard guard)
    {
        ThrowIfDisposed();
        uint typeId;
        nuint actualSize;
        var ptr = TachyonNative.tachyon_acquire_rx(_bus, &typeId, &actualSize);
        if (ptr == null)
        {
            guard = default;
            return false;
        }

        guard = new RxGuard(_bus, ptr, actualSize, typeId);
        return true;
    }

    /// <summary>
    /// Spins up to <paramref name="maxSpins"/> times then returns <c>false</c> if no message arrived.
    /// </summary>
    /// <param name="maxSpins">Maximum spin iterations before giving up.</param>
    /// <param name="guard">Read-side guard, valid only if the method returns <c>true</c>.</param>
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public bool ReceiveSpin(uint maxSpins, out RxGuard guard)
    {
        ThrowIfDisposed();
        uint typeId;
        nuint actualSize;
        var ptr = TachyonNative.tachyon_acquire_rx_spin(_bus, &typeId, &actualSize, maxSpins);
        if (ptr == null)
        {
            guard = default;
            return false;
        }

        guard = new RxGuard(_bus, ptr, actualSize, typeId);
        return true;
    }

    /// <summary>
    /// Blocking receive. Spins up to <paramref name="spinThreshold"/> times then parks on a futex
    /// until a message arrives. Never returns <c>null</c>.
    /// </summary>
    /// <param name="spinThreshold">Spin iterations before sleeping.</param>
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public RxGuard ReceiveBlocking(uint spinThreshold = 1000)
    {
        ThrowIfDisposed();
        uint typeId;
        nuint actualSize;
        var ptr = TachyonNative.tachyon_acquire_rx_blocking(_bus, &typeId, &actualSize, spinThreshold);
        return new RxGuard(_bus, ptr, actualSize, typeId);
    }

    /// <summary>
    /// Non-blocking batch receive. Drains up to <paramref name="maxMsgs"/> messages into
    /// a caller-allocated view buffer. Returns an empty guard if the ring is empty.
    /// </summary>
    /// <param name="views">Caller-allocated view buffer, typically <c>stackalloc TachyonMsgView[N]</c>.</param>
    /// <param name="maxMsgs">Capacity of <paramref name="views"/>.</param>
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public RxBatchGuard TryReceiveBatch(TachyonMsgView* views, int maxMsgs)
    {
        ThrowIfDisposed();
        var count = TachyonNative.tachyon_acquire_rx_batch(_bus, views, (nuint)maxMsgs);
        return new RxBatchGuard(_bus, views, count);
    }

    /// <summary>
    /// Blocking batch receive. Spins then parks until at least one message is available,
    /// then drains up to <paramref name="maxMsgs"/>.
    /// </summary>
    /// <param name="views">Caller-allocated view buffer, typically <c>stackalloc TachyonMsgView[N]</c>.</param>
    /// <param name="maxMsgs">Capacity of <paramref name="views"/>.</param>
    /// <param name="spinThreshold">Spin iterations before sleeping.</param>
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public RxBatchGuard DrainBatch(TachyonMsgView* views, int maxMsgs, uint spinThreshold = 1000)
    {
        ThrowIfDisposed();
        var count = TachyonNative.tachyon_drain_batch(_bus, views, (nuint)maxMsgs, spinThreshold);
        return new RxBatchGuard(_bus, views, count);
    }

    /// <summary>Issues an acquire barrier. Use before reading data received out-of-band.</summary>
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static void MemoryBarrierAcquire() =>
        TachyonNative.tachyon_memory_barrier_acquire();

    /// <summary>
    /// Increments the native refcount. Advanced use only, required when sharing a bus handle
    /// across ownership boundaries.
    /// </summary>
    public void AddRef()
    {
        ThrowIfDisposed();
        TachyonNative.tachyon_bus_ref(_bus);
    }

    /// <summary>Destroys the native bus, releasing SHM and FDs. Idempotent.</summary>
    public void Dispose()
    {
        var bus = Interlocked.Exchange(ref _bus, nint.Zero);
        if (bus != nint.Zero)
            TachyonNative.tachyon_bus_destroy(bus);
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    private void ThrowIfDisposed()
    {
        if (_bus == nint.Zero)
            throw new ObjectDisposedException(nameof(Bus));
    }
}