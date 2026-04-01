package main

import (
	"fmt"
	"log"
	"runtime"
	"sync/atomic"
	"time"
	"unsafe"

	"github.com/riyaneel/tachyon/bindings/go/tachyon"
	"github.com/riyaneel/tachyon/examples/go_producer_go_consumer/common"
)

func main() {
	fmt.Println("[Agent] Starting telemetry agent...")

	bus, err := tachyon.Connect("/tmp/tachyon_telemetry.sock")
	if err != nil {
		log.Fatalf("Connect error: %v", err)
	}

	defer bus.Close()

	var totalSpans uint64
	var totalLatency uint64
	var batchCount uint64

	go func() {
		ticker := time.NewTicker(100 * time.Millisecond)
		defer ticker.Stop()

		var lastSpans uint64
		for range ticker.C {
			currentSpans := atomic.LoadUint64(&totalSpans)
			currentLat := atomic.LoadUint64(&totalLatency)
			currentBatches := atomic.LoadUint64(&batchCount)

			delta := currentSpans - lastSpans
			if delta > 0 {
				avgLat := currentLat / currentSpans
				fmt.Printf("[Flush] Ingested spans: %d (+%d) | Batches: %d | Avg Latency: %d ns\n",
					currentSpans, delta, currentBatches, avgLat)
				lastSpans = currentSpans
			}
		}
	}()

	runtime.LockOSThread()

	for {
		batch, err := bus.DrainBatch(2048, 1000)
		if err != nil {
			time.Sleep(time.Millisecond)
			continue
		}

		if batch.Len() == 0 {
			continue
		}

		atomic.AddUint64(&batchCount, 1)

		var localLatencyAcc uint64
		var localSpansAcc uint64

		for msg := range batch.Iter() {
			if msg.TypeID() != common.TypeIDMetricSpan {
				continue
			}

			span := (*common.MetricSpan)(unsafe.Pointer(&msg.Data()[0]))
			localSpansAcc++
			localLatencyAcc += uint64(span.LatencyNS)
		}

		atomic.AddUint64(&totalSpans, localSpansAcc)
		atomic.AddUint64(&totalLatency, localLatencyAcc)

		if err := batch.Commit(); err != nil {
			log.Printf("Batch commit failed: %v", err)
		}
	}
}
