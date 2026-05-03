using System.Runtime.CompilerServices;
using TachyonIpc.Native;

namespace TachyonIpc;

/// <summary>
/// Managed wrapper around a native <c>tachyon_rpc_bus_t</c>.
/// Not thread-safe: caller side (<see cref="Call"/>, <see cref="Wait"/>) and callee side
/// (<see cref="Serve"/>, <see cref="Reply"/>) must each be accessed from a single thread.
/// </summary>
public sealed unsafe class RpcBus : IDisposable
{
    private nint _rpc;

    static RpcBus() => NativeLoader.Register();

    private RpcBus(nint rpc) => _rpc = rpc;

    /// <summary>
    /// Creates two SHM arenas and blocks until a caller connects via <paramref name="socketPath"/>.
    /// EINTR is retried transparently.
    /// </summary>
    /// <param name="socketPath">UDS socket path. Unlinked automatically after handshake.</param>
    /// <param name="capFwd">arena_fwd capacity in bytes (caller to callee). Must be a power of two.</param>
    /// <param name="capRev">arena_rev capacity in bytes (callee to caller). Must be a power of two.</param>
    /// <exception cref="TachyonException">SHM creation or socket binding failure.</exception>
    public static RpcBus Listen(string socketPath, nuint capFwd, nuint capRev)
    {
        nint rpc;
        TachyonError err;
        do
        {
            err = TachyonNative.tachyon_rpc_listen(socketPath, capFwd, capRev, &rpc);
        } while (err == TachyonError.Interrupted);

        TachyonException.ThrowIfError(err, nameof(TachyonNative.tachyon_rpc_listen));
        return new RpcBus(rpc);
    }

    /// <summary>
    /// Attaches to existing SHM arenas via the UDS socket at <paramref name="socketPath"/>.
    /// </summary>
    /// <param name="socketPath">UDS socket path of the listener.</param>
    /// <exception cref="TachyonException">
    /// <see cref="TachyonError.AbiMismatch"/> if the callee was compiled with a different
    /// Tachyon version or <c>TACHYON_MSG_ALIGNMENT</c>.
    /// </exception>
    public static RpcBus Connect(string socketPath)
    {
        nint rpc;
        TachyonException.ThrowIfError(
            TachyonNative.tachyon_rpc_connect(socketPath, &rpc),
            nameof(TachyonNative.tachyon_rpc_connect));
        return new RpcBus(rpc);
    }

    /// <summary>
    /// Controls the polling back-off strategy for both arenas.
    /// Call before the first message.
    /// When <paramref name="pureSpin"/> is <c>true</c>, the producer skips the futex wake check
    /// on every flush. Only use this when both consumer threads are dedicated and never park.
    /// </summary>
    /// <param name="pureSpin"><c>true</c> for busy-spin, <c>false</c> for adaptive futex mode.</param>
    public void SetPollingMode(bool pureSpin)
    {
        ThrowIfDisposed();
        TachyonNative.tachyon_rpc_set_polling_mode(_rpc, pureSpin ? 1 : 0);
    }

    /// <summary>Current composite bus state. Returns <see cref="TachyonState.FatalError"/> if
    /// either arena has entered a fatal error state.</summary>
    public TachyonState State
    {
        get
        {
            ThrowIfDisposed();
            return TachyonNative.tachyon_rpc_get_state(_rpc);
        }
    }

    /// <summary>
    /// Copies <paramref name="payload"/> into arena_fwd and returns the assigned correlation ID.
    /// Blocks if the ring buffer is full.
    /// </summary>
    /// <param name="payload">Request payload.</param>
    /// <param name="msgType">Application-level message type (uint16 range).</param>
    /// <returns>Assigned correlation ID, always greater than zero.</returns>
    /// <exception cref="TachyonException">Commit failure or internal error.</exception>
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public ulong Call(ReadOnlySpan<byte> payload, uint msgType)
    {
        ThrowIfDisposed();
        ulong cid;
        fixed (byte* p = payload)
        {
            TachyonException.ThrowIfError(
                TachyonNative.tachyon_rpc_call(_rpc, p, (nuint)payload.Length, msgType, &cid),
                nameof(TachyonNative.tachyon_rpc_call));
        }

        return cid;
    }

    /// <summary>
    /// Blocks until the response matching <paramref name="correlationId"/> arrives in arena_rev,
    /// spinning then falling back to futex sleep.
    /// A correlation ID mismatch triggers a fatal error on arena_rev.
    /// </summary>
    /// <param name="correlationId">The ID returned by <see cref="Call"/>.</param>
    /// <param name="spinThreshold">Spin iterations before futex sleep.</param>
    /// <returns>An <see cref="RpcRxGuard"/> holding the response payload.</returns>
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public RpcRxGuard Wait(ulong correlationId, uint spinThreshold = 10_000)
    {
        ThrowIfDisposed();
        nuint actualSize;
        uint typeId;
        var ptr = TachyonNative.tachyon_rpc_wait(_rpc, correlationId, &actualSize, &typeId, spinThreshold);
        if (ptr == null)
            throw new TachyonException(TachyonNative.tachyon_rpc_get_state(_rpc) == TachyonState.FatalError
                    ? TachyonError.System
                    : TachyonError.Interrupted,
                nameof(TachyonNative.tachyon_rpc_wait));
        return new RpcRxGuard(_rpc, ptr, actualSize, typeId, correlationId, isServe: false);
    }

    /// <summary>
    /// Blocks until a request arrives in arena_fwd, spinning then falling back to futex sleep.
    /// Commit the returned guard before calling <see cref="Reply"/> to avoid holding both
    /// arena slots simultaneously.
    /// </summary>
    /// <param name="spinThreshold">Spin iterations before futex sleep.</param>
    /// <returns>An <see cref="RpcRxGuard"/> holding the request payload and correlation ID.</returns>
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public RpcRxGuard Serve(uint spinThreshold = 10_000)
    {
        ThrowIfDisposed();
        ulong correlationId;
        uint typeId;
        nuint actualSize;
        var ptr = TachyonNative.tachyon_rpc_serve(_rpc, &correlationId, &typeId, &actualSize, spinThreshold);
        if (ptr == null)
            throw new TachyonException(TachyonNative.tachyon_rpc_get_state(_rpc) == TachyonState.FatalError
                    ? TachyonError.System
                    : TachyonError.Interrupted,
                nameof(TachyonNative.tachyon_rpc_serve));
        return new RpcRxGuard(_rpc, ptr, actualSize, typeId, correlationId, isServe: true);
    }

    /// <summary>
    /// Copies <paramref name="payload"/> into arena_rev as a response to <paramref name="correlationId"/>.
    /// The <see cref="RpcRxGuard"/> from <see cref="Serve"/> must be committed before calling this method.
    /// </summary>
    /// <param name="correlationId">Must match the value from the served <see cref="RpcRxGuard"/>.</param>
    /// <param name="payload">Response payload.</param>
    /// <param name="msgType">Application-level response type (uint16 range).</param>
    /// <exception cref="TachyonException">
    /// <see cref="TachyonError.InvalidSize"/> if <paramref name="correlationId"/> is zero.
    /// </exception>
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public void Reply(ulong correlationId, ReadOnlySpan<byte> payload, uint msgType)
    {
        ThrowIfDisposed();
        fixed (byte* p = payload)
        {
            TachyonException.ThrowIfError(
                TachyonNative.tachyon_rpc_reply(_rpc, correlationId, p, (nuint)payload.Length, msgType),
                nameof(TachyonNative.tachyon_rpc_reply));
        }
    }

    /// <summary>Unmaps both SHM arenas and releases all resources. Idempotent.</summary>
    public void Dispose()
    {
        var rpc = Interlocked.Exchange(ref _rpc, nint.Zero);
        if (rpc != nint.Zero)
            TachyonNative.tachyon_rpc_destroy(rpc);
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    private void ThrowIfDisposed()
    {
        if (_rpc == nint.Zero)
            throw new ObjectDisposedException(nameof(RpcBus));
    }
}