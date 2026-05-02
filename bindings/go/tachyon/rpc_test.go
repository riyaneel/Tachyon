//go:build linux || darwin

package tachyon_test

import (
	"bytes"
	"encoding/binary"
	"testing"
	"time"

	"github.com/riyaneel/tachyon/bindings/go/tachyon"
)

const rpcCap = 1 << 16

func rpcSockPath(t *testing.T) string {
	t.Helper()
	return sockPath(t)
}

func rpcConnectWithRetry(t *testing.T, path string) *tachyon.RpcBus {
	t.Helper()
	for i := 0; i < 200; i++ {
		bus, err := tachyon.RpcConnect(path)
		if err == nil {
			return bus
		}
		time.Sleep(10 * time.Millisecond)
	}

	t.Fatal("RpcConnect: timed out")
	return nil
}

func listenRpcAsync(t *testing.T, path string) <-chan *tachyon.RpcBus {
	t.Helper()
	ch := make(chan *tachyon.RpcBus, 1)

	go func() {
		bus, err := tachyon.RpcListen(path, rpcCap, rpcCap)
		if err != nil {
			t.Errorf("RpcListen: %v", err)
			ch <- nil
			return
		}
		ch <- bus
	}()

	return ch
}

func TestRpcRoundtrip(t *testing.T) {
	path := rpcSockPath(t)
	calleeCh := listenRpcAsync(t, path)

	time.Sleep(20 * time.Millisecond)

	caller := rpcConnectWithRetry(t, path)
	defer caller.Close()

	callee := <-calleeCh
	if callee == nil {
		t.Fatal("RpcListen failed")
	}
	defer callee.Close()

	payload := []byte("hello_rpc_go")
	done := make(chan struct{})

	go func() {
		defer close(done)
		rx, err := callee.Serve(10_000)
		if err != nil {
			t.Errorf("Serve: %v", err)
			return
		}

		cid := rx.CorrelationID()
		got := bytes.Clone(rx.Data())
		if err := rx.Commit(); err != nil {
			t.Errorf("Serve Commit: %v", err)
			return
		}

		// echo reversed
		reply := make([]byte, len(got))
		for i, b := range got {
			reply[len(got)-1-i] = b
		}

		if err := callee.Reply(cid, reply, 2); err != nil {
			t.Errorf("Reply: %v", err)
		}
	}()

	cid, err := caller.Call(payload, 1)
	if err != nil {
		t.Fatalf("Call: %v", err)
	}
	if cid == 0 {
		t.Fatal("correlation_id must be non-zero")
	}

	rx, err := caller.Wait(cid, 10_000)
	if err != nil {
		t.Fatalf("Wait: %v", err)
	}
	if rx.TypeID() != 2 {
		t.Errorf("TypeID: got %d, want 2", rx.TypeID())
	}

	expected := make([]byte, len(payload))
	for i, b := range payload {
		expected[len(payload)-1-i] = b
	}
	if !bytes.Equal(rx.Data(), expected) {
		t.Errorf("data: got %q, want %q", rx.Data(), expected)
	}
	if err := rx.Commit(); err != nil {
		t.Fatalf("Wait Commit: %v", err)
	}

	<-done
}

func TestRpcCorrelationIDMonotonic(t *testing.T) {
	path := rpcSockPath(t)
	calleeCh := listenRpcAsync(t, path)

	time.Sleep(20 * time.Millisecond)

	caller := rpcConnectWithRetry(t, path)
	defer caller.Close()

	callee := <-calleeCh
	if callee == nil {
		t.Fatal("RpcListen failed")
	}
	defer callee.Close()

	const n = 4
	done := make(chan struct{})

	go func() {
		defer close(done)
		for range n {
			rx, err := callee.Serve(10_000)
			if err != nil {
				t.Errorf("Serve: %v", err)
				return
			}

			cid := rx.CorrelationID()
			if err := rx.Commit(); err != nil {
				t.Errorf("Commit: %v", err)
				return
			}
			if err := callee.Reply(cid, []byte("ok"), 0); err != nil {
				t.Errorf("Reply: %v", err)
				return
			}
		}
	}()

	cids := make([]uint64, n)
	for i := range n {
		cid, err := caller.Call([]byte("x"), 0)
		if err != nil {
			t.Fatalf("Call[%d]: %v", i, err)
		}

		cids[i] = cid
		rx, err := caller.Wait(cid, 10_000)
		if err != nil {
			t.Fatalf("Wait[%d]: %v", i, err)
		}
		if err := rx.Commit(); err != nil {
			t.Fatalf("Commit[%d]: %v", i, err)
		}
	}

	for i := range n - 1 {
		if cids[i+1] != cids[i]+1 {
			t.Errorf("cids not monotonic: cids[%d]=%d cids[%d]=%d", i, cids[i], i+1, cids[i+1])
		}
	}

	for _, cid := range cids {
		if cid == 0 {
			t.Error("correlation_id must never be 0")
		}
	}

	<-done
}

func TestRpcStructPayload(t *testing.T) {
	path := rpcSockPath(t)
	calleeCh := listenRpcAsync(t, path)

	time.Sleep(20 * time.Millisecond)

	caller := rpcConnectWithRetry(t, path)
	defer caller.Close()

	callee := <-calleeCh
	if callee == nil {
		t.Fatal("RpcListen failed")
	}
	defer callee.Close()

	sentA := uint32(0xDEADBEEF)
	sentB := uint32(0xCAFEBABE)
	req := make([]byte, 8)
	binary.LittleEndian.PutUint32(req[0:], sentA)
	binary.LittleEndian.PutUint32(req[4:], sentB)

	done := make(chan struct{})
	go func() {
		defer close(done)
		rx, err := callee.Serve(10_000)
		if err != nil {
			t.Errorf("Serve: %v", err)
			return
		}

		cid := rx.CorrelationID()
		a := binary.LittleEndian.Uint32(rx.Data()[0:])
		b := binary.LittleEndian.Uint32(rx.Data()[4:])
		if err := rx.Commit(); err != nil {
			t.Errorf("Commit: %v", err)
			return
		}

		reply := make([]byte, 8)
		binary.LittleEndian.PutUint32(reply[0:], a+b)
		binary.LittleEndian.PutUint32(reply[4:], a^b)
		if err := callee.Reply(cid, reply, 1); err != nil {
			t.Errorf("Reply: %v", err)
		}
	}()

	cid, err := caller.Call(req, 1)
	if err != nil {
		t.Fatalf("Call: %v", err)
	}

	rx, err := caller.Wait(cid, 10_000)
	if err != nil {
		t.Fatalf("Wait: %v", err)
	}

	gotSum := binary.LittleEndian.Uint32(rx.Data()[0:])
	gotXor := binary.LittleEndian.Uint32(rx.Data()[4:])
	if err := rx.Commit(); err != nil {
		t.Fatalf("Commit: %v", err)
	}

	if gotSum != sentA+sentB {
		t.Errorf("sum: got %x, want %x", gotSum, sentA+sentB)
	}
	if gotXor != sentA^sentB {
		t.Errorf("xor: got %x, want %x", gotXor, sentA^sentB)
	}

	<-done
}

func TestRpcGuardFinalizerSafety(t *testing.T) {
	path := rpcSockPath(t)
	calleeCh := listenRpcAsync(t, path)

	time.Sleep(20 * time.Millisecond)

	caller := rpcConnectWithRetry(t, path)
	defer caller.Close()

	callee := <-calleeCh
	if callee == nil {
		t.Fatal("RpcListen failed")
	}
	defer callee.Close()

	done := make(chan struct{})
	go func() {
		defer close(done)
		rx, err := callee.Serve(10_000)
		if err != nil {
			t.Errorf("Serve: %v", err)
			return
		}

		cid := rx.CorrelationID()
		_ = cid
		rx = nil
		// give GC a chance
		for range 3 {
			time.Sleep(10 * time.Millisecond)
		}

		if err := callee.Reply(cid, []byte("fin"), 0); err != nil {
			t.Logf("Reply after finalizer commit: %v (expected on some platforms)", err)
		}
	}()

	cid, err := caller.Call([]byte("fin_test"), 0)
	if err != nil {
		t.Fatalf("Call: %v", err)
	}
	_ = cid

	<-done
}

func TestRpcSetPollingMode(t *testing.T) {
	path := rpcSockPath(t)
	calleeCh := listenRpcAsync(t, path)

	time.Sleep(20 * time.Millisecond)

	caller := rpcConnectWithRetry(t, path)
	defer caller.Close()

	callee := <-calleeCh
	if callee == nil {
		t.Fatal("RpcListen failed")
	}
	defer callee.Close()

	// must not panic or error for both directions
	callee.SetPollingMode(1)
	caller.SetPollingMode(1)
	callee.SetPollingMode(0)
	caller.SetPollingMode(0)

	// functional verify after mode change
	done := make(chan struct{})
	go func() {
		defer close(done)
		rx, err := callee.Serve(10_000)
		if err != nil {
			t.Errorf("Serve: %v", err)
			return
		}

		cid := rx.CorrelationID()
		if err := rx.Commit(); err != nil {
			t.Errorf("Commit: %v", err)
		}
		if err := callee.Reply(cid, []byte("pong"), 0); err != nil {
			t.Errorf("Reply: %v", err)
		}
	}()

	cid, err := caller.Call([]byte("ping"), 0)
	if err != nil {
		t.Fatalf("Call: %v", err)
	}

	rx, err := caller.Wait(cid, 10_000)
	if err != nil {
		t.Fatalf("Wait: %v", err)
	}
	if !bytes.Equal(rx.Data(), []byte("pong")) {
		t.Errorf("data: got %q, want pong", rx.Data())
	}
	if err := rx.Commit(); err != nil {
		t.Fatalf("Commit: %v", err)
	}

	<-done
}
