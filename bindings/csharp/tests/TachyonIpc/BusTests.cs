using System.Collections.Concurrent;
using TachyonIpc;
using TachyonIpc.Native;
using Xunit;

namespace TachyonIpc.Tests;

public sealed unsafe class BusTests : IDisposable
{
    private readonly string _socketPath;

    public BusTests()
    {
        _socketPath = Path.Combine(Path.GetTempPath(), $"tachyon_test_{Guid.NewGuid():N}.sock");
    }

    public void Dispose()
    {
        if (File.Exists(_socketPath)) File.Delete(_socketPath);
    }

    private static Thread StartListener(string path, nuint capacity, Action<Bus> work)
    {
        var t = new Thread(() =>
        {
            using var bus = Bus.Listen(path, capacity);
            work(bus);
        }) { IsBackground = true };
        t.Start();
        return t;
    }

    private static Bus ConnectWithRetry(string path, int maxAttempts = 200)
    {
        for (var i = 0; i < maxAttempts; i++)
        {
            try { return Bus.Connect(path); }
            catch (TachyonException e) when (e.Error == TachyonError.Network)
            {
                Thread.Sleep(10);
            }
        }
        throw new TimeoutException($"Could not connect to {path} after {maxAttempts} attempts.");
    }

    [Fact]
    public void TypeId_Make_RoundTrip()
    {
        var id = TypeId.Make(0xAB, 0xCD);
        Assert.Equal((ushort)0xAB, TypeId.RouteId(id));
        Assert.Equal((ushort)0xCD, TypeId.MsgType(id));
    }

    [Fact]
    public void TypeId_Make_RouteZero_PreservesV3Compat()
    {
        var id = TypeId.Make(0, 42);
        Assert.Equal(42u, id);
    }

    [Fact]
    public void TypeId_Make_MaxValues()
    {
        var id = TypeId.Make(0xFFFF, 0xFFFF);
        Assert.Equal(0xFFFFFFFFu, id);
        Assert.Equal(0xFFFF, TypeId.RouteId(id));
        Assert.Equal(0xFFFF, TypeId.MsgType(id));
    }

    [Fact]
    public void Listen_And_Connect_Succeed()
    {
        Bus? listener = null;
        var t = StartListener(_socketPath, 1024 * 1024, bus => { listener = bus; });
        Thread.Sleep(20);
        using var client = ConnectWithRetry(_socketPath);
        t.Join(2000);
        Assert.NotNull(client);
    }

    [Fact]
    public void Dispose_IsIdempotent()
    {
        Bus? captured = null;
        var t = StartListener(_socketPath, 1024 * 1024, bus => { captured = bus; });
        Thread.Sleep(20);
        using var client = ConnectWithRetry(_socketPath);
        t.Join(2000);
        client.Dispose();
        client.Dispose();
    }

    [Fact]
    public void ThrowsAfterDispose()
    {
        var t = StartListener(_socketPath, 1024 * 1024, _ => { });
        Thread.Sleep(20);
        var client = ConnectWithRetry(_socketPath);
        t.Join(2000);
        client.Dispose();
        Assert.Throws<ObjectDisposedException>(() => client.State);
    }

    [Fact]
    public void TryAcquireTx_ReturnsFalse_WhenFull()
    {
        var t = StartListener(_socketPath, 4096, _ => { });
        Thread.Sleep(20);
        using var client = ConnectWithRetry(_socketPath);
        t.Join(2000);

        var full = false;
        for (var i = 0; i < 1000; i++)
        {
            if (!client.TryAcquireTx(64, out var tx)) { full = true; break; }
            using (tx) tx.Commit(64, 0, 1);
        }
        Assert.True(full);
    }

    [Fact]
    public void TxGuard_Rollback_OnDispose_WhenNotCommitted()
    {
        var t = StartListener(_socketPath, 1024 * 1024, _ => { });
        Thread.Sleep(20);
        using var client = ConnectWithRetry(_socketPath);
        t.Join(2000);

        Assert.True(client.TryAcquireTx(64, out var tx));
        tx.Dispose();

        Assert.True(client.TryAcquireTx(64, out var tx2));
        using (tx2) tx2.Commit(8, 0, 1);
    }

    [Fact]
    public void SendAndReceive_SingleMessage_RoundTrip()
    {
        var received = new BlockingCollection<(uint typeId, byte[] data)>(1);
        byte[] payload = [1, 2, 3, 4, 5];
        var typeId = TypeId.Make(0, 7);

        var t = StartListener(_socketPath, 1024 * 1024, bus =>
        {
            using var rx = bus.ReceiveBlocking(spinThreshold: 10_000);
            received.Add((rx.TypeId, rx.Data.ToArray()));
        });

        Thread.Sleep(20);
        using var client = ConnectWithRetry(_socketPath);
        Assert.True(client.TryAcquireTx((nuint)payload.Length, out var tx));
        using (tx) { payload.CopyTo(tx.Buffer); tx.Commit(payload.Length, typeId); }
        client.Flush();

        var msg = received.Take();
        t.Join(2000);

        Assert.Equal(typeId, msg.typeId);
        Assert.Equal((ushort)0, TypeId.RouteId(msg.typeId));
        Assert.Equal((ushort)7, TypeId.MsgType(msg.typeId));
        Assert.Equal(payload, msg.data);
    }

    [Fact]
    public void TryReceive_ReturnsFalse_WhenEmpty()
    {
        var t = StartListener(_socketPath, 1024 * 1024, _ => { });
        Thread.Sleep(20);
        using var client = ConnectWithRetry(_socketPath);
        t.Join(2000);
        Assert.False(client.TryReceive(out _));
    }

    [Fact]
    public void TryReceive_ReturnsTrue_WhenMessageAvailable()
    {
        var received = new BlockingCollection<ushort>(1);

        var t = StartListener(_socketPath, 1024 * 1024, bus =>
        {
            RxGuard rx = default;
            var got = false;
            for (var i = 0; i < 1_000_000 && !got; i++) got = bus.TryReceive(out rx);
            Assert.True(got);
            using (rx) received.Add(rx.MsgType);
        });

        Thread.Sleep(20);
        using var client = ConnectWithRetry(_socketPath);

        Assert.True(client.TryAcquireTx(8, out var tx));
        using (tx) tx.Commit(8, 0, 42);
        client.Flush();

        var msgType = received.Take();
        t.Join(2000);
        Assert.Equal((ushort)42, msgType);
    }

    [Fact]
    public void Batch_IsEmpty_WhenNoMessages()
    {
        var t = StartListener(_socketPath, 1024 * 1024, bus =>
        {
            var views = stackalloc TachyonMsgView[16];
            using var batch = bus.TryReceiveBatch(views, 16);
            Assert.True(batch.IsEmpty);
            Assert.Equal(0, batch.Count);
        });

        Thread.Sleep(20);
        using var client = ConnectWithRetry(_socketPath);
        t.Join(2000);
    }

    [Fact]
    public void Batch_Receive_MultipleMessages()
    {
        const int N = 8;
        var results = new BlockingCollection<ushort[]>(1);

        var t = StartListener(_socketPath, 1024 * 1024, bus =>
        {
            var views = stackalloc TachyonMsgView[N];
            RxBatchGuard batch = default;
            var found = false;
            for (var attempt = 0; attempt < 1_000_000; attempt++)
            {
                batch = bus.TryReceiveBatch(views, N);
                if (!batch.IsEmpty) { found = true; break; }
                batch.Dispose();
            }
            Assert.True(found, "Batch never became visible after 1M spins");
            using (batch)
            {
                var types = new ushort[batch.Count];
                var idx = 0;
                foreach (var msg in batch) types[idx++] = msg.MsgType;
                results.Add(types);
            }
        });

        Thread.Sleep(20);
        using var client = ConnectWithRetry(_socketPath);

        for (ushort i = 0; i < N; i++)
        {
            Assert.True(client.TryAcquireTx(4, out var tx));
            using (tx) tx.Commit(4, 0, i);
        }
        client.Flush();

        var msgTypes = results.Take();
        t.Join(2000);

        Assert.Equal(N, msgTypes.Length);
        for (var i = 0; i < N; i++)
            Assert.Equal((ushort)i, msgTypes[i]);
    }

    [Fact]
    public void State_IsReady_AfterConnect()
    {
        TachyonState listenerState = default;
        var t = StartListener(_socketPath, 1024 * 1024, bus =>
        {
            listenerState = bus.State;
        });

        Thread.Sleep(20);
        using var client = ConnectWithRetry(_socketPath);
        var clientState = client.State;
        t.Join(2000);

        Assert.Equal(TachyonState.Ready, clientState);
        Assert.Equal(TachyonState.Ready, listenerState);
    }
}