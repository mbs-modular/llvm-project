//===- llvm/Support/TimeProfiler.h - Hierarchical Time Profiler -*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This provides lightweight and dependency-free machinery to trace execution
// time around arbitrary code. Three API flavors are available.
//
// The primary API uses a RAII object to trigger tracing:
//
// \code
//   {
//     TimeTraceScope scope("my_event_name");
//     ...my code...
//   }
// \endcode
//
// If the code to be profiled does not have a natural lexical scope then
// it is also possible to start and end events with respect to an implicit
// per-thread stack of profiling entries:
//
// \code
//   timeTraceProfilerBegin("my_event_name");
//   ...my code...
//   timeTraceProfilerEnd();  // must be called on all control flow paths
// \endcode
//
// Finally, it is also possible to manually create, begin and complete time
// profiling entries. This API allows an entry to be created in one
// context, stored, then completed in another. The completing context need not
// be on the same thread as the creating context:
//
// \code
//   auto entry = timeTraceProfilerBeginEntry("my_event_name");
//   ...
//   // Possibly on a different thread
//   entry.begin(); // optional, if the event start time should be decoupled
//                  // from entry creation
//   ...my code...
//   timeTraceProfilerEndEntry(std::move(entry));
// \endcode
//
// Time profiling entries can be given an arbitrary name and, optionally,
// an arbitrary 'detail' string. The resulting trace will include 'Total'
// entries summing the time spent for each name. Thus, it's best to choose
// names to be fairly generic, and rely on the detail field to capture
// everything else of interest.
//
// To avoid lifetime issues name and detail strings are copied into the event
// entries at their time of creation. Care should be taken to make string
// construction cheap to prevent 'Heisenperf' effects. In particular, the
// 'detail' argument may be a string-returning closure:
//
// \code
//   int n;
//   {
//     TimeTraceScope scope("my_event_name",
//                          [n]() { return (Twine("x=") + Twine(n)).str(); });
//     ...my code...
//   }
// \endcode
// The closure will not be called if tracing is disabled. Otherwise, the
// resulting string will be directly moved into the entry.
//
// If string construction is a significant cost it is possible to construct
// the entry outside of the critical section:
//
// \code
//   auto entry = timeTraceProfilerBeginEntry("my_event_name",
//                                            [=]() { ... expensive ... });
//   ...non critical code...
//   entry.begin();
//   ...my critical code...
//   timeTraceProfilerEndEntry(std::move(entry));
// \endcode
//
// The main process should begin with a timeTraceProfilerInitialize, and
// finish with timeTraceProfileWrite and timeTraceProfilerCleanup calls.
// Each new thread should begin with a timeTraceProfilerInitialize, and
// finish with a timeTraceProfilerFinishThread call.
//
// Timestamps come from std::chrono::high_resolution_clock, so all threads
// see the same time at the highest available resolution.
//
// Currently, there are a number of compatible viewers:
//  - chrome://tracing is the original chromium trace viewer.
//  - http://ui.perfetto.dev is the replacement for the above, under active
//    development by Google as part of the 'Perfetto' project.
//  - https://www.speedscope.app/ has also been reported as an option.
//
// Future work:
//  - Support akin to LLVM_DEBUG for runtime enable/disable of named tracing
//    families for non-debug builds which wish to support optional tracing.
//  - Evaluate the detail closures at profile write time to avoid
//    stringification costs interfering with tracing.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_SUPPORT_TIMEPROFILER_H
#define LLVM_SUPPORT_TIMEPROFILER_H

#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/Support/Error.h"

#include <chrono>

namespace llvm {

class raw_pwrite_stream;

struct TimeTraceProfiler;
TimeTraceProfiler *getTimeTraceProfilerInstance();

/// Initialize the time trace profiler.
/// This sets up the global \p TimeTraceProfilerInstance
/// variable to be the profiler instance.
void timeTraceProfilerInitialize(unsigned TimeTraceGranularity,
                                 StringRef ProcName);

/// Cleanup the time trace profiler, if it was initialized.
void timeTraceProfilerCleanup();

/// Finish a time trace profiler running on a worker thread.
void timeTraceProfilerFinishThread();

/// Is the time trace profiler enabled, i.e. initialized?
inline bool timeTraceProfilerEnabled() {
  return getTimeTraceProfilerInstance() != nullptr;
}

/// Write profiling data to output stream.
/// Data produced is JSON, in Chrome "Trace Event" format, see
/// https://docs.google.com/document/d/1CvAClvFfyA5R-PhYUmn5OOQtYMH4h6I0nSsKchNAySU/preview
void timeTraceProfilerWrite(raw_pwrite_stream &OS);

/// Write profiling data to a file.
/// The function will write to \p PreferredFileName if provided, if not
/// then will write to \p FallbackFileName appending .time-trace.
/// Returns a StringError indicating a failure if the function is
/// unable to open the file for writing.
Error timeTraceProfilerWrite(StringRef PreferredFileName,
                             StringRef FallbackFileName);

/// Manually begin a time section, with the given \p Name and \p Detail.
/// Profiler copies the string data, so the pointers can be given into
/// temporaries. Time sections can be hierarchical; every Begin must have a
/// matching End pair but they can nest.
void timeTraceProfilerBegin(StringRef Name, StringRef Detail);
void timeTraceProfilerBegin(StringRef Name,
                            llvm::function_ref<std::string()> Detail);

/// Manually end the last time section.
void timeTraceProfilerEnd();

/// Represents an open or completed time section entry to be captured.
struct TimeTraceProfilerEntry {
  // We use the high_resolution_clock for maximum precision.
  // It may not be steady (ClockType::is_steady may be false), which means
  // it is possible for profiles to yield invalid durations during leap
  // second transitions or other system clock adjustments. This rare glitch
  // seems worthwhile in exchange for the precision.
  // Under linux glibc++ the high_resolution_clock is consistent across threads
  // which is necessary for building cross-thread entries.
  // It is unknown whether that's the case under Windows, and the C++ standard
  // does not appear to impose any thread consistency on any of the clocks.

  using ClockType = std::chrono::high_resolution_clock;
  using TimePointType = std::chrono::time_point<ClockType>;

  TimePointType Start;
  TimePointType End;
  const std::string Name;
  const std::string Detail;

  TimeTraceProfilerEntry() : Start(TimePointType()), End(TimePointType()) {}

  TimeTraceProfilerEntry(TimePointType &&S, TimePointType &&E, std::string &&N,
                         std::string &&Dt)
      : Start(std::move(S)), End(std::move(E)), Name(std::move(N)),
        Detail(std::move(Dt)) {}

  // Calculate timings for FlameGraph. Cast time points to microsecond precision
  // rather than casting duration. This avoids truncation issues causing inner
  // scopes overruning outer scopes.
  ClockType::rep getFlameGraphStartUs(TimePointType StartTime) const {
    return (std::chrono::time_point_cast<std::chrono::microseconds>(Start) -
            std::chrono::time_point_cast<std::chrono::microseconds>(StartTime))
        .count();
  }

  ClockType::rep getFlameGraphDurUs() const {
    return (std::chrono::time_point_cast<std::chrono::microseconds>(End) -
            std::chrono::time_point_cast<std::chrono::microseconds>(Start))
        .count();
  }

  /// Resets the starting time of this entry to now. By default the entry
  /// will have taken its start time to be the time of entry construction.
  /// But if the entry has been constructed early so as to keep detail string
  /// construction out of the measured section then this method can be called
  /// to signal measurement should begin. If the time profiler is not
  /// initialized, the overhead is a single branch.
  void begin();
};

/// Returns an entry with starting time of now and Name and Detail.
/// The entry can later be added to the trace by timeTraceProfilerEndEntry
/// below when the tracked event has completed. If the time profiler is not
/// initialized, the overhead is constructing an empty entry without any
/// use of the global clock.
TimeTraceProfilerEntry timeTraceProfilerBeginEntry(StringRef Name,
                                                   StringRef Detail = {});
TimeTraceProfilerEntry
timeTraceProfilerBeginEntry(StringRef Name,
                            llvm::function_ref<std::string()> Detail);

/// Ends the Entry returned by timeTraceProfilerBeginEntry above. The entry is
/// recorded by the current thread, which need not be the same as the thread
/// which executed the original timeTraceProfilerBeginEntry call. If the time
/// profiler is not initialized, the overhead is a single branch.
void timeTraceProfilerEndEntry(TimeTraceProfilerEntry &&Entry);

/// The TimeTraceScope is a helper class to call the begin and end functions
/// of the time trace profiler.  When the object is constructed, it begins
/// the section; and when it is destroyed, it stops it. If the time profiler
/// is not initialized, the overhead is a single branch.
struct TimeTraceScope {

  TimeTraceScope() = delete;
  TimeTraceScope(const TimeTraceScope &) = delete;
  TimeTraceScope &operator=(const TimeTraceScope &) = delete;
  TimeTraceScope(TimeTraceScope &&) = delete;
  TimeTraceScope &operator=(TimeTraceScope &&) = delete;

  TimeTraceScope(StringRef Name) {
    if (getTimeTraceProfilerInstance() != nullptr)
      timeTraceProfilerBegin(Name, StringRef(""));
  }
  TimeTraceScope(StringRef Name, StringRef Detail) {
    if (getTimeTraceProfilerInstance() != nullptr)
      timeTraceProfilerBegin(Name, Detail);
  }
  TimeTraceScope(StringRef Name, llvm::function_ref<std::string()> Detail) {
    if (getTimeTraceProfilerInstance() != nullptr)
      timeTraceProfilerBegin(Name, Detail);
  }
  ~TimeTraceScope() {
    if (getTimeTraceProfilerInstance() != nullptr)
      timeTraceProfilerEnd();
  }
};

} // end namespace llvm

#endif
