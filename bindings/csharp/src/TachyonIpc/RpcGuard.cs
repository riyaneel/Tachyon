using TachyonIpc.Native;

namespace TachyonIpc;

/// <summary>
/// Read-side guard for a single RPC message, returned by <see cref="RpcBus.Wait"/> and
/// <see cref="RpcBus.Serve"/>. Must be used with <c>using</c>.
/// <see cref="Dispose"/> commits the slot and advances the read pointer.
/// </summary>
/// <remarks>
/// ref struct, stack-only, zero allocation.
/// The underlying shared memory is valid only until <see cref="Dispose"/> is called.
/// Never store a reference to <see cref="Data"/> beyond the guard's lifetime.
/// </remarks>
public unsafe ref struct RpcRxGuard
{
    private readonly nint _rpc;
    private readonly byte* _ptr;
    private readonly nuint _actualSize;
    private readonly bool _isServe;
    private bool _done;

    /// <summary>bits [15:0] = msg_type, bits [31:16] = route_id.</summary>
    public readonly uint TypeId;

    /// <summary>Decoded msg_type component of <see cref="TypeId"/>.</summary>
    public ushort MsgType => TachyonIpc.TypeId.MsgType(TypeId);

    /// <summary>Decoded route_id component of <see cref="TypeId"/>.</summary>
    public ushort RouteId => TachyonIpc.TypeId.RouteId(TypeId);

    /// <summary>
    /// Correlation ID of this message.
    /// On the callee side (produced by <see cref="RpcBus.Serve"/>), pass this value
    /// unchanged to <see cref="RpcBus.Reply"/>.
    /// </summary>
    public readonly ulong CorrelationId;

    internal RpcRxGuard(nint rpc, void* ptr, nuint actualSize, uint typeId, ulong correlationId, bool isServe)
    {
        _rpc = rpc;
        _ptr = (byte*)ptr;
        _actualSize = actualSize;
        _isServe = isServe;
        _done = false;
        TypeId = typeId;
        CorrelationId = correlationId;
    }

    /// <summary>
    /// Read-only view of the message payload.
    /// Valid only within the guard's scope.
    /// </summary>
    public ReadOnlySpan<byte> Data
    {
        get
        {
            if (_done) ThrowInvalidated();
            return new ReadOnlySpan<byte>(_ptr, checked((int)_actualSize));
        }
    }

    /// <summary>Advances the read pointer. Called automatically by <see cref="Dispose"/>.</summary>
    public void Commit()
    {
        if (_done) return;
        _done = true;
        var err = _isServe
            ? TachyonNative.tachyon_rpc_commit_serve(_rpc)
            : TachyonNative.tachyon_rpc_commit_rx(_rpc);
        TachyonException.ThrowIfError(err,
            _isServe
                ? nameof(TachyonNative.tachyon_rpc_commit_serve)
                : nameof(TachyonNative.tachyon_rpc_commit_rx));
    }

    /// <summary>Commits and releases the slot.</summary>
    public void Dispose() => Commit();

    private static void ThrowInvalidated() =>
        throw new InvalidOperationException("RpcRxGuard has already been committed.");
}