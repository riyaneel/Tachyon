using System.Runtime.CompilerServices;
using TachyonIpc.Native;

namespace TachyonIpc;

/// <summary>
/// Managed wrapper around a native <c>tachyon_bus_t</c>.
/// Thread-safety: a single Bus instance must be used from one thread only (Same contract as underlying C API).
/// </summary>
public sealed unsafe class Bus : IDisposable
{
    private nint _bus;

    static Bus() => NativeLoader.Register();

    private Bus(nint bus) => _bus = bus;

    public static Bus Listen(string socketPath, nuint capacity)
    {
        nint bus;
        TachyonException.ThrowIfError(
            TachyonNative.tachyon_bus_listen(socketPath, capacity, &bus), nameof(TachyonNative.tachyon_bus_listen));
        return new Bus(bus);
    }

    public static Bus Connect(string socketPath)
    {
        nint bus;
        TachyonException.ThrowIfError(
            TachyonNative.tachyon_bus_connect(socketPath, &bus), nameof(TachyonNative.tachyon_bus_connect));
        return new Bus(bus);
    }

    public void SetNumaNode(int nodeId)
    {
        ThrowIfDisposed();
        TachyonException.ThrowIfError(
            TachyonNative.tachyon_bus_set_numa_node(_bus, nodeId), nameof(TachyonNative.tachyon_bus_set_numa_node));
    }

    public void SetPollingMode(bool pureSpin)
    {
        ThrowIfDisposed();
        TachyonNative.tachyon_bus_set_polling_mode(_bus, pureSpin ? 1 : 0);
    }

    public TachyonState State
    {
        get
        {
            ThrowIfDisposed();
            return TachyonNative.tachyon_get_state(_bus);
        }
    }

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


    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public void Flush()
    {
        ThrowIfDisposed();
        TachyonNative.tachyon_flush(_bus);
    }

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

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public RxGuard ReceiveBlocking(uint spinThreshold = 1000)
    {
        ThrowIfDisposed();
        uint typeId;
        nuint actualSize;
        var ptr = TachyonNative.tachyon_acquire_rx_blocking(_bus, &typeId, &actualSize, spinThreshold);
        // acquire_rx_blocking never returns null — it blocks until a message is available
        return new RxGuard(_bus, ptr, actualSize, typeId);
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public RxBatchGuard TryReceiveBatch(TachyonMsgView* views, int maxMsgs)
    {
        ThrowIfDisposed();
        var count = TachyonNative.tachyon_acquire_rx_batch(_bus, views, (nuint)maxMsgs);
        return new RxBatchGuard(_bus, views, count);
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public RxBatchGuard DrainBatch(TachyonMsgView* views, int maxMsgs, uint spinThreshold = 1000)
    {
        ThrowIfDisposed();
        var count = TachyonNative.tachyon_drain_batch(_bus, views, (nuint)maxMsgs, spinThreshold);
        return new RxBatchGuard(_bus, views, count);
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static void MemoryBarrierAcquire() =>
        TachyonNative.tachyon_memory_barrier_acquire();

    public void AddRef()
    {
        ThrowIfDisposed();
        TachyonNative.tachyon_bus_ref(_bus);
    }

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