using TachyonIpc.Native;

namespace TachyonIpc;

/// <summary>
/// Read-side guard for a single message, returned by Bus.TryReceive / Bus.ReceiveSpin / Bus.ReceiveBlocking.
/// Must be used with <c>using</c>.
/// Dispose() commits (advances the read pointer).
/// </summary>
/// <remarks>
/// ref struct, stack-only, zero allocation.
/// The underlying shared memory is valid only until Dispose() is called.
/// Never store a reference to <see cref="Data"/> beyond the guard's lifetime.
/// </remarks>
public unsafe ref struct RxGuard
{
    private readonly nint _bus;
    private readonly byte* _ptr;
    private readonly nuint _actualSize;
    private bool _done;

    /// <summary>bits [15:0] = msg_type, bits [31:16] = route_id.</summary>
    public readonly uint TypeId;

    /// <summary>Decoded msg_type component of <see cref="TypeId"/>.</summary>
    public ushort MsgType => TachyonIpc.TypeId.MsgType(TypeId);

    /// <summary>Decoded route_id component of <see cref="TypeId"/>.</summary>
    public ushort RouteId => TachyonIpc.TypeId.RouteId(TypeId);

    internal RxGuard(nint bus, void* ptr, nuint actualSize, uint typeId)
    {
        _bus = bus;
        _ptr = (byte*)ptr;
        _actualSize = actualSize;
        _done = false;
        TypeId = typeId;
    }

    /// <summary>
    /// Read-only view of the message payload.
    /// Valid only within the guard's scope (do not retain after Dispose()).
    /// </summary>
    public ReadOnlySpan<byte> Data
    {
        get
        {
            if (_done) ThrowInvalidated();
            return new ReadOnlySpan<byte>(_ptr, checked((int)_actualSize));
        }
    }

    /// <summary>Advances the read pointer. Called automatically by Dispose.</summary>
    public void Commit()
    {
        if (_done) return;
        TachyonException.ThrowIfError(
            TachyonNative.tachyon_commit_rx(_bus),
            nameof(TachyonNative.tachyon_commit_rx));
        _done = true;
    }

    /// <summary>Commits and releases the slot.</summary>
    public void Dispose() => Commit();

    private static void ThrowInvalidated() =>
        throw new InvalidOperationException("RxGuard has already been committed.");
}