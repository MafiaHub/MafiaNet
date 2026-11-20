/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */
#include <benchmark/benchmark.h>
#include <valgrind/callgrind.h>

namespace
{
class CallgrindProfiler final : public benchmark::ProfilerManager
{
public:
	void AfterSetupStart() override { CALLGRIND_START_INSTRUMENTATION; }
	void BeforeTeardownStop() override { CALLGRIND_STOP_INSTRUMENTATION; }
};
}

int main(int argc, char **argv)
{
	benchmark::Initialize(&argc, argv);
	if (benchmark::ReportUnrecognizedArguments(argc, argv))
		return 1;
	CallgrindProfiler profiler;
	if (RUNNING_ON_VALGRIND)
		benchmark::RegisterProfilerManager(&profiler);
	benchmark::RunSpecifiedBenchmarks();
	benchmark::RegisterProfilerManager(nullptr);
	benchmark::Shutdown();
}
