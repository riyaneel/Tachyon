using TachyonIpc.Native;
using Xunit;

namespace TachyonIpc.Tests;

public sealed unsafe class RpcBusTests
{
    private static string Sock(string name) =>
        Path.Combine(Path.GetTempPath(), $"tachyon_rpc_{name}_{Guid.NewGuid():N}.sock");

    private static RpcBus ConnectWithRetry(string path, int maxRetries = 100)
    {
        for (int i = 0; i < maxRetries; i++)
        {
            try
            {
                return RpcBus.Connect(path);
            }
            catch (TachyonException e) when (e.Error == TachyonError.Network)
            {
                Thread.Sleep(10);
            }
        }

        throw new InvalidOperationException($"RpcBus.Connect timed out on {path}");
    }

    [Fact]
    public void Handshake_BothSidesReady()
    {
        var path = Sock("handshake");
        RpcBus? callee = null;
        var t = new Thread(() => callee = RpcBus.Listen(path, 1 << 16, 1 << 16));
        t.Start();

        using var caller = ConnectWithRetry(path);
        t.Join(2000);

        using (callee!)
        {
            Assert.Equal(TachyonState.Ready, caller.State);
            Assert.Equal(TachyonState.Ready, callee.State);
        }
    }

    [Fact]
    public void Call_Wait_Roundtrip()
    {
        var path = Sock("roundtrip");
        RpcBus? callee = null;
        var t = new Thread(() =>
        {
            callee = RpcBus.Listen(path, 1 << 16, 1 << 16);
            using var rx = callee.Serve();
            var cid = rx.CorrelationId;
            var echo = rx.Data.ToArray();
            Array.Reverse(echo);
            rx.Dispose();
            callee.Reply(cid, echo, rx.MsgType + 1U);
        });
        t.Start();

        using var caller = ConnectWithRetry(path);
        var payload = "hello_tachyon_cs"u8.ToArray();
        var cid = caller.Call(payload, msgType: 1);

        Assert.True(cid > 0);

        using var resp = caller.Wait(cid);
        Assert.Equal(2u, resp.MsgType);
        Array.Reverse(payload);
        Assert.Equal(payload, resp.Data.ToArray());

        t.Join(2000);
        callee!.Dispose();
    }

    [Fact]
    public void Serve_Reply_TypeIdPreserved()
    {
        var path = Sock("typeid");
        uint receivedMsgType = 0;
        RpcBus? callee = null;

        var t = new Thread(() =>
        {
            callee = RpcBus.Listen(path, 1 << 16, 1 << 16);
            using var rx = callee.Serve();
            receivedMsgType = rx.MsgType;
            var cid = rx.CorrelationId;
            rx.Dispose();
            callee.Reply(cid, new byte[] { 0x01 }, msgType: 99);
        });
        t.Start();

        using var caller = ConnectWithRetry(path);
        var cid = caller.Call(new byte[] { 0x01 }, msgType: 42);
        using var resp = caller.Wait(cid);

        Assert.Equal(99u, resp.MsgType);
        t.Join(2000);
        Assert.Equal(42u, receivedMsgType);
        callee!.Dispose();
    }

    [Fact]
    public void CorrelationId_Monotonic()
    {
        const int n = 4;
        var path = Sock("cid_mono");
        RpcBus? callee = null;

        var t = new Thread(() =>
        {
            callee = RpcBus.Listen(path, 1 << 16, 1 << 16);
            for (int i = 0; i < n; i++)
            {
                using var rx = callee.Serve();
                var cid = rx.CorrelationId;
                rx.Dispose();
                callee.Reply(cid, new byte[] { 0x01 }, msgType: 0);
            }
        });
        t.Start();

        using var caller = ConnectWithRetry(path);
        var cids = new ulong[n];
        for (int i = 0; i < n; i++)
        {
            cids[i] = caller.Call(new byte[] { 0x01 }, msgType: 0);
            using var rx = caller.Wait(cids[i]);
        }

        t.Join(2000);
        callee!.Dispose();

        for (int i = 0; i < n - 1; i++)
            Assert.Equal(cids[i] + 1, cids[i + 1]);
        Assert.All(cids, cid => Assert.True(cid > 0));
    }

    [Fact]
    public void NInflight_ResponsesMatchRequests()
    {
        const int n = 8;
        var path = Sock("inflight");
        var sent = Enumerable.Range(0, n).Select(i => i * 100).ToArray();
        RpcBus? callee = null;

        var t = new Thread(() =>
        {
            callee = RpcBus.Listen(path, 1 << 18, 1 << 18);
            callee.SetPollingMode(true);
            for (int i = 0; i < n; i++)
            {
                using var rx = callee.Serve(uint.MaxValue);
                var cid = rx.CorrelationId;
                var data = rx.Data.ToArray();
                rx.Dispose();
                callee.Reply(cid, data, msgType: 0);
            }
        });
        t.Start();

        using var caller = ConnectWithRetry(path);
        caller.SetPollingMode(true);

        var cids = new ulong[n];
        for (int i = 0; i < n; i++)
        {
            var bytes = BitConverter.GetBytes(sent[i]);
            cids[i] = caller.Call(bytes, msgType: 0);
        }

        for (int i = 0; i < n; i++)
        {
            using var rx = caller.Wait(cids[i], uint.MaxValue);
            Assert.Equal(sent[i], BitConverter.ToInt32(rx.Data));
        }

        t.Join(2000);
        callee!.Dispose();
    }

    [Fact]
    public void Reply_CorrelationIdZero_Throws()
    {
        var path = Sock("cid_zero");
        RpcBus? callee = null;
        var t = new Thread(() => callee = RpcBus.Listen(path, 1 << 16, 1 << 16));
        t.Start();

        using var caller = ConnectWithRetry(path);
        t.Join(2000);

        var ex = Assert.Throws<TachyonException>(() =>
            callee!.Reply(correlationId: 0, new byte[] { 0x01 }, msgType: 1));

        Assert.Equal(TachyonError.InvalidSize, ex.Error);
        callee!.Dispose();
    }

    [Fact]
    public void SetPollingMode_DoesNotThrow()
    {
        var path = Sock("polling");
        RpcBus? callee = null;
        var t = new Thread(() =>
        {
            callee = RpcBus.Listen(path, 1 << 16, 1 << 16);
            callee.SetPollingMode(true);
            callee.SetPollingMode(false);
            using var rx = callee.Serve();
            var cid = rx.CorrelationId;
            rx.Dispose();
            callee.Reply(cid, new byte[] { 0x01 }, msgType: 0);
        });
        t.Start();

        using var caller = ConnectWithRetry(path);
        caller.SetPollingMode(true);
        caller.SetPollingMode(false);
        var cid = caller.Call(new byte[] { 0x01 }, msgType: 0);
        using var resp = caller.Wait(cid);
        Assert.Equal(new byte[] { 0x01 }, resp.Data.ToArray());

        t.Join(2000);
        callee.Dispose();
    }

    [Fact]
    public void RpcRxGuard_Commit_Idempotent()
    {
        var path = Sock("guard_commit");
        RpcBus? callee = null;
        var t = new Thread(() =>
        {
            callee = RpcBus.Listen(path, 1 << 16, 1 << 16);
            using var rx = callee.Serve();
            var cid = rx.CorrelationId;
            rx.Dispose();
            callee.Reply(cid, new byte[] { 0x01 }, msgType: 0);
        });
        t.Start();

        using var caller = ConnectWithRetry(path);
        var cid = caller.Call(new byte[] { 0x01 }, msgType: 0);
        var guard = caller.Wait(cid);
        guard.Commit();
        guard.Commit();

        t.Join(2000);
        callee!.Dispose();
    }

    [Fact]
    public void Dispose_Idempotent()
    {
        var path = Sock("dispose");
        RpcBus? callee = null;
        var t = new Thread(() => callee = RpcBus.Listen(path, 1 << 16, 1 << 16));
        t.Start();

        var caller = ConnectWithRetry(path);
        t.Join(2000);

        caller.Dispose();
        caller.Dispose();
        callee!.Dispose();
        callee.Dispose();
    }

    [Fact]
    public void StructPayload_RoundTrip()
    {
        var path = Sock("struct");
        const int a = 0xDEAD;
        const int b = 0xBEEF;
        RpcBus? callee = null;

        var t = new Thread(() =>
        {
            callee = RpcBus.Listen(path, 1 << 16, 1 << 16);
            using var rx = callee.Serve();
            var cid = rx.CorrelationId;
            var x = BitConverter.ToInt32(rx.Data[..4]);
            var y = BitConverter.ToInt32(rx.Data[4..]);
            rx.Dispose();
            var resp = new byte[8];
            BitConverter.TryWriteBytes(resp.AsSpan(0), x + y);
            BitConverter.TryWriteBytes(resp.AsSpan(4), x ^ y);
            callee.Reply(cid, resp, msgType: 1);
        });
        t.Start();

        using var caller = ConnectWithRetry(path);
        var req = new byte[8];
        BitConverter.TryWriteBytes(req.AsSpan(0), a);
        BitConverter.TryWriteBytes(req.AsSpan(4), b);

        var cid = caller.Call(req, msgType: 1);
        using var r = caller.Wait(cid);

        Assert.Equal(a + b, BitConverter.ToInt32(r.Data[..4]));
        Assert.Equal(a ^ b, BitConverter.ToInt32(r.Data[4..]));

        t.Join(2000);
        callee!.Dispose();
    }
}