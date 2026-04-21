//go:build linux || darwin

package tachyon_test

import (
	"bytes"
	"path/filepath"
	"runtime"
	"testing"
	"time"

	"github.com/riyaneel/tachyon/bindings/go/tachyon"
)

const (
	capacity = 1 << 16 // 64KB
)

func sockPath(t *testing.T) string {
	t.Helper()
	return filepath.Join(t.TempDir(), "bus.sock")
}

func listenAsync(t *testing.T, path string) <-chan *tachyon.Bus {
	t.Helper()
	ch := make(chan *tachyon.Bus, 1)
	go func() {
		bus, err := tachyon.Listen(path, capacity)
		if err != nil {
			t.Errorf("Listen: %v", err)
			ch <- nil
			return
		}
		ch <- bus
	}()
	return ch
}

func TestSendRecv(t *testing.T) {
	path := sockPath(t)
	srvCh := listenAsync(t, path)

	time.Sleep(20 * time.Millisecond)

	client := connectWithRetry(t, path)
	defer client.Close()

	payload := []byte("hello_tachyon_go")
	if err := client.Send(payload, 42); err != nil {
		t.Fatalf("Send: %v", err)
	}

	srv := <-srvCh
	if srv == nil {
		t.Fatal("Listen failed")
	}
	defer srv.Close()

	data, typeID, err := srv.Recv(10_000)
	if err != nil {
		t.Fatalf("Recv: %v", err)
	}
	if typeID != 42 {
		t.Errorf("typeID: got %d, want 42", typeID)
	}
	if !bytes.Equal(data, payload) {
		t.Errorf("data: got %q, want %q", data, payload)
	}
}

func TestSendRecvMultiple(t *testing.T) {
	path := sockPath(t)
	srvCh := listenAsync(t, path)

	time.Sleep(20 * time.Millisecond)

	client := connectWithRetry(t, path)
	defer client.Close()

	srv := <-srvCh
	if srv == nil {
		t.Fatal("Listen failed")
	}
	defer srv.Close()

	for i := range 5 {
		payload := []byte{byte(i)}
		if err := client.Send(payload, uint32(i)); err != nil {
			t.Fatalf("Send[%d]: %v", i, err)
		}
		data, typeID, err := srv.Recv(10_000)
		if err != nil {
			t.Fatalf("Recv[%d]: %v", i, err)
		}
		if typeID != uint32(i) {
			t.Errorf("[%d] typeID: got %d, want %d", i, typeID, i)
		}
		if !bytes.Equal(data, payload) {
			t.Errorf("[%d] data: got %v, want %v", i, data, payload)
		}
	}
}

func TestZeroCopyTX(t *testing.T) {
	path := sockPath(t)
	srvCh := listenAsync(t, path)

	time.Sleep(20 * time.Millisecond)

	client := connectWithRetry(t, path)
	defer client.Close()

	srv := <-srvCh
	if srv == nil {
		t.Fatal("Listen failed")
	}
	defer srv.Close()

	payload := []byte("zero_copy_tx_data")

	guard, err := client.AcquireTx(len(payload))
	if err != nil {
		t.Fatalf("AcquireTx: %v", err)
	}
	copy(guard.Bytes(), payload)
	if err := guard.Commit(len(payload), 7); err != nil {
		t.Fatalf("Commit: %v", err)
	}

	data, typeID, err := srv.Recv(10_000)
	if err != nil {
		t.Fatalf("Recv: %v", err)
	}
	if typeID != 7 {
		t.Errorf("typeID: got %d, want 7", typeID)
	}
	if !bytes.Equal(data, payload) {
		t.Errorf("data: got %q, want %q", data, payload)
	}
}

func TestRollbackNoPhantomMessage(t *testing.T) {
	path := sockPath(t)
	srvCh := listenAsync(t, path)

	time.Sleep(20 * time.Millisecond)

	client := connectWithRetry(t, path)
	defer client.Close()

	srv := <-srvCh
	if srv == nil {
		t.Fatal("Listen failed")
	}
	defer srv.Close()

	guard, err := client.AcquireTx(32)
	if err != nil {
		t.Fatalf("AcquireTx: %v", err)
	}
	copy(guard.Bytes(), []byte("this_should_not_appear"))
	if err := guard.Rollback(); err != nil {
		t.Fatalf("Rollback: %v", err)
	}
	client.Flush()

	committed := []byte("committed_message")
	if err := client.Send(committed, 99); err != nil {
		t.Fatalf("Send: %v", err)
	}

	data, typeID, err := srv.Recv(10_000)
	if err != nil {
		t.Fatalf("Recv: %v", err)
	}
	if typeID != 99 {
		t.Errorf("typeID: got %d, want 99", typeID)
	}
	if !bytes.Equal(data, committed) {
		t.Errorf("data: got %q, want %q", data, committed)
	}
}

func TestZeroCopyRX(t *testing.T) {
	path := sockPath(t)
	srvCh := listenAsync(t, path)

	time.Sleep(20 * time.Millisecond)

	client := connectWithRetry(t, path)
	defer client.Close()

	srv := <-srvCh
	if srv == nil {
		t.Fatal("Listen failed")
	}
	defer srv.Close()

	payload := []byte("zero_copy_rx_payload")
	if err := client.Send(payload, 55); err != nil {
		t.Fatalf("Send: %v", err)
	}

	guard, err := srv.AcquireRx(10_000)
	if err != nil {
		t.Fatalf("AcquireRx: %v", err)
	}

	if guard.TypeID() != 55 {
		t.Errorf("TypeID: got %d, want 55", guard.TypeID())
	}
	if guard.Size() != len(payload) {
		t.Errorf("Size: got %d, want %d", guard.Size(), len(payload))
	}

	got := make([]byte, guard.Size())
	copy(got, guard.Data())

	if err := guard.Commit(); err != nil {
		t.Fatalf("Commit: %v", err)
	}

	if !bytes.Equal(got, payload) {
		t.Errorf("data: got %q, want %q", got, payload)
	}
}

func TestDrainBatch(t *testing.T) {
	path := sockPath(t)
	srvCh := listenAsync(t, path)

	time.Sleep(20 * time.Millisecond)

	client := connectWithRetry(t, path)
	defer client.Close()

	srv := <-srvCh
	if srv == nil {
		t.Fatal("Listen failed")
	}
	defer srv.Close()

	const n = 5
	payloads := [][]byte{
		{0x01}, {0x02, 0x02}, {0x03, 0x03, 0x03}, {0x04}, {0x05, 0x05},
	}

	for i, p := range payloads {
		guard, err := client.AcquireTx(len(p))
		if err != nil {
			t.Fatalf("AcquireTx[%d]: %v", i, err)
		}
		copy(guard.Bytes(), p)
		if err := guard.CommitUnflushed(len(p), uint32(i)); err != nil {
			t.Fatalf("CommitUnflushed[%d]: %v", i, err)
		}
	}
	client.Flush()

	batch, err := srv.DrainBatch(64, 10_000)
	if err != nil {
		t.Fatalf("DrainBatch: %v", err)
	}
	defer batch.Commit()

	if batch.Len() != n {
		t.Fatalf("Len: got %d, want %d", batch.Len(), n)
	}

	for i := range n {
		msg := batch.At(i)
		if msg.TypeID() != uint32(i) {
			t.Errorf("[%d] TypeID: got %d, want %d", i, msg.TypeID(), i)
		}
		if !bytes.Equal(msg.Data(), payloads[i]) {
			t.Errorf("[%d] Data: got %v, want %v", i, msg.Data(), payloads[i])
		}
	}
}

func TestDrainBatchIter(t *testing.T) {
	path := sockPath(t)
	srvCh := listenAsync(t, path)

	time.Sleep(20 * time.Millisecond)

	client := connectWithRetry(t, path)
	defer client.Close()

	srv := <-srvCh
	if srv == nil {
		t.Fatal("Listen failed")
	}
	defer srv.Close()

	msgs := [][]byte{[]byte("a"), []byte("bb"), []byte("ccc")}
	for i, p := range msgs {
		guard, err := client.AcquireTx(len(p))
		if err != nil {
			t.Fatalf("AcquireTx[%d]: %v", i, err)
		}
		copy(guard.Bytes(), p)
		if err := guard.CommitUnflushed(len(p), uint32(i)); err != nil {
			t.Fatalf("CommitUnflushed[%d]: %v", i, err)
		}
	}
	client.Flush()

	batch, err := srv.DrainBatch(16, 10_000)
	if err != nil {
		t.Fatalf("DrainBatch: %v", err)
	}

	var collected [][]byte
	for msg := range batch.Iter() {
		collected = append(collected, bytes.Clone(msg.Data()))
	}
	if err := batch.Commit(); err != nil {
		t.Fatalf("Commit: %v", err)
	}

	if len(collected) != len(msgs) {
		t.Fatalf("collected %d messages, want %d", len(collected), len(msgs))
	}
	for i, got := range collected {
		if !bytes.Equal(got, msgs[i]) {
			t.Errorf("[%d] got %q, want %q", i, got, msgs[i])
		}
	}
}

func TestSetNumaNode(t *testing.T) {
	if runtime.GOOS == "darwin" {
		t.Skip("SetNumaNode is a no-op on Darwin: skipping")
	}

	path := sockPath(t)
	srvCh := listenAsync(t, path)

	time.Sleep(20 * time.Millisecond)

	client := connectWithRetry(t, path)
	defer client.Close()

	srv := <-srvCh
	if srv == nil {
		t.Fatal("Listen failed")
	}
	defer srv.Close()

	if err := srv.SetNumaNode(0); err != nil {
		t.Errorf("SetNumaNode(0): %v", err)
	}
	if err := client.SetNumaNode(0); err != nil {
		t.Errorf("SetNumaNode(0) client: %v", err)
	}

	if err := srv.SetNumaNode(-1); err == nil {
		t.Error("SetNumaNode(-1): expected error, got nil")
	}

	if err := srv.SetNumaNode(64); err == nil {
		t.Error("SetNumaNode(64): expected error, got nil")
	}
}

func TestABIMismatchError(t *testing.T) {
	err := &tachyon.TachyonError{Code: 14, Message: "ABI mismatch"}
	if !tachyon.IsABIMismatch(err) {
		t.Error("IsABIMismatch: expected true for code 14")
	}

	other := &tachyon.TachyonError{Code: 9, Message: "buffer full"}
	if tachyon.IsABIMismatch(other) {
		t.Error("IsABIMismatch: expected false for code 9")
	}

	msg := err.Error()
	if msg == "" {
		t.Error("Error(): empty string")
	}
}

func TestTypeIDEncoding(t *testing.T) {
	// route=0 preserves v0.3.x semantics
	if got := tachyon.MakeTypeID(0, 42); got != 42 {
		t.Errorf("MakeTypeID(0, 42): got %d, want 42", got)
	}
	if got := tachyon.RouteID(42); got != 0 {
		t.Errorf("RouteID(42): got %d, want 0", got)
	}
	if got := tachyon.MsgType(42); got != 42 {
		t.Errorf("MsgType(42): got %d, want 42", got)
	}

	// round-trip
	id := tachyon.MakeTypeID(1, 99)
	if tachyon.RouteID(id) != 1 {
		t.Errorf("RouteID: got %d, want 1", tachyon.RouteID(id))
	}
	if tachyon.MsgType(id) != 99 {
		t.Errorf("MsgType: got %d, want 99", tachyon.MsgType(id))
	}

	// sentinel unchanged
	if tachyon.MakeTypeID(0, 0) != 0 {
		t.Error("MakeTypeID(0, 0) must be 0")
	}
}

func TestTypeIDRoundTripOverBus(t *testing.T) {
	path := sockPath(t)
	srvCh := listenAsync(t, path)

	time.Sleep(20 * time.Millisecond)

	client := connectWithRetry(t, path)
	defer client.Close()

	srv := <-srvCh
	if srv == nil {
		t.Fatal("Listen failed")
	}
	defer srv.Close()

	if err := client.Send([]byte("payload"), tachyon.MakeTypeID(0, 42)); err != nil {
		t.Fatalf("Send: %v", err)
	}

	data, typeID, err := srv.Recv(10_000)
	if err != nil {
		t.Fatalf("Recv: %v", err)
	}
	if tachyon.RouteID(typeID) != 0 {
		t.Errorf("RouteID: got %d, want 0", tachyon.RouteID(typeID))
	}
	if tachyon.MsgType(typeID) != 42 {
		t.Errorf("MsgType: got %d, want 42", tachyon.MsgType(typeID))
	}
	if len(data) == 0 {
		t.Error("data: empty")
	}
}
