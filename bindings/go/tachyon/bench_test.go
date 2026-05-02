//go:build linux || darwin

package tachyon_test

import (
	"path/filepath"
	"runtime"
	"sort"
	"sync"
	"testing"
	"time"

	"github.com/riyaneel/tachyon/bindings/go/tachyon"
)

const (
	benchCapacity = 1 << 22 // 4MB
	benchPayload  = 32
)

func connectWithRetry(tb testing.TB, path string) *tachyon.Bus {
	tb.Helper()
	deadline := time.Now().Add(2 * time.Second)
	for time.Now().Before(deadline) {
		bus, err := tachyon.Connect(path)
		if err == nil {
			return bus
		}
		time.Sleep(10 * time.Millisecond)
	}
	tb.Fatalf("Connect: timed out after 2s on %s", path)
	return nil
}

func benchSockPath(b *testing.B, name string) string {
	b.Helper()
	return filepath.Join(b.TempDir(), name+".sock")
}

func drainLoop(bus *tachyon.Bus, stop <-chan struct{}, wg *sync.WaitGroup) {
	defer wg.Done()
	for {
		select {
		case <-stop:
			return
		default:
		}
		batch, err := bus.TryDrainBatch(64)
		if err != nil {
			return
		}
		if batch == nil {
			runtime.Gosched()
			continue
		}
		batch.Commit()
	}
}

func BenchmarkSend(b *testing.B) {
	path := benchSockPath(b, "send")

	srvCh := make(chan *tachyon.Bus, 1)
	go func() {
		bus, err := tachyon.Listen(path, benchCapacity)
		if err != nil {
			b.Errorf("Listen: %v", err)
			srvCh <- nil
			return
		}
		srvCh <- bus
	}()

	time.Sleep(20 * time.Millisecond)

	client, err := tachyon.Connect(path)
	if err != nil {
		b.Fatalf("Connect: %v", err)
	}

	srv := <-srvCh
	if srv == nil {
		b.Fatal("Listen failed")
	}
	srv.SetPollingMode(1)

	stop := make(chan struct{})
	var wg sync.WaitGroup
	wg.Add(1)
	go drainLoop(srv, stop, &wg)

	payload := make([]byte, benchPayload)

	b.ResetTimer()
	b.SetBytes(benchPayload)
	for range b.N {
		if err := client.Send(payload, 1); err != nil {
			b.Fatal(err)
		}
	}
	b.StopTimer()

	close(stop)
	wg.Wait()
	srv.Close()
	client.Close()
}

func BenchmarkRecv(b *testing.B) {
	path := benchSockPath(b, "recv")

	srvCh := make(chan *tachyon.Bus, 1)
	go func() {
		bus, err := tachyon.Listen(path, benchCapacity)
		if err != nil {
			b.Errorf("Listen: %v", err)
			srvCh <- nil
			return
		}
		srvCh <- bus
	}()

	time.Sleep(20 * time.Millisecond)

	client, err := tachyon.Connect(path)
	if err != nil {
		b.Fatalf("Connect: %v", err)
	}

	srv := <-srvCh
	if srv == nil {
		b.Fatal("Listen failed")
	}
	srv.SetPollingMode(1)

	stop := make(chan struct{})
	var wg sync.WaitGroup
	payload := make([]byte, benchPayload)
	wg.Add(1)
	go func() {
		defer wg.Done()
		for {
			select {
			case <-stop:
				return
			default:
			}
			client.Send(payload, 1)
		}
	}()

	b.ResetTimer()
	b.SetBytes(benchPayload)
	for range b.N {
		if _, _, err := srv.Recv(10_000); err != nil {
			b.Fatal(err)
		}
	}
	b.StopTimer()

	close(stop)
	wg.Wait()
	srv.Close()
	client.Close()
}

func BenchmarkDrainBatch(b *testing.B) {
	path := benchSockPath(b, "drainbatch")

	srvCh := make(chan *tachyon.Bus, 1)
	go func() {
		bus, err := tachyon.Listen(path, benchCapacity)
		if err != nil {
			b.Errorf("Listen: %v", err)
			srvCh <- nil
			return
		}
		srvCh <- bus
	}()

	time.Sleep(20 * time.Millisecond)

	client, err := tachyon.Connect(path)
	if err != nil {
		b.Fatalf("Connect: %v", err)
	}

	srv := <-srvCh
	if srv == nil {
		b.Fatal("Listen failed")
	}
	srv.SetPollingMode(1)

	stop := make(chan struct{})
	var wg sync.WaitGroup
	payload := make([]byte, benchPayload)
	wg.Add(1)
	go func() {
		defer wg.Done()
		for {
			select {
			case <-stop:
				return
			default:
			}
			client.Send(payload, 1)
		}
	}()

	b.ResetTimer()
	for range b.N {
		batch, err := srv.DrainBatch(64, 10_000)
		if err != nil {
			b.Fatal(err)
		}
		n := batch.Len()
		if err := batch.Commit(); err != nil {
			b.Fatal(err)
		}
		b.SetBytes(int64(n * benchPayload))
	}
	b.StopTimer()

	close(stop)
	wg.Wait()
	srv.Close()
	client.Close()
}

func BenchmarkPingPong(b *testing.B) {
	pathAB := benchSockPath(b, "pp_ab")
	pathBA := benchSockPath(b, "pp_ba")

	abSrvCh := make(chan *tachyon.Bus, 1)
	baSrvCh := make(chan *tachyon.Bus, 1)

	go func() {
		bus, err := tachyon.Listen(pathAB, benchCapacity)
		if err != nil {
			b.Errorf("Listen AB: %v", err)
			abSrvCh <- nil
			return
		}
		abSrvCh <- bus
	}()

	go func() {
		bus, err := tachyon.Listen(pathBA, benchCapacity)
		if err != nil {
			b.Errorf("Listen BA: %v", err)
			baSrvCh <- nil
			return
		}
		baSrvCh <- bus
	}()

	time.Sleep(30 * time.Millisecond)

	txBus, err := tachyon.Connect(pathAB)
	if err != nil {
		b.Fatalf("Connect AB: %v", err)
	}
	rxBus, err := tachyon.Connect(pathBA)
	if err != nil {
		b.Fatalf("Connect BA: %v", err)
	}

	abSrv := <-abSrvCh
	baSrv := <-baSrvCh
	if abSrv == nil || baSrv == nil {
		b.Fatal("Listen failed")
	}

	abSrv.SetPollingMode(1)
	baSrv.SetPollingMode(1)

	stop := make(chan struct{})
	var wg sync.WaitGroup
	wg.Add(1)
	go func() {
		defer wg.Done()
		for {
			select {
			case <-stop:
				return
			default:
			}
			batch, err := abSrv.TryDrainBatch(64)
			if err != nil {
				return
			}
			if batch == nil {
				runtime.Gosched()
				continue
			}
			for msg := range batch.Iter() {
				data := make([]byte, msg.Size())
				copy(data, msg.Data())
				baSrv.Send(data, msg.TypeID())
			}
			batch.Commit()
		}
	}()

	payload := make([]byte, benchPayload)

	for range 1000 {
		txBus.Send(payload, 1)
		rxBus.Recv(10_000)
	}

	latencies := make([]int64, b.N)

	b.ResetTimer()
	for i := range b.N {
		t0 := time.Now()
		if err := txBus.Send(payload, 1); err != nil {
			b.Fatal(err)
		}
		if _, _, err := rxBus.Recv(10_000); err != nil {
			b.Fatal(err)
		}
		latencies[i] = time.Since(t0).Nanoseconds()
	}
	b.StopTimer()

	close(stop)
	wg.Wait()
	abSrv.Close()
	baSrv.Close()
	txBus.Close()
	rxBus.Close()

	sort.Slice(latencies, func(i, j int) bool { return latencies[i] < latencies[j] })
	n := len(latencies)
	if n == 0 {
		return
	}

	pct := func(p float64) int64 {
		idx := int(float64(n) * p)
		if idx >= n {
			idx = n - 1
		}
		return latencies[idx]
	}

	b.ReportMetric(float64(pct(0.50)), "p50_ns")
	b.ReportMetric(float64(pct(0.90)), "p90_ns")
	b.ReportMetric(float64(pct(0.99)), "p99_ns")
	b.ReportMetric(float64(pct(0.999)), "p999_ns")
}
