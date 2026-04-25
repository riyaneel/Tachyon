using TachyonIpc.Native;

namespace TachyonIpc;

/// <summary>
/// Write-side guard returned by <see cref="Bus.AcquireTx"/>.
/// Must be used with <c>using</c>. Dispose() rolls back if Commit() was not called.
/// </summary>
/// <remarks>ref struct. cannot escape to heap, enforces stack-only usage.</remarks>
public unsafe ref struct TxGuard
{
    private readonly nint _bus;
    private readonly byte* _ptr;
    private readonly nuint _maxSize;
    private bool _done;

    internal TxGuard(nint bus, void* ptr, nuint maxSize)
    {
        _bus = bus;
        _ptr = (byte*)ptr;
        _maxSize = maxSize;
        _done = false;
    }

    /// <summary>Writable buffer up to the reserved capacity.</summary>
    public Span<byte> Buffer
    {
        get
        {
            if (_done)
                ThrowInvalidated();
            return new Span<byte>(_ptr, checked((int)_maxSize));
        }
    }

    /// <summary>
    /// Commits the message.
    /// <paramref name="actualSize"/> must be ≤ reserved capacity.
    /// <paramref name="typeId"/> should be built with <see cref="TypeId.Make"/>.
    /// </summary>
    public void Commit(int actualSize, uint typeId)
    {
        if (_done)
            ThrowInvalidated();

        if (actualSize < 0 || (nuint)actualSize > _maxSize)
            throw new ArgumentOutOfRangeException(nameof(actualSize),
                $"actualSize {actualSize} exceeds reserved capacity {_maxSize}.");

        TachyonException.ThrowIfError(
            TachyonNative.tachyon_commit_tx(_bus, (nuint)actualSize, typeId),
            nameof(TachyonNative.tachyon_commit_tx));

        _done = true;
    }

    /// <summary>Convenience overload. Builds type_id from route and message type.</summary>
    public void Commit(int actualSize, ushort routeId, ushort msgType) =>
        Commit(actualSize, TypeId.Make(routeId, msgType));

    /// <summary>
    /// Rolls back the acquired slot.
    /// Called automatically by Dispose if Commit was not called.
    /// </summary>
    public void Rollback()
    {
        if (_done)
            return;
        TachyonNative.tachyon_rollback_tx(_bus);
        _done = true;
    }

    /// <summary>Rolls back if Commit was not called.</summary>
    public void Dispose() => Rollback();

    private static void ThrowInvalidated() =>
        throw new InvalidOperationException("TxGuard has already been committed or rolled back.");
}