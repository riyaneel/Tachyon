using TachyonIpc.Native;

namespace TachyonIpc;

/// <summary>
/// Lightweight read-only view over a single message inside an <see cref="RxBatchGuard"/>.
/// Valid only within the parent guard's scope.
/// </summary>
public unsafe ref struct MsgView
{
    private readonly byte* _ptr;
    private readonly nuint _actualSize;

    /// <summary>bits [15:0] = msg_type, bits [31:16] = route_id.</summary>
    public readonly uint TypeId;

    public ushort MsgType => TachyonIpc.TypeId.MsgType(TypeId);
    public ushort RouteId => TachyonIpc.TypeId.RouteId(TypeId);

    /// <summary>
    /// Read-only view of the message payload.
    /// Do not retain beyond the parent <see cref="RxBatchGuard"/> scope.
    /// </summary>
    public ReadOnlySpan<byte> Data => new(_ptr, checked((int)_actualSize));

    internal MsgView(void* ptr, nuint actualSize, uint typeId)
    {
        _ptr = (byte*)ptr;
        _actualSize = actualSize;
        TypeId = typeId;
    }
}

/// <summary>
/// Read-side guard for a batch of messages.
/// Returned by Bus.TryReceiveBatch / Bus.DrainBatch.
/// Must be used with <c>using</c>.
/// Dispose() commits all slots at once.
/// </summary>
/// <remarks>
/// ref struct, stack-only, zero allocation.
/// All message pointers are valid only until Dispose() is called.
/// </remarks>
public unsafe ref struct RxBatchGuard
{
    private readonly nint _bus;
    private readonly TachyonMsgView* _views;
    private readonly nuint _count;
    private bool _done;

    /// <summary>Number of messages received in this batch.</summary>
    public int Count => (int)_count;

    /// <summary>True if the batch is empty.</summary>
    public bool IsEmpty => _count == 0;

    internal RxBatchGuard(nint bus, TachyonMsgView* views, nuint count)
    {
        _bus = bus;
        _views = views;
        _count = count;
        _done = false;
    }

    /// <summary>Returns the message at <paramref name="index"/>.</summary>
    public MsgView this[int index]
    {
        get
        {
            if (_done)
                ThrowInvalidated();
            if ((uint)index >= _count)
                throw new ArgumentOutOfRangeException(nameof(index));
            ref var v = ref _views[index];
            return new MsgView(v.Ptr, v.ActualSize, v.TypeId);
        }
    }

    /// <summary>Commits all slots in the batch. Called automatically by Dispose.</summary>
    public void Commit()
    {
        if (_done)
            return;
        if (_count == 0)
            return;
        TachyonException.ThrowIfError(
            TachyonNative.tachyon_commit_rx_batch(_bus, _views, _count), nameof(TachyonNative.tachyon_commit_rx_batch)
        );
        _done = true;
    }

    /// <summary>Commits all slots and releases the batch.</summary>
    public void Dispose() => Commit();

    /// <summary>Supports <c>foreach</c> iteration without allocation.</summary>
    public Enumerator GetEnumerator() => new(this);

    public ref struct Enumerator
    {
        private readonly RxBatchGuard _guard;
        private int _index;

        internal Enumerator(RxBatchGuard guard)
        {
            _guard = guard;
            _index = -1;
        }

        public bool MoveNext() => ++_index < _guard.Count;

        public MsgView Current => _guard[_index];
    }

    private static void ThrowInvalidated() =>
        throw new InvalidOperationException("RxBatchGuard has already been committed.");
}