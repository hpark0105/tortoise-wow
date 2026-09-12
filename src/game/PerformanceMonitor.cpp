#include "PerformanceMonitor.h"
#include "Log.h"
#include "Chat.h"
#include "World.h"
#include "MapManager.h"

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
	sLog.outInfo("World processing telemetry: samples=%llu proc p50=%ums p95=%ums p99=%ums avg_proc=%ums avg_tick_interval=%ums%s (processing time is separate from the elapsed tick interval)",
		(unsigned long long)count, p50, p95, p99, (unsigned long long)avgProc, (unsigned long long)avgTick, overflowNote);
}
