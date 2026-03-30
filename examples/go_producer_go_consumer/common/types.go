package common

const TypeIDMetricSpan uint32 = 1

type MetricSpan struct {
	Timestamp uint64
	TraceID   uint64
	SpanID    uint64
	LatencyNS uint32
	CPUUsage  float32
	MemUsage  float32
	EventType uint8
	_         [7]byte
}
