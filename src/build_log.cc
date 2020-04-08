// Copyright 2011 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// On AIX, inttypes.h gets indirectly included by build_log.h.
// It's easiest just to ask for the printf format macros right away.
#ifndef _WIN32
#ifndef __STDC_FORMAT_MACROS
#define __STDC_FORMAT_MACROS
#endif
#endif

#include "build_log.h"

#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#ifndef _WIN32
#include <inttypes.h>
#include <unistd.h>
#endif

#include <numeric>

#include "build.h"
#include "disk_interface.h"
#include "graph.h"
#include "metrics.h"
#include "parallel_map.h"
#include "util.h"
#if defined(_MSC_VER) && (_MSC_VER < 1800)
#define strtoll _strtoi64
#endif

// Implementation details:
// Each run's log appends to the log file.
// To load, we run through all log entries in series, throwing away
// older runs.
// Once the number of redundant entries exceeds a threshold, we write
// out a new file and replace the existing one with it.

namespace {

const char kFileSignature[] = "# ninja log v%d\n";
const int kOldestSupportedVersion = 5;
const int kCurrentVersion = 5;

const char kFieldSeparator = '\t';

// 64bit MurmurHash2, by Austin Appleby
#if defined(_MSC_VER)
#define BIG_CONSTANT(x) (x)
#else   // defined(_MSC_VER)
#define BIG_CONSTANT(x) (x##LLU)
#endif // !defined(_MSC_VER)
inline
uint64_t MurmurHash64A(const void* key, size_t len) {
  static const uint64_t seed = 0xDECAFBADDECAFBADull;
  const uint64_t m = BIG_CONSTANT(0xc6a4a7935bd1e995);
  const int r = 47;
  uint64_t h = seed ^ (len * m);
  const unsigned char* data = (const unsigned char*)key;
  while (len >= 8) {
    uint64_t k;
    memcpy(&k, data, sizeof k);
    k *= m;
    k ^= k >> r;
    k *= m;
    h ^= k;
    h *= m;
    data += 8;
    len -= 8;
  }
  switch (len & 7)
  {
  case 7: h ^= uint64_t(data[6]) << 48;
          NINJA_FALLTHROUGH;
  case 6: h ^= uint64_t(data[5]) << 40;
          NINJA_FALLTHROUGH;
  case 5: h ^= uint64_t(data[4]) << 32;
          NINJA_FALLTHROUGH;
  case 4: h ^= uint64_t(data[3]) << 24;
          NINJA_FALLTHROUGH;
  case 3: h ^= uint64_t(data[2]) << 16;
          NINJA_FALLTHROUGH;
  case 2: h ^= uint64_t(data[1]) << 8;
          NINJA_FALLTHROUGH;
  case 1: h ^= uint64_t(data[0]);
          h *= m;
  };
  h ^= h >> r;
  h *= m;
  h ^= h >> r;
  return h;
}
#undef BIG_CONSTANT


}  // namespace

// static
uint64_t BuildLog::LogEntry::HashCommand(StringPiece command) {
  METRIC_RECORD("hash command");
  return MurmurHash64A(command.data(), command.size());
}

BuildLog::LogEntry::LogEntry(const HashedStrView& output)
  : output(output) {}

BuildLog::LogEntry::LogEntry(const HashedStrView& output, uint64_t command_hash,
  int start_time, int end_time, TimeStamp restat_mtime)
  : output(output), command_hash(command_hash),
    start_time(start_time), end_time(end_time), mtime(restat_mtime)
{}

BuildLog::BuildLog()
  : log_file_(NULL), needs_recompaction_(false) {}

BuildLog::~BuildLog() {
  Close();
}

bool BuildLog::OpenForWrite(const string& path, const BuildLogUser& user,
                            string* err) {
  if (needs_recompaction_) {
    if (!Recompact(path, user, err))
      return false;
  }

  log_file_ = fopen(path.c_str(), "ab");
  if (!log_file_) {
    *err = strerror(errno);
    return false;
  }
  setvbuf(log_file_, NULL, _IOLBF, BUFSIZ);
  SetCloseOnExec(fileno(log_file_));

  // Opening a file in append mode doesn't set the file pointer to the file's
  // end on Windows. Do that explicitly.
  fseek(log_file_, 0, SEEK_END);

  if (ftell(log_file_) == 0) {
    if (fprintf(log_file_, kFileSignature, kCurrentVersion) < 0) {
      *err = strerror(errno);
      return false;
    }
  }

  return true;
}

bool BuildLog::RecordCommand(Edge* edge, int start_time, int end_time,
                             TimeStamp mtime) {
  string command = edge->EvaluateCommand(true);
  uint64_t command_hash = LogEntry::HashCommand(command);
  for (Node* out : edge->outputs_) {
    const HashedStr& path = out->path_hashed();
    LogEntry* log_entry;
    if (LogEntry** i = entries_.Lookup(path)) {
      log_entry = *i;
    } else {
      log_entry = new LogEntry(path);
      fprintf(stderr, "build_log1\n");
      entries_.insert(Entries::value_type(log_entry->output, log_entry));
    }
    log_entry->command_hash = command_hash;
    log_entry->start_time = start_time;
    log_entry->end_time = end_time;
    log_entry->mtime = mtime;

    if (log_file_) {
      if (!WriteEntry(log_file_, *log_entry))
        return false;
      if (fflush(log_file_) != 0) {
          return false;
      }
    }
  }
  return true;
}

void BuildLog::Close() {
  if (log_file_)
    fclose(log_file_);
  log_file_ = NULL;
}

/// Retrieve the next tab-delimited or LF-delimited piece in the input.
///
/// Specifically, the function searches for a separator, starting at the given
/// input offset. If the function finds the separator, it removes everything up
/// to and including the separator from |*input|, places it in |*out|, and
/// returns true. Otherwise, it zeroes |*out| and returns false.
static bool GetNextPiece(StringPiece* input, char sep, StringPiece* out,
                         size_t start_offset=0) {
  assert(input != out);
  const char* data = input->data();
  const char* split = nullptr;

  // memchr(NULL, NULL, 0) has undefined behavior, so avoid calling memchr when
  // input->data() is nullptr. If input->size() is non-zero, its data() will be
  // non-null.
  if (start_offset < input->size()) {
    split = static_cast<const char*>(
        memchr(data + start_offset, sep, input->size() - start_offset));
  }

  if (split != nullptr) {
    size_t len = split + 1 - data;
    *out = input->substr(0, len);
    *input = input->substr(len);
    return true;
  } else {
    *out = {};
    return false;
  }
}

struct BuildLogInput {
  std::unique_ptr<LoadedFile> file;
  int log_version = 0;

  // Content excluding the file header.
  StringPiece content;
};

static bool OpenBuildLogForReading(const std::string& path,
                                   BuildLogInput* log,
                                   std::string* err) {
  *log = {};

  RealDiskInterface file_reader;
  std::string load_err;
  switch (file_reader.LoadFile(path, &log->file, &load_err)) {
  case FileReader::Okay:
    break;
  case FileReader::NotFound:
    return true;
  default:
    *err = load_err;
    return false;
  }

  // We need a NUL terminator after the log file's content so that we can call
  // atoi/atol/strtoull with a pointer to within the content.
  log->content = log->file->content_with_nul();
  log->content.remove_suffix(1);

  StringPiece header_line;
  if (GetNextPiece(&log->content, '\n', &header_line)) {
    // At least with glibc, sscanf will touch every byte of the string it scans,
    // so make a copy of the first line for sscanf to use. (Maybe sscanf is
    // calling strlen internally?)
    sscanf((header_line.AsString()).c_str(), kFileSignature,
           &log->log_version);
  }

  if (log->log_version < kOldestSupportedVersion) {
    *err = ("build log version invalid, perhaps due to being too old; "
            "starting over");
    log->content = {};
    log->file.reset();
    unlink(path.c_str());
    // Don't report this as a failure.  An empty build log will cause
    // us to rebuild the outputs anyway.
  }

  return true;
}

/// Split the build log's content (i.e. the lines excluding the header) into
/// chunks. Each chunk is guaranteed to end with a newline character. Any output
/// beyond the end of the last newline is quietly discarded.
static std::vector<StringPiece> SplitBuildLog(StringPiece content) {
  // The log file should end with an LF character, but if it doesn't, start by
  // stripping off non-LF characters.
  while (!content.empty() && content.back() != '\n') {
    content.remove_suffix(1);
  }

  size_t ideal_chunk_count = GetOptimalThreadPoolJobCount();
  size_t ideal_chunk_size = content.size() / ideal_chunk_count + 1;

  std::vector<StringPiece> result;
  StringPiece chunk;
  while (GetNextPiece(&content, '\n', &chunk, ideal_chunk_size)) {
    result.push_back(chunk);
  }
  if (!content.empty()) {
    result.push_back(content);
  }

  return result;
}

/// Call the given function on each line in the given chunk. The chunk must end
/// with an LF character. The LF characters are included in the string views
/// passed to the callback.
template <typename Func>
static void VisitEachLineInChunk(StringPiece chunk, Func func) {
  assert(!chunk.empty() && chunk.back() == '\n');
  StringPiece line;
  while (GetNextPiece(&chunk, '\n', &line)) {
    func(line);
  }
}

/// Count the number of LF newline characters in the string. The string is
/// guaranteed to end with an LF.
static size_t CountNewlinesInChunk(StringPiece chunk) {
  size_t line_count = 0;
  VisitEachLineInChunk(chunk, [&line_count](StringPiece line) {
    ++line_count;
  });
  return line_count;
}

struct ParsedLine {
  /// These fields are guaranteed to be followed by a whitespace character
  /// (either a tab or an LF), but the whitespace terminator isn't explicitly
  /// part of the string piece.
  StringPiece start_time;
  StringPiece end_time;
  StringPiece mtime;
  StringPiece path;
  StringPiece command_hash;
};

/// Given a single line of the build log (including the terminator LF), split
/// the line into its various tab-delimited fields.
static inline bool SplitLine(StringPiece line, ParsedLine* out) {
  assert(!line.empty() && line.back() == '\n');
  line.remove_suffix(1);

  auto get_next_field = [&line](StringPiece* out_piece) {
    // Extract the next piece from the line. If we're successful, remove the
    // field separator from the end of the piece.
    if (!GetNextPiece(&line, kFieldSeparator, out_piece)) return false;
    assert(!out_piece->empty() && out_piece->back() == kFieldSeparator);
    out_piece->remove_suffix(1);
    return true;
  };

  *out = {};
  if (!get_next_field(&out->start_time)) return false;
  if (!get_next_field(&out->end_time)) return false;
  if (!get_next_field(&out->mtime)) return false;
  if (!get_next_field(&out->path)) return false;
  out->command_hash = line;

  return true;
}

/// Given a single line of the build log (including the terminator LF), return
/// just the path field.
static StringPiece GetPathForLine(StringPiece line) {
  ParsedLine parsed_line;
  return SplitLine(line, &parsed_line) ? parsed_line.path : StringPiece();
}

bool BuildLog::Load(const string& path, string* err) {
  METRIC_RECORD(".ninja_log load");
  assert(entries_.empty());

  BuildLogInput log;
  if (!OpenBuildLogForReading(path, &log, err)) return false;
  if (log.file.get() == nullptr) return true;

  std::vector<StringPiece> chunks = SplitBuildLog(log.content);
  std::unique_ptr<ThreadPool> thread_pool = CreateThreadPool();

  // The concurrent hashmap doesn't automatically resize when entries are added
  // to it, so we need to reserve space in advance. The number of newlines
  // should exceed the number of distinct paths in the build log by a small
  // factor.
  size_t line_count = 0;
  for (size_t count :
      ParallelMap(thread_pool.get(), chunks, CountNewlinesInChunk)) {
    line_count += count;
  }
  entries_.reserve(line_count);

  // Construct an initial table of path -> LogEntry. Each LogEntry is
  // initialized with the newest record for a particular path. Build a list of
  // each chunk's new log entries.
  std::vector<std::vector<LogEntry*>> chunk_new_entries =
      ParallelMap(thread_pool.get(), chunks, [this, &log](StringPiece chunk) {
    std::vector<LogEntry*> chunk_entries_list;
    VisitEachLineInChunk(chunk, [&](StringPiece line) {
      HashedStrView path = GetPathForLine(line);
      if (path.empty()) return;
      LogEntry* entry = nullptr;
      if (LogEntry** entry_it = entries_.Lookup(path)) {
        entry = *entry_it;
      } else {
        std::unique_ptr<LogEntry> new_entry(new LogEntry(path));
        new_entry->newest_parsed_line = line.data() - log.content.data();
      fprintf(stderr, "build_log2\n");
        if (entries_.insert({ new_entry->output, new_entry.get() }).second) {
          chunk_entries_list.push_back(new_entry.release());
          return;
        } else {
          // Another thread beat us to it. Update the existing entry instead.
          LogEntry** entry_it = entries_.Lookup(path);
          assert(entry_it != nullptr);
          entry = *entry_it;
        }
      }
      AtomicUpdateMaximum<size_t>(&entry->newest_parsed_line,
                                  line.data() - log.content.data());
    });
    return chunk_entries_list;
  });

  // Collect all the log entries into a flat vector so we can distribute the
  // final parsing work. This list has a non-deterministic order when a node
  // appears in multiple chunks.
  std::vector<LogEntry*> entries_vec;
  entries_vec.reserve(entries_.size());
  for (auto& chunk : chunk_new_entries) {
      fprintf(stderr, "build_log3\n");
    std::move(chunk.begin(), chunk.end(), std::back_inserter(entries_vec));
  }

  // Finish parsing the log entries.
  ParallelMap(thread_pool.get(), entries_vec, [&log](LogEntry* entry) {
    // Locate the end of the line. The end of the line wasn't stored in the log
    // entry because that would complicate the atomic line location updating.
    assert(entry->newest_parsed_line < log.content.size());
    StringPiece line_to_end = log.content.substr(entry->newest_parsed_line);
    StringPiece line;
    if (!GetNextPiece(&line_to_end, '\n', &line)) {
      assert(false && "build log changed during parsing");
      abort();
    }

    ParsedLine parsed_line {};
    if (!SplitLine(line, &parsed_line)) {
      assert(false && "build log changed during parsing");
      abort();
    }

    // Initialize the entry object.
    entry->start_time = atoi(parsed_line.start_time.data());
    entry->end_time = atoi(parsed_line.end_time.data());
    entry->mtime = strtoll(parsed_line.mtime.data(), nullptr, 10);
    entry->command_hash = static_cast<uint64_t>(
        strtoull(parsed_line.command_hash.data(), nullptr, 16));
  });

  int total_entry_count = line_count;
  int unique_entry_count = entries_vec.size();

  // Decide whether it's time to rebuild the log:
  // - if we're upgrading versions
  // - if it's getting large
  const int kMinCompactionEntryCount = 100;
  const int kCompactionRatio = 3;
  if (log.log_version < kCurrentVersion) {
    needs_recompaction_ = true;
  } else if (total_entry_count > kMinCompactionEntryCount &&
             total_entry_count > unique_entry_count * kCompactionRatio) {
    needs_recompaction_ = true;
  }

  return true;
}

BuildLog::LogEntry* BuildLog::LookupByOutput(const HashedStrView& path) {
  if (LogEntry** i = entries_.Lookup(path))
    return *i;
  return nullptr;
}

bool BuildLog::WriteEntry(FILE* f, const LogEntry& entry) {
  return fprintf(f, "%d\t%d\t%" PRId64 "\t%s\t%" PRIx64 "\n",
          entry.start_time, entry.end_time, entry.mtime,
          entry.output.c_str(), entry.command_hash) > 0;
}

bool BuildLog::Recompact(const string& path, const BuildLogUser& user,
                         string* err) {
  METRIC_RECORD(".ninja_log recompact");

  Close();
  string temp_path = path + ".recompact";
  FILE* f = fopen(temp_path.c_str(), "wb");
  if (!f) {
    *err = strerror(errno);
    return false;
  }

  if (fprintf(f, kFileSignature, kCurrentVersion) < 0) {
    *err = strerror(errno);
    fclose(f);
    return false;
  }

  Entries new_entries;
  new_entries.reserve(std::max(entries_.size(), entries_.bucket_count()));

  for (const std::pair<HashedStrView, LogEntry*>& pair : entries_) {
    if (user.IsPathDead(pair.first.str_view()))
      continue;

      fprintf(stderr, "build_log4\n");
    new_entries.insert(pair);

    if (!WriteEntry(f, *pair.second)) {
      *err = strerror(errno);
      fclose(f);
      return false;
    }
  }

  entries_.swap(new_entries);

  fclose(f);
  if (unlink(path.c_str()) < 0) {
    *err = strerror(errno);
    return false;
  }

  if (rename(temp_path.c_str(), path.c_str()) < 0) {
    *err = strerror(errno);
    return false;
  }

  return true;
}
