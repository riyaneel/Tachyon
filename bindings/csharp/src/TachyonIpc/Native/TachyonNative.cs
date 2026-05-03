using System.Runtime.InteropServices;

namespace TachyonIpc.Native;

public enum TachyonError
{
    Success = 0,
    NullPtr = 1,
    Mem = 2,
    Open = 3,
    Truncate = 4,
    Chmod = 5,
    Seal = 6,
    Map = 7,
    InvalidSize = 8,
    Full = 9,
    Empty = 10,
    Network = 11,
    System = 12,
    Interrupted = 13,
    AbiMismatch = 14
}

public enum TachyonState
{
    Uninitialized = 0,
    Initializing = 1,
    Ready = 2,
    Disconnected = 3,
    FatalError = 4,
    Unknown = 5,
}

[StructLayout(LayoutKind.Sequential)]
public unsafe struct TachyonMsgView
{
    public void* Ptr;
    public nuint ActualSize;
    public nuint Reserved;
    public uint TypeId;
    public uint Padding;
}

internal static unsafe partial class TachyonNative
{
    private const string Lib = "tachyon";

    [LibraryImport(Lib)]
    internal static partial void tachyon_memory_barrier_acquire();

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial TachyonError tachyon_bus_listen(
        string socketPath,
        nuint capacity,
        nint* outBus);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial TachyonError tachyon_bus_connect(
        string socketPath,
        nint* outBus);

    [LibraryImport(Lib)]
    internal static partial void tachyon_bus_ref(nint bus);

    [LibraryImport(Lib)]
    internal static partial void tachyon_bus_destroy(nint bus);

    [LibraryImport(Lib)]
    internal static partial TachyonError tachyon_bus_set_numa_node(nint bus, int nodeId);

    [LibraryImport(Lib)]
    internal static partial void tachyon_bus_set_polling_mode(nint bus, int pureSpin);

    [LibraryImport(Lib)]
    internal static partial void* tachyon_acquire_tx(nint bus, nuint maxPayloadSize);

    [LibraryImport(Lib)]
    internal static partial TachyonError tachyon_commit_tx(
        nint bus,
        nuint actualPayloadSize,
        uint typeId);

    [LibraryImport(Lib)]
    internal static partial TachyonError tachyon_rollback_tx(nint bus);

    [LibraryImport(Lib)]
    internal static partial void tachyon_flush(nint bus);

    [LibraryImport(Lib)]
    internal static partial void* tachyon_acquire_rx(
        nint bus,
        uint* outTypeId,
        nuint* outActualSize);

    [LibraryImport(Lib)]
    internal static partial void* tachyon_acquire_rx_spin(
        nint bus,
        uint* outTypeId,
        nuint* outActualSize,
        uint maxSpins);

    [LibraryImport(Lib)]
    internal static partial void* tachyon_acquire_rx_blocking(
        nint bus,
        uint* outTypeId,
        nuint* outActualSize,
        uint spinThreshold);

    [LibraryImport(Lib)]
    internal static partial TachyonError tachyon_commit_rx(nint bus);

    [LibraryImport(Lib)]
    internal static partial nuint tachyon_acquire_rx_batch(
        nint bus,
        TachyonMsgView* outViews,
        nuint maxMsgs);

    [LibraryImport(Lib)]
    internal static partial nuint tachyon_drain_batch(
        nint bus,
        TachyonMsgView* outViews,
        nuint maxMsgs,
        uint spinThreshold);

    [LibraryImport(Lib)]
    internal static partial TachyonError tachyon_commit_rx_batch(
        nint bus,
        TachyonMsgView* views,
        nuint count);

    [LibraryImport(Lib)]
    internal static partial TachyonState tachyon_get_state(nint bus);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial TachyonError tachyon_rpc_listen(
        string socketPath,
        nuint capFwd,
        nuint capRev,
        nint* outRpc);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial TachyonError tachyon_rpc_connect(
        string socketPath,
        nint* outRpc);

    [LibraryImport(Lib)]
    internal static partial void tachyon_rpc_destroy(nint rpc);

    [LibraryImport(Lib)]
    internal static partial TachyonError tachyon_rpc_call(
        nint rpc,
        void* payload,
        nuint size,
        uint msgType,
        ulong* outCorrelationId);

    [LibraryImport(Lib)]
    internal static partial void* tachyon_rpc_wait(
        nint rpc,
        ulong correlationId,
        nuint* outSize,
        uint* outMsgType,
        uint spinThreshold);

    [LibraryImport(Lib)]
    internal static partial TachyonError tachyon_rpc_commit_rx(nint rpc);

    [LibraryImport(Lib)]
    internal static partial void* tachyon_rpc_serve(
        nint rpc,
        ulong* outCorrelationId,
        uint* outMsgType,
        nuint* outSize,
        uint spinThreshold);

    [LibraryImport(Lib)]
    internal static partial TachyonError tachyon_rpc_commit_serve(nint rpc);

    [LibraryImport(Lib)]
    internal static partial TachyonError tachyon_rpc_reply(
        nint rpc,
        ulong correlationId,
        void* payload,
        nuint size,
        uint msgType);

    [LibraryImport(Lib)]
    internal static partial void* tachyon_rpc_acquire_tx(nint rpc, nuint maxSize);

    [LibraryImport(Lib)]
    internal static partial TachyonError tachyon_rpc_commit_call(
        nint rpc,
        nuint actualSize,
        uint msgType,
        ulong* outCid);

    [LibraryImport(Lib)]
    internal static partial void tachyon_rpc_rollback_call(nint rpc);

    [LibraryImport(Lib)]
    internal static partial void* tachyon_rpc_acquire_reply_tx(nint rpc, nuint maxSize);

    [LibraryImport(Lib)]
    internal static partial TachyonError tachyon_rpc_commit_reply(
        nint rpc,
        ulong cid,
        nuint actualSize,
        uint msgType);

    [LibraryImport(Lib)]
    internal static partial void tachyon_rpc_rollback_reply(nint rpc);

    [LibraryImport(Lib)]
    internal static partial void tachyon_rpc_set_polling_mode(nint rpc, int pureSpin);

    [LibraryImport(Lib)]
    internal static partial TachyonState tachyon_rpc_get_state(nint rpc);
}