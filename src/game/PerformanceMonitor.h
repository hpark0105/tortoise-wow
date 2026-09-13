#pragma once

#include "Common.h"
#include "SharedDefines.h"
#include "Timer.h"
#include "AllocatorWithCategory.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

class ChatHandler;

// Bounded fixed histogram of whole-millisecond world processing durations.
// One bucket per millisecond for 0..BUCKETS-1, plus an overflow bucket for
// >= BUCKETS. Zero allocation and no locks: the world thread is the sole
// writer and reader. PercentileBucket(p) returns the smallest bucket b such
// that at least p percent of the samples are <= b milliseconds.
struct PerfProcessingHistogram
{
	static constexpr uint32 BUCKETS = 250;
	uint64 counts[BUCKETS + 1] = { 0 };
	uint64 totalMs = 0;

	void Record(uint32 ms) { ++counts[ms < BUCKETS ? ms : BUCKETS]; totalMs += ms; }
	uint64 Count() const;
	uint32 PercentileBucket(uint32 p) const;
	uint64 TotalMs() const { return totalMs; }
	void Reset() { std::fill(counts, counts + (BUCKETS + 1), 0); totalMs = 0; }
	static bool SelfTest();
};

// Bounded nonblocking sink for world processing telemetry (R6, TW-003).
// The world thread only calls Enqueue(): a fixed-capacity queue bounds memory
// and never waits on the consumer. A dedicated writer thread owns an isolated
// FILE* (its own stream, never the shared sLog/stdout stream), so a consumer
// that blocks on open or write can stall only the writer thread, never the
// world thread. Drops are counted and exposed: the writer emits a bounded drop
// notice when it resumes, and Stop() reports the counters.
struct BoundedTelemetrySink
{
	static constexpr size_t MAX_QUEUED_LINES = 64;
	static constexpr size_t MAX_LINE_CHARS = 512;

	bool Start(const char* path);
	void Stop();
	void Enqueue(const std::string& line);
	uint64_t DroppedFull() const;
	uint64_t Unsent() const;
	bool OpenFailed() const;
	size_t QueueSize() const;
	// True when the most recent Stop() joined the writer (its
	// FILE* is closed and its writes are complete); false when it detached
	// it.
	bool LastWriterJoined() const;
	// Counters captured by the most recent Stop() (state is
	// released there, so the live accessors read zero afterwards).
	uint64_t LastDroppedFull() const;
	uint64_t LastUnsent() const;
	static bool SelfTest();

private:
	struct State;
	std::shared_ptr<State> state;
	std::thread worker;
	std::atomic<bool> lastJoined{ false };
	std::atomic<uint64_t> lastDroppedFull{ 0 };
	std::atomic<uint64_t> lastUnsent{ 0 };
	void WorkerLoop();
};

struct PerformanceMonitor : public IPerfMonitor
{
	PerformanceMonitor();

	bool StartTelemetrySink(const char* path) { return telemetrySink.Start(path); }
	void StopTelemetrySink() { telemetrySink.Stop(); }
	BoundedTelemetrySink telemetrySink;

	void Initialize();
	void FrameStart();
	void FrameEnd(uint32 delta);

	void SetReportInterval(uint32 IntervalInSeconds);

	void ReportCPU(ChatHandler& Handler);
	void ReportMemory(ChatHandler& Handler);
	void RecordProcessingTime(uint32 ms);
	void ReportProcessingTime(uint64 tickIntervalTotalMs);

	virtual void ReportAlloc(const char* Category, size_t Bytes) override;
	virtual void ReportDealloc(const char* Category, size_t Bytes) override;

	XStatTimer Tick;
	XStatTimer WorldSleep;
	XStatTimer WorldTick;
	XStatTimer UpdateSession;
	XStatTimer MapManager;
	XStatTimer TEST0;
	PerfProcessingHistogram processingHistogram;
	// Monotonic sequence of emitted processing-telemetry report lines.
	uint64_t telemetrySeq = 0;

protected:

	uint32 QPC_Counter = 0;

	void ReportPerformanceToDB();

	IntervalTimer IntervalReport;

	using MemBytesMap = std::unordered_map<const char*, int64>;
	MemBytesMap MemBytes;
	std::mutex MemBytesGuard;
};

extern PerformanceMonitor sPerfMonitor;