#include "PerformanceMonitor.h"
#include "Log.h"
#include "Chat.h"
#include "World.h"
#include "MapManager.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

PerformanceMonitor sPerfMonitor;

PerformanceMonitor::PerformanceMonitor()
{
	gPerfMonitorInterface = this;
}

void PerformanceMonitor::Initialize()
{
	// in seconds
	uint32 IntervalReportValue = sWorld.getConfig(CONFIG_UINT32_PERFORMANCE_REPORT_INTERVAL);

	// convert to milliseconds
	IntervalReport.SetInterval(IntervalReportValue * 1000);
	IntervalReport.SetCurrent(0);
}

void PerformanceMonitor::FrameStart()
{
	if (!g_bEnableStatGather)
	{
		return;
	}
	Tick.FrameStart();
	WorldSleep.FrameStart();
	WorldTick.FrameStart();
	TEST0.FrameStart();
	UpdateSession.FrameStart();
	MapManager.FrameStart();

	const MapManager::MapMapType& Maps = sMapMgr.Maps();
	for (MapManager::MapMapType::const_iterator iter = Maps.cbegin(); iter != Maps.cend(); iter++)
	{
		iter->second->MovementPerfTimer.FrameStart();
		iter->second->SpellPerfTimer.FrameStart();
		iter->second->UpdateTimer.FrameStart();
	}
}

void PerformanceMonitor::FrameEnd(uint32 delta)
{
	if (!g_bEnableStatGather)
	{
		return;
	}
	WorldSleep.FrameEnd();
	Tick.FrameEnd();
	WorldTick.FrameEnd();
	TEST0.FrameEnd();
	UpdateSession.FrameEnd();
	MapManager.FrameEnd();

	const MapManager::MapMapType& Maps = sMapMgr.Maps();
	for (MapManager::MapMapType::const_iterator iter = Maps.cbegin(); iter != Maps.cend(); iter++)
	{
		Map* CurrentMap = iter->second;
		CurrentMap->MovementPerfTimer.FrameEnd();
		CurrentMap->SpellPerfTimer.FrameEnd();
		CurrentMap->UpdateTimer.FrameEnd();
	}

	QPC_Counter = CPU::qpc_counter;
	CPU::qpc_counter = 0;

	IntervalReport.Update(delta);
	if (IntervalReport.Passed())
	{
		IntervalReport.Reset();
		ReportPerformanceToDB();
	}
}

void PerformanceMonitor::SetReportInterval(uint32 IntervalInSeconds)
{
	IntervalReport.SetInterval(IntervalInSeconds * 1000);
}

void PerformanceMonitor::ReportCPU(ChatHandler& Handler)
{
	/// #### CPU ####
	Handler.PSendSysMessage("CPU Performance report");
	Handler.PSendSysMessage("QPC counter: %u", QPC_Counter);

	auto GetPercentOfLambda = [](XStatTimer Local, XStatTimer Global) -> float
		{
			return 100.0f * float(Local.result) / float(Global.result);
		};

	auto ReportStatWithParentLambda = [&Handler, &GetPercentOfLambda](const char* Name, const XStatTimer& Timer, const XStatTimer& Parent, int32 Level = 1)
		{
			char BeginShit[256];
			if (Level == 0)
			{
				memset(BeginShit, 0, sizeof(BeginShit));
			}
			else
			{
				int32 Cursor = 0;
				for (int32 i = 0; i < Level; i++)
				{
					BeginShit[Cursor++] = '-';
					BeginShit[Cursor++] = '>';
					BeginShit[Cursor++] = ' ';
				}
				BeginShit[Cursor] = 0;
			}
			Handler.PSendSysMessage(" %s %s: [%u] %2.2fms, %2.1f%%, min: %2.2fms, max: %2.2fms", 
				BeginShit, Name, Timer.count, Timer.result, GetPercentOfLambda(Timer, Parent), Timer.MinResult, Timer.MaxResult);
		};

	Handler.PSendSysMessage("Tick: %2.2f", Tick.result);
	Handler.PSendSysMessage("WorldSleep: %2.2f", WorldSleep.result);

	ReportStatWithParentLambda("TEST0", TEST0, Tick);
	ReportStatWithParentLambda("WorldTick", WorldTick, Tick);
	ReportStatWithParentLambda("UpdateSession", UpdateSession, WorldTick, 2);
	ReportStatWithParentLambda("MapManager", MapManager, WorldTick, 2);

	/// #### Maps ####
	Handler.PSendSysMessage("Map performance report. Some maps update async");

	const MapManager::MapMapType& Maps = sMapMgr.Maps();
	for (MapManager::MapMapType::const_iterator iter = Maps.cbegin(); iter != Maps.cend(); iter++)
	{
		Map* CurrentMap = iter->second;
		Handler.PSendSysMessage("Map: %u, InstanceID: %u", CurrentMap->GetId(), CurrentMap->GetInstanceId());
		ReportStatWithParentLambda("Update", CurrentMap->UpdateTimer, MapManager);
		ReportStatWithParentLambda("Movement", CurrentMap->MovementPerfTimer, CurrentMap->UpdateTimer, 2);
		ReportStatWithParentLambda("Spell", CurrentMap->SpellPerfTimer, CurrentMap->UpdateTimer, 2);
	}
}

void PerformanceMonitor::ReportMemory(ChatHandler& Handler)
{
	Handler.PSendSysMessage("Memory Performance report");
	int64 ProcessMemory = (int64)Memory::GetProcessMemory(); // fix later
	int64 AllTrackedBytes = 0;
	std::lock_guard guard{ MemBytesGuard };

	std::unordered_map<std::string, int64> FixedMap;

	for (auto& [key, value] : MemBytes)
	{
		FixedMap[key] += value;
		AllTrackedBytes += value;
	}

	auto GetValueAsMbLambda = [](int64 InValue) -> double
		{
			double fConvertValue = InValue;
			fConvertValue /= 1024.0; // to kbytes
			fConvertValue /= 1024.0; // to mbytes
			return fConvertValue;
		};

	double fProcessMemory = GetValueAsMbLambda(ProcessMemory);
	double fTrackedMemory = GetValueAsMbLambda(AllTrackedBytes);
	Handler.PSendSysMessage("All process memory: %.2fMb (Tracked: %.2fMb)", fProcessMemory, fTrackedMemory);

	for (auto& [key, value] : FixedMap)
	{
		Handler.PSendSysMessage("-> %s - %.2fMb", key.c_str(), GetValueAsMbLambda(value));
	}
}

void PerformanceMonitor::ReportPerformanceToDB()
{

}

void PerformanceMonitor::ReportAlloc(const char* Category, size_t Bytes)
{
	std::lock_guard guard{ MemBytesGuard };
	MemBytes[Category] += Bytes;
}

void PerformanceMonitor::ReportDealloc(const char* Category, size_t Bytes)
{
	std::lock_guard guard{ MemBytesGuard };
	MemBytes[Category] -= Bytes;
}
// --- World processing-time telemetry (Perf.ProcessingTelemetry) -----------

uint64 PerfProcessingHistogram::Count() const
{
	uint64 count = 0;
	for (uint64 c : counts)
		count += c;
	return count;
}

uint32 PerfProcessingHistogram::PercentileBucket(uint32 p) const
{
	const uint64 count = Count();
	if (count == 0 || p == 0)
		return 0;
	const uint64 target = (count * p + 99) / 100;
	uint64 cumulative = 0;
	for (uint32 bucket = 0; bucket <= BUCKETS; ++bucket)
	{
		cumulative += counts[bucket];
		if (cumulative >= target)
			return bucket;
	}
	return BUCKETS;
}

bool PerfProcessingHistogram::SelfTest()
{
	PerfProcessingHistogram histogram;
	if (histogram.Count() != 0 || histogram.PercentileBucket(99) != 0)
		return false;

	for (uint32 ms = 0; ms < 100; ++ms)
		histogram.Record(ms);
	if (histogram.Count() != 100 || histogram.PercentileBucket(50) != 49 ||
		histogram.PercentileBucket(95) != 94 || histogram.PercentileBucket(99) != 98)
		return false;

	PerfProcessingHistogram single;
	single.Record(7);
	if (single.Count() != 1 || single.PercentileBucket(50) != 7 || single.TotalMs() != 7)
		return false;

	PerfProcessingHistogram overflow;
	for (int i = 0; i < 10; ++i)
		overflow.Record(300);
	if (overflow.PercentileBucket(99) != PerfProcessingHistogram::BUCKETS)
		return false;

	PerfProcessingHistogram mixed;
	for (uint32 ms = 1; ms <= 100; ++ms)
		mixed.Record(ms);
	return mixed.PercentileBucket(95) == 95 && mixed.PercentileBucket(99) == 99;
}


// ---------------------------------------------------------------------------
// R6 (KAP-546 / TW-003): bounded, nonblocking telemetry emission.
//
// The world thread only calls Enqueue(): a fixed-capacity queue bounds memory
// and never waits on the consumer. The dedicated writer thread owns an
// isolated FILE* (its own stream, never the shared sLog/stdout stream), so a
// consumer that blocks on open or write can stall only the writer thread,
// never the world thread. Drops are counted and exposed: the writer emits a
// bounded drop notice when it resumes, and Stop() reports the counters.
// ---------------------------------------------------------------------------

struct BoundedTelemetrySink::State
{
	std::mutex guard;
	std::condition_variable cv;
	std::deque<std::string> queue;
	std::string path;
	FILE* file = nullptr;              // writer thread only
	std::atomic<bool> fileFailed{ false };
	bool stopping = false;
	bool exited = false;
	std::atomic<uint64_t> droppedFull{ 0 };
	std::atomic<uint64_t> unsent{ 0 };
};

bool BoundedTelemetrySink::Start(const char* path)
{
	if (worker.joinable())
		return false;
	auto s = std::make_shared<State>();
	s->path = path ? path : "";
	state = s;
	worker = std::thread(&BoundedTelemetrySink::WorkerLoop, this);
	return true;
}

void BoundedTelemetrySink::Stop()
{
	if (!state)
		return;
	{
		std::lock_guard<std::mutex> lk(state->guard);
		state->stopping = true;
		state->cv.notify_all();
	}
	{
		std::unique_lock<std::mutex> lk(state->guard);
		state->cv.wait_for(lk, std::chrono::seconds(30), [this]() { return state->exited; });
	}
	bool exited = false;
	{
		std::lock_guard<std::mutex> lk(state->guard);
		exited = state->exited;
	}
	if (exited)
		worker.join();
	else
		// The writer is still blocked on a consumer while the world is
		// shutting down. Detach so shutdown stays bounded; the writer keeps
		// its own shared_ptr to the state object until the process exits.
		worker.detach();
	lastJoined = exited;
	uint64_t pending = 0;
	{
		std::lock_guard<std::mutex> lk(state->guard);
		pending = state->queue.size();
	}
	if (pending)
		state->unsent += pending;
	sLog.outInfo("World telemetry sink stopped: dropped_full=%llu unsent=%llu",
		(unsigned long long)state->droppedFull.load(), (unsigned long long)state->unsent.load());
	lastDroppedFull = state->droppedFull.load();
	lastUnsent = state->unsent.load();
	state = nullptr;
}

void BoundedTelemetrySink::Enqueue(const std::string& line)
{
	if (!state)
		return;
	std::string bounded = line;
	if (bounded.size() > MAX_LINE_CHARS)
		bounded.resize(MAX_LINE_CHARS);
	{
		std::lock_guard<std::mutex> lk(state->guard);
		if (state->stopping)
		{
			state->unsent++;
			return;
		}
		if (state->queue.size() >= MAX_QUEUED_LINES)
		{
			state->droppedFull++;
			return;
		}
		state->queue.push_back(std::move(bounded));
		state->cv.notify_one();
	}
}

uint64_t BoundedTelemetrySink::DroppedFull() const
{
	return state ? state->droppedFull.load() : 0;
}

uint64_t BoundedTelemetrySink::Unsent() const
{
	return state ? state->unsent.load() : 0;
}

bool BoundedTelemetrySink::OpenFailed() const
{
	return state ? state->fileFailed.load() : false;
}

size_t BoundedTelemetrySink::QueueSize() const
{
	if (!state)
		return 0;
	std::lock_guard<std::mutex> lk(state->guard);
	return state->queue.size();
}

bool BoundedTelemetrySink::LastWriterJoined() const
{
	return lastJoined.load();
}

uint64_t BoundedTelemetrySink::LastDroppedFull() const
{
	return lastDroppedFull.load();
}

uint64_t BoundedTelemetrySink::LastUnsent() const
{
	return lastUnsent.load();
}

void BoundedTelemetrySink::WorkerLoop()
{
	// Keep the state object alive even if Stop() detaches this thread.
	std::shared_ptr<State> s = state;
	State& st = *s;
	// Open inside the writer thread: a consumer that blocks on open (for
	// example a FIFO with no reader) blocks only this thread, never the
	// world thread.
	st.file = std::fopen(st.path.c_str(), "a");
	if (!st.file)
	{
		st.fileFailed = true;
		sLog.outError("World telemetry sink: cannot open '%s'; telemetry emission disabled", st.path.c_str());
	}
	std::unique_lock<std::mutex> lk(st.guard);
	// Writer-local cursor into the cumulative drop counter: each notice
	// covers the drops accumulated since the previous successful write.
	uint64_t noticed = 0;
	while (true)
	{
		if (st.stopping)
			break;
		if (st.queue.empty())
		{
			st.cv.wait(lk);
			continue;
		}
		std::string line = std::move(st.queue.front());
		st.queue.pop_front();
		lk.unlock();
		if (st.file)
		{
			bool ok = true;
			// Expose drops in-band when the consumer recovers: the notice
			// precedes the first line written after the backlog.
			uint64_t total = st.droppedFull.load();
			if (total > noticed)
			{
				char notice[MAX_LINE_CHARS];
				int n = std::snprintf(notice, sizeof(notice),
					"World telemetry: %llu report line(s) dropped while the sink queue was full",
					(unsigned long long)(total - noticed));
				ok = n > 0 && std::fwrite(notice, 1, (size_t)n, st.file) == (size_t)n
					&& std::fputc('\n', st.file) != EOF;
				if (ok)
					noticed = total;
			}
			if (ok)
				ok = std::fwrite(line.data(), 1, line.size(), st.file) == line.size()
					&& std::fputc('\n', st.file) != EOF;
			if (ok)
				ok = std::fflush(st.file) == 0;
			if (!ok)
			{
				// I/O failed: stop writing, keep draining so shutdown stays
				// bounded, and count the unsent line.
				st.unsent++;
				std::lock_guard<std::mutex> lk2(st.guard);
				if (st.file)
				{
					std::fclose(st.file);
					st.file = nullptr;
				}
				st.fileFailed = true;
			}
		}
		else
			st.unsent++;
		lk.lock();
	}
	lk.unlock();
	if (st.file)
	{
		std::fclose(st.file);
		st.file = nullptr;
	}
	{
		std::lock_guard<std::mutex> lk2(st.guard);
		st.exited = true;
	}
	st.cv.notify_all();
}

namespace
{
	bool R6WaitFor(std::function<bool()> pred, uint32 timeoutMs)
	{
		auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
		while (std::chrono::steady_clock::now() < deadline)
		{
			if (pred())
				return true;
			std::this_thread::sleep_for(std::chrono::milliseconds(25));
		}
		return pred();
	}
}

bool BoundedTelemetrySink::SelfTest()
{
	// The world-thread contract: Enqueue is bounded and never blocks,
	// drops are counted and exposed, and every enqueued line is accounted
	// for. Temp files are removed on every exit path.
	const char* path = "world_telemetry_selftest.log";
	const char* path2 = "world_telemetry_selftest2.log";
	std::remove(path);
	std::remove(path2);
	{
		BoundedTelemetrySink sink;
		if (!sink.Start(path))
		{
			sLog.outError("R6 self-test failed: block 1 sink start");
			std::remove(path);
			return false;
		}
		uint64 enqueued = 0;
		size_t observedMax = 0;
		// The producer (in-memory lock+push) outpaces any realistic writer
		// (lock + fwrite + fflush per line), so the bounded queue fills and
		// drops start. Producer bursts keep the drain bounded in both
		// extremes: a slow writer drops early (small backlog), a fast
		// writer keeps the queue drained (and then drains fast).
		for (size_t burst = 0; burst < 128 && sink.DroppedFull() == 0; ++burst)
			for (size_t i = 0; i < 256; ++i)
			{
				sink.Enqueue("selftest line " + std::to_string(enqueued++));
				size_t q = sink.QueueSize();
				if (q > observedMax)
					observedMax = q;
			}
		for (uint64 i = 0; i < 128; ++i)
		{
			sink.Enqueue("selftest line " + std::to_string(enqueued++));
			size_t q = sink.QueueSize();
			if (q > observedMax)
				observedMax = q;
		}
		bool ok = sink.DroppedFull() > 0 && observedMax <= MAX_QUEUED_LINES;
		if (ok)
			ok = R6WaitFor([&sink]() { return sink.QueueSize() == 0; }, 60000);
		// Counters are cumulative; capture them before Stop() releases
		// state.
		uint64 dropped = sink.DroppedFull();
		uint64 unsent = sink.Unsent();
		sink.Stop();
		if (ok && sink.LastWriterJoined())
		{
			// The writer joined: the file is complete and closed, so its
			// contents are checkable.
			std::ifstream in(path);
			std::vector<std::string> fileLines;
			std::string fileLine;
			while (std::getline(in, fileLine))
				fileLines.push_back(fileLine);
			uint64 emitted = 0;
			uint64 noticeDropped = 0;
			bool anyNotice = false;
			std::vector<uint64> indices;
			for (size_t i = 0; i < fileLines.size() && ok; ++i)
			{
				const std::string& line = fileLines[i];
				if (line.rfind("World telemetry: ", 0) == 0)
				{
					anyNotice = true;
					uint64 n = 0;
					if (std::sscanf(line.c_str(), "World telemetry: %llu report line(s) dropped",
									(unsigned long long*)&n) != 1)
						ok = false;
					else
						noticeDropped += n;
				}
				else
				{
					uint64 idx = 0;
					if (line.rfind("selftest line ", 0) != 0 ||
						std::sscanf(line.c_str() + 14, "%llu", (unsigned long long*)&idx) != 1)
						ok = false;
					else
					{
						if (!indices.empty() && idx <= indices.back())
							ok = false;
						indices.push_back(idx);
						++emitted;
					}
				}
			}
			// Conservation: every line is emitted, dropped, or unsent, and
			// the in-band notices account for every drop.
			ok = ok && anyNotice && emitted > 0 && noticeDropped == dropped
				&& (emitted + dropped + unsent == enqueued);
		}
		else if (ok)
		{
			// The writer was still active at Stop (pathologically slow
			// storage): the file cannot be trusted; the counters alone are
			// the check.
			ok = dropped > 0;
		}
		std::remove(path);
		if (!ok)
		{
			sLog.outError("R6 self-test failed: block 1 (observedMax=%u dropped=%llu unsent=%llu enqueued=%llu joined=%d)",
				(unsigned)observedMax, (unsigned long long)dropped,
				(unsigned long long)unsent, (unsigned long long)enqueued,
				(int)sink.LastWriterJoined());
			return false;
		}
	}
	{
		// Bounded shutdown with pending lines: Stop returns within its
		// bound and counts pending lines as unsent instead of draining
		// forever.
		BoundedTelemetrySink sink2;
		if (!sink2.Start(path2))
		{
			sLog.outError("R6 self-test failed: block 2 sink start");
			std::remove(path2);
			return false;
		}
		for (uint64 i = 0; i < 3; ++i)
			sink2.Enqueue("selftest tail " + std::to_string(i));
		auto before = std::chrono::steady_clock::now();
		sink2.Stop();
		auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now() - before).count();
		// The Last* accessors hold the counters Stop() computed before
		// releasing the state (the live ones read zero after Stop).
		bool ok = elapsedMs < 35000 && sink2.LastDroppedFull() == 0;
		if (ok && sink2.LastWriterJoined())
		{
			size_t written = 0;
			std::ifstream in(path2);
			std::string fileLine;
			while (std::getline(in, fileLine))
				++written;
			ok = written <= 3 && sink2.LastUnsent() == 3 - written;
		}
		std::remove(path2);
		if (!ok)
		{
			sLog.outError("R6 self-test failed: block 2 (elapsedMs=%lld)", (long long)elapsedMs);
			return false;
		}
	}
	{
		// Fail-closed on an unopenable sink: emission is disabled, queued
		// lines are counted unsent, and shutdown stays bounded. The open
		// runs in the writer thread, so a consumer that stalls the open
		// (observed on some volume filesystems) blocks only the writer,
		// never the world thread; that case is accepted when Stop stays
		// within its bound.
		BoundedTelemetrySink bad;
		if (!bad.Start("world_telemetry_selftest_nodir/selftest.log"))
		{
			sLog.outError("R6 self-test failed: block 3 sink start");
			return false;
		}
		if (!R6WaitFor([&bad]() { return bad.OpenFailed(); }, 15000))
		{
			auto before = std::chrono::steady_clock::now();
			bad.Stop();
			auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now() - before).count();
			if (elapsedMs >= 35000)
			{
				sLog.outError("R6 self-test failed: block 3 bounded shutdown of a stalled open (elapsedMs=%lld)",
					(long long)elapsedMs);
				return false;
			}
			sLog.outInfo("R6 self-test: block 3 open still pending after 15s; verified bounded shutdown instead (elapsedMs=%lld)",
				(long long)elapsedMs);
			return true;
		}
		bad.Enqueue("selftest a");
		bad.Enqueue("selftest b");
		if (!R6WaitFor([&bad]() { return bad.Unsent() == 2; }, 15000))
		{
			bad.Stop();
			sLog.outError("R6 self-test failed: block 3 unsent accounting");
			return false;
		}
		bad.Stop();
		if (bad.DroppedFull() != 0)
		{
			sLog.outError("R6 self-test failed: block 3 unexpected drops");
			return false;
		}
	}
	return true;
}

void PerformanceMonitor::RecordProcessingTime(uint32 ms)
{
	processingHistogram.Record(ms);
}

void PerformanceMonitor::ReportProcessingTime(uint64 tickIntervalTotalMs)
{
	const uint64 count = processingHistogram.Count();
	if (count == 0)
		return;
	const uint32 p50 = processingHistogram.PercentileBucket(50);
	const uint32 p95 = processingHistogram.PercentileBucket(95);
	const uint32 p99 = processingHistogram.PercentileBucket(99);
	const uint64 totalMs = processingHistogram.TotalMs();
	processingHistogram.Reset();
	const uint64 avgProc = totalMs / count;
	const uint64 avgTick = tickIntervalTotalMs / count;
	const char* overflowNote = (p95 >= PerfProcessingHistogram::BUCKETS || p99 >= PerfProcessingHistogram::BUCKETS)
		? " (percentiles hit the histogram overflow bucket; values are lower bounds)" : "";
	char buffer[BoundedTelemetrySink::MAX_LINE_CHARS];
	const uint64 seq = ++telemetrySeq;
	int len = std::snprintf(buffer, sizeof(buffer),
		"World processing telemetry: seq=%llu samples=%llu proc p50=%ums p95=%ums p99=%ums avg_proc=%ums avg_tick_interval=%ums%s (processing time is separate from the elapsed tick interval)",
		(unsigned long long)seq, (unsigned long long)count, p50, p95, p99,
		(unsigned long long)avgProc, (unsigned long long)avgTick, overflowNote);
	if (len < 0 || (size_t)len >= sizeof(buffer))
		std::snprintf(buffer, sizeof(buffer),
			"World processing telemetry: seq=%llu line truncated (samples=%llu)",
		(unsigned long long)seq, (unsigned long long)count);
	telemetrySink.Enqueue(buffer);
}
