package main

import (
	"fmt"
	"log"
	"math/rand"
	"time"
	"unsafe"

	"github.com/riyaneel/tachyon/bindings/go/tachyon"
	"github.com/riyaneel/tachyon/examples/go_producer_go_consumer/common"
)

func main() {
	fmt.Println("[Producer] Starting business application...")

	bus, err := tachyon.Listen("/tmp/tachyon_telemetry.sock", 8192)
	if err != nil {
		log.Fatalf("Listen error: %v", err)
	}

	defer bus.Close()

	structSize := int(unsafe.Sizeof(common.MetricSpan{}))
	fmt.Printf("[Producer] MetricSpan size: %d bytes\n", structSize)

	for {
		for i := 0; i < 5000; i++ {
			guard, err := bus.AcquireTx(structSize)
			if err != nil {
				continue
			}

			span := (*common.MetricSpan)(unsafe.Pointer(&guard.Bytes()[0]))
			span.Timestamp = uint64(time.Now().UnixNano())
			span.TraceID = rand.Uint64()
			span.SpanID = rand.Uint64()
			span.LatencyNS = uint32(rand.Intn(500000))
			span.CPUUsage = rand.Float32() * 100
			span.MemUsage = rand.Float32() * 1024
			span.EventType = uint8(i % 3)

			if err := guard.Commit(structSize, common.TypeIDMetricSpan); err != nil {
				log.Printf("Commit failed: %v", err)
			}
		}

		time.Sleep(10 * time.Millisecond)
	}
}
