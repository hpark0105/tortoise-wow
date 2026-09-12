#pragma once

#include "Common.h"
#include "SharedDefines.h"
#include "Timer.h"
#include "AllocatorWithCategory.h"

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

struct PerformanceMonitor : public IPerfMonitor
{
	PerformanceMonitor();

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

protected:

	uint32 QPC_Counter = 0;

	void ReportPerformanceToDB();

	IntervalTimer IntervalReport;

	using MemBytesMap = std::unordered_map<const char*, int64>;
	MemBytesMap MemBytes;
	std::mutex MemBytesGuard;
};

extern PerformanceMonitor sPerfMonitor;