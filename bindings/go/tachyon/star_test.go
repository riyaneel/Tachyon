//go:build linux || darwin

package tachyon_test

import (
	"bytes"
	"testing"
	"time"

	"github.com/riyaneel/tachyon/bindings/go/tachyon"
)

func makeStarSpoke(t *testing.T, path string) (producer *tachyon.Bus, spoke *tachyon.Bus) {
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

	spoke = connectWithRetry(t, path)
	producer = <-ch
	if producer == nil {
		t.Fatal("Listen failed")
	}

	return producer, spoke
}

func TestStarNSpokes(t *testing.T) {
	p0 := sockPath(t)
	p1 := sockPath(t)

	prod0, spoke0 := makeStarSpoke(t, p0)
	prod1, spoke1 := makeStarSpoke(t, p1)
	defer prod0.Close()
	defer prod1.Close()
	defer spoke0.Close()
	defer spoke1.Close()

	star, err := tachyon.Create([]*tachyon.Bus{spoke0, spoke1}, nil)
	if err != nil {
		t.Fatalf("Create: %v", err)
	}
	defer star.Close()

	if star.NSpokes() != 2 {
		t.Errorf("NSPokes: got %d, want 2", star.NSpokes())
	}
}

func TestStarPoll_DrainsBothSpokes(t *testing.T) {
	p0 := sockPath(t)
	p1 := sockPath(t)

	prod0, spoke0 := makeStarSpoke(t, p0)
	prod1, spoke1 := makeStarSpoke(t, p1)
	defer prod0.Close()
	defer prod1.Close()

	if err := prod0.Send([]byte("spoke0"), tachyon.MakeTypeID(0, 1)); err != nil {
		t.Fatalf("Send[0]: %v", err)
	}
	if err := prod1.Send([]byte("spoke1"), tachyon.MakeTypeID(1, 1)); err != nil {
		t.Fatalf("Send[1]: %v", err)
	}

	star, err := tachyon.Create([]*tachyon.Bus{spoke0, spoke1}, nil)
	if err != nil {
		t.Fatalf("Create: %v", err)
	}
	defer star.Close()
	defer spoke0.Close()
	defer spoke1.Close()

	guard, err := star.Poll(16, 5_000)
	if err != nil {
		t.Fatalf("Poll: %v", err)
	}

	if guard.Len() != 2 {
		t.Fatalf("Len: got %d, want 2", guard.Len())
	}

	received := make(map[int][]byte, 2)
	for msg := range guard.Iter() {
		received[msg.SpokeIdx()] = bytes.Clone(msg.Data())
	}
	if err := guard.Commit(); err != nil {
		t.Fatalf("Commit: %v", err)
	}

	if !bytes.Equal(received[0], []byte("spoke0")) {
		t.Errorf("spoke0 data: got %q, want %q", received[0], "spoke0")
	}
	if !bytes.Equal(received[1], []byte("spoke1")) {
		t.Errorf("spoke1 data: got %q, want %q", received[1], "spoke1")
	}
	if tachyon.MsgType(guard.At(0).TypeID()); false {
		// silence unused import
	}
}

func TestStarPoll_EmptyOnBudgetExpiry(t *testing.T) {
	p0 := sockPath(t)
	prod0, spoke0 := makeStarSpoke(t, p0)
	defer prod0.Close()
	defer spoke0.Close()

	star, err := tachyon.Create([]*tachyon.Bus{spoke0}, nil)
	if err != nil {
		t.Fatalf("Create: %v", err)
	}
	defer star.Close()

	guard, err := star.Poll(16, 1)
	if err != nil {
		t.Fatalf("Poll: %v", err)
	}

	if !guard.IsEmpty() {
		t.Errorf("expected empty guard, got %d messages", guard.Len())
	}

	if err := guard.Commit(); err != nil {
		t.Fatalf("Commit: %v", err)
	}
}

func TestStarAcquireTx_CommitAndRollback(t *testing.T) {
	p0 := sockPath(t)
	prod0, spoke0 := makeStarSpoke(t, p0)
	defer prod0.Close()
	defer spoke0.Close()

	star, err := tachyon.Create([]*tachyon.Bus{spoke0}, nil)
	if err != nil {
		t.Fatalf("Create: %v", err)
	}
	defer star.Close()

	guard := star.AcquireTx(0, 8)
	if guard == nil {
		t.Fatal("AcquireTx returned nil")
	}
	copy(guard.Bytes(), []byte("dropped!"))
	if err := guard.Rollback(); err != nil {
		t.Fatalf("Rollback: %v", err)
	}

	payload := []byte{0xDE, 0xAD, 0xBE, 0xEF}
	guard2 := star.AcquireTx(0, len(payload))
	if guard2 == nil {
		t.Fatal("AcquireTx returned nil")
	}
	copy(guard2.Bytes(), payload)
	if err := guard2.Commit(len(payload), tachyon.MakeTypeID(0, 7)); err != nil {
		t.Fatalf("Commit: %v", err)
	}

	data, typeID, err := prod0.Recv(10_000)
	if err != nil {
		t.Fatalf("Recv: %v", err)
	}
	if !bytes.Equal(data, payload) {
		t.Errorf("data: got %v, want %v", data, payload)
	}
	if tachyon.MsgType(typeID) != 7 {
		t.Errorf("msg_type: got %d, want 7", tachyon.MsgType(typeID))
	}
}

func TestStar_BusRefHeldAfterClose(t *testing.T) {
	p0 := sockPath(t)
	prod0, spoke0 := makeStarSpoke(t, p0)
	defer prod0.Close()

	star, err := tachyon.Create([]*tachyon.Bus{spoke0}, nil)
	if err != nil {
		t.Fatalf("Create: %v", err)
	}
	defer star.Close()

	spoke0.Close()
	if err := prod0.Send([]byte("still_alive"), 99); err != nil {
		t.Fatalf("Send: %v", err)
	}

	guard, err := star.Poll(4, 5_000)
	if err != nil {
		t.Fatalf("Poll: %v", err)
	}
	if guard.Len() != 1 {
		t.Fatalf("Len: got %d, want 1", guard.Len())
	}
	if !bytes.Equal(guard.At(0).Data(), []byte("still_alive")) {
		t.Errorf("data: got %q, want %q", guard.At(0).Data(), "still_alive")
	}
	if err := guard.Commit(); err != nil {
		t.Fatalf("Commit: %v", err)
	}
}

func TestStarAcquireTx_OobSpokeReturnsNil(t *testing.T) {
	p0 := sockPath(t)
	prod0, spoke0 := makeStarSpoke(t, p0)
	defer prod0.Close()
	defer spoke0.Close()

	star, err := tachyon.Create([]*tachyon.Bus{spoke0}, nil)
	if err != nil {
		t.Fatalf("Create: %v", err)
	}
	defer star.Close()

	if g := star.AcquireTx(99, 8); g != nil {
		t.Error("expected nil for out-of-bounds spoke, got non-nil guard")
		g.Rollback()
	}
}

func TestStarPoll_Iter(t *testing.T) {
	p0 := sockPath(t)
	p1 := sockPath(t)

	prod0, spoke0 := makeStarSpoke(t, p0)
	prod1, spoke1 := makeStarSpoke(t, p1)
	defer prod0.Close()
	defer prod1.Close()
	defer spoke0.Close()
	defer spoke1.Close()

	payloads := [][]byte{[]byte("aaa"), []byte("bbb")}
	prod0.Send(payloads[0], tachyon.MakeTypeID(0, 10))
	prod1.Send(payloads[1], tachyon.MakeTypeID(1, 11))

	time.Sleep(5 * time.Millisecond)
	star, err := tachyon.Create([]*tachyon.Bus{spoke0, spoke1}, nil)
	if err != nil {
		t.Fatalf("Create: %v", err)
	}
	defer star.Close()

	guard, err := star.Poll(16, 5_000)
	if err != nil {
		t.Fatalf("Poll: %v", err)
	}
	defer guard.Commit()

	var count int
	for msg := range guard.Iter() {
		if msg.Size() == 0 {
			t.Errorf("msg[%d]: zero size", count)
		}
		count++
	}
	if count != 2 {
		t.Errorf("Iter count: got %d, want 2", count)
	}
}
