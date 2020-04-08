// Copyright 2012 Google Inc. All Rights Reserved.
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

#include "deps_log.h"

#include <assert.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h>
#elif defined(_MSC_VER) && (_MSC_VER < 1900)
typedef __int32 int32_t;
typedef unsigned __int32 uint32_t;
#endif

#include <numeric>

#include "disk_interface.h"
#include "graph.h"
#include "metrics.h"
#include "parallel_map.h"
#include "state.h"
#include "util.h"

// The version is stored as 4 bytes after the signature and also serves as a
// byte order mark. Signature and version combined are 16 bytes long.
static constexpr StringPiece kFileSignature { "# ninjadeps\n", 12 };
static_assert(kFileSignature.size() % 4 == 0,
              "file signature size is not a multiple of 4");
static constexpr size_t kFileHeaderSize = kFileSignature.size() + 4;
const int kCurrentVersion = 4;

// Record size is currently limited to less than the full 32 bit, due to
// internal buffers having to have this size.
const unsigned kMaxRecordSize = (1 << 19) - 1;

DepsLog::~DepsLog() {
  Close();
}

bool DepsLog::OpenForWrite(const string& path, const DiskInterface& disk, string* err) {
  if (needs_recompaction_) {
    if (!Recompact(path, disk, err))
      return false;
  }
  
  file_ = fopen(path.c_str(), "ab");
  if (!file_) {
    *err = strerror(errno);
    return false;
  }
  // Set the buffer size to this and flush the file buffer after every record
  // to make sure records aren't written partially.
  setvbuf(file_, NULL, _IOFBF, kMaxRecordSize + 1);
  SetCloseOnExec(fileno(file_));

  // Opening a file in append mode doesn't set the file pointer to the file's
  // end on Windows. Do that explicitly.
  fseek(file_, 0, SEEK_END);

  if (ftell(file_) == 0) {
    if (fwrite(kFileSignature.data(), kFileSignature.size(), 1, file_) < 1) {
      *err = strerror(errno);
      return false;
    }
    if (fwrite(&kCurrentVersion, 4, 1, file_) < 1) {
      *err = strerror(errno);
      return false;
    }
  }
  if (fflush(file_) != 0) {
    *err = strerror(errno);
    return false;
  }
  return true;
}

bool DepsLog::RecordDeps(Node* node, TimeStamp mtime,
                         const vector<Node*>& nodes) {
  return RecordDeps(node, mtime, nodes.size(),
                    nodes.empty() ? NULL : (Node**)&nodes.front());
}

bool DepsLog::RecordDeps(Node* node, TimeStamp mtime,
                         int node_count, Node** nodes) {
  // Track whether there's any new data to be recorded.
  bool made_change = false;

  // Assign ids to all nodes that are missing one.
  if (node->id() < 0) {
    if (!RecordId(node))
      return false;
    made_change = true;
  }
  for (int i = 0; i < node_count; ++i) {
    if (nodes[i]->id() < 0) {
      if (!RecordId(nodes[i]))
        return false;
      made_change = true;
    }
  }

  // See if the new data is different than the existing data, if any.
  if (!made_change) {
    Deps* deps = GetDeps(node);
    if (!deps ||
        deps->mtime != mtime ||
        deps->node_count != node_count) {
      made_change = true;
    } else {
      for (int i = 0; i < node_count; ++i) {
        if (deps->nodes[i] != nodes[i]) {
          made_change = true;
          break;
        }
      }
    }
  }

  // Don't write anything if there's no new info.
  if (!made_change)
    return true;

  // Update on-disk representation.
  unsigned size = 4 * (1 + 2 + node_count);
  if (size > kMaxRecordSize) {
    errno = ERANGE;
    return false;
  }
  size |= 0x80000000;  // Deps record: set high bit.
  if (fwrite(&size, 4, 1, file_) < 1)
    return false;
  int id = node->id();
  if (fwrite(&id, 4, 1, file_) < 1)
    return false;
  uint32_t mtime_part = static_cast<uint32_t>(mtime & 0xffffffff);
  if (fwrite(&mtime_part, 4, 1, file_) < 1)
    return false;
  mtime_part = static_cast<uint32_t>((mtime >> 32) & 0xffffffff);
  if (fwrite(&mtime_part, 4, 1, file_) < 1)
    return false;
  for (int i = 0; i < node_count; ++i) {
    id = nodes[i]->id();
    if (fwrite(&id, 4, 1, file_) < 1)
      return false;
  }
  if (fflush(file_) != 0)
    return false;

  // Update in-memory representation.
  Deps* deps = new Deps(mtime, node_count);
  for (int i = 0; i < node_count; ++i)
    deps->nodes[i] = nodes[i];
  UpdateDeps(node->id(), deps);

  return true;
}

void DepsLog::Close() {
  if (file_)
    fclose(file_);
  file_ = NULL;
}

struct DepsLogWordSpan {
  size_t begin; // starting index into a deps log buffer of uint32 words
  size_t end; // stopping index
};

/// Split the v4 deps log into independently-parseable chunks using a heuristic.
/// If the heuristic fails, we'll still load the file correctly, but it could be
/// slower.
///
/// There are two kinds of records -- path and deps records. Their formats:
///
///    path:
///     - uint32 size -- high bit is clear
///     - String content. The string is padded to a multiple of 4 bytes with
///       trailing NULs.
///     - uint32 checksum (ones complement of the path's index / node ID)
///
///    deps:
///     - uint32 size -- high bit is set
///     - int32 output_path_id
///     - uint32 output_path_mtime_lo
///     - uint32 output_path_mtime_hi
///     - int32 input_path_id[...] -- every remaining word is an input ID
///
/// To split the deps log into chunks, look for uint32 words with the value
/// 0x8000xxxx, where xxxx is nonzero. Such a word is almost guaranteed to be
/// the size field of a deps record (with fewer than ~16K dependencies):
///  - It can't be part of a string, because paths can't have embedded NULs.
///  - It (probably) can't be a node ID, because node IDs are represented using
///    "int", and it would be unlikely to have more than 2 billion of them. An
///    Android build typically has about 1 million nodes.
///  - It's unlikely to be part of a path checksum, because that would also
///    imply that we have at least 2 billion nodes.
///  - It could be the upper word of a mtime from 2262, or the lower word, which
///    wraps every ~4s. We rule these out by looking for the mtime's deps size
///    two or three words above the split candidate.
///
/// This heuristic can fail in a few ways:
///  - We only find path records in the area we scan.
///  - The deps records all have >16K of dependencies. (Almost all deps records
///    I've seen in the Android build have a few hundred. Only a few have ~10K.)
///  - All deps records with <16K of dependencies are preceded by something that
///    the mtime check rejects:
///     - A deps record with an mtime within a 524us span every 4s, with zero or
///       one dependency.
///     - A deps record with an mtime from 2262, with one or two dependencies.
///     - A path record containing "xx xx 0[0-7] 80" (little-endian) or
///       "80 0[0-7] xx xx" (big-endian) as the last or second-to-last word. The
///       "0[0-7]" is a control character, and "0[0-7] 80" is invalid UTF-8.
///
/// Maybe we can add a delimiter to the log format and replace this heuristic.
size_t MustBeDepsRecordHeader(DepsLogData log, size_t index) {
  assert(index < log.size);

  size_t this_header = log.words[index];
  if ((this_header & 0xffff0000) != 0x80000000) return false;
  if ((this_header & 0x0000ffff) == 0) return false;

  // We've either found a deps record or an mtime. If it's an mtime, the
  // word two or three spaces back will be a valid deps size (0x800xxxxx).
  auto might_be_deps_record_header = [](size_t header) {
    return (header & 0x80000000) == 0x80000000 &&
           (header & 0x7fffffff) <= kMaxRecordSize;
  };
  if (index >= 3 && might_be_deps_record_header(log.words[index - 3])) {
    return false;
  }
  if (index >= 2 && might_be_deps_record_header(log.words[index - 2])) {
    return false;
  }

  // Success: As long as the deps log is valid up to this point, this index
  // must start a deps record.
  return true;
}

static std::vector<DepsLogWordSpan>
SplitDepsLog(DepsLogData log, ThreadPool* thread_pool) {
  if (log.size == 0) return {};

  std::vector<std::pair<size_t, size_t>> blind_splits =
      SplitByThreads(log.size);
  std::vector<DepsLogWordSpan> chunks;
  size_t chunk_start = 0;

  auto split_candidates = ParallelMap(thread_pool, blind_splits,
      [log](std::pair<size_t, size_t> chunk) {
    for (size_t i = chunk.first; i < chunk.second; ++i) {
      if (MustBeDepsRecordHeader(log, i)) {
        return i;
      }
    }
    return SIZE_MAX;
  });
  for (size_t candidate : split_candidates) {
    assert(chunk_start <= candidate);
    if (candidate != SIZE_MAX && chunk_start < candidate) {
      chunks.push_back({ chunk_start, candidate });
      chunk_start = candidate;
    }
  }

  assert(chunk_start < log.size);
  chunks.push_back({ chunk_start, log.size });
  return chunks;
}

struct InputRecord {
  enum Kind { InvalidHeader, PathRecord, DepsRecord } kind = InvalidHeader;
  size_t size = 0; // number of 32-bit words, including the header

  union {
    struct {
      StringPiece path;
      int checksum;
    } path;

    struct {
      int output_id;
      TimeStamp mtime;
      const uint32_t* deps;
      size_t deps_count;
    } deps;
  } u = {};
};

/// Parse a deps log record at the given index within the loaded deps log.
/// If there is anything wrong with the header (e.g. the record extends past the
/// end of the file), this function returns a blank InvalidHeader record.
static InputRecord ParseRecord(DepsLogData log, size_t index) {
  InputRecord result {};
  assert(index < log.size);

  // The header of a record is the size of the record, in bytes, without
  // including the header itself. The high bit is set for a deps record and
  // clear for a path record.
  const uint32_t header = log.words[index];
  const uint32_t raw_size = header & 0x7fffffff;
  if (raw_size % sizeof(uint32_t) != 0) return result;
  if (raw_size > kMaxRecordSize) return result;

  // Add the header to the size for easier iteration over records later on.
  const size_t size = raw_size / sizeof(uint32_t) + 1;
  if (log.size - index < size) return result;

  if ((header & 0x80000000) == 0) {
    // Path record (header, content, checksum).
    if (size < 3) return result;

    const char* path = reinterpret_cast<const char*>(&log.words[index + 1]);
    size_t path_size = (size - 2) * sizeof(uint32_t);
    if (path[path_size - 1] == '\0') --path_size;
    if (path[path_size - 1] == '\0') --path_size;
    if (path[path_size - 1] == '\0') --path_size;

    result.kind = InputRecord::PathRecord;
    result.size = size;
    result.u.path.path = StringPiece(path, path_size);
    result.u.path.checksum = log.words[index + size - 1];
    return result;
  } else {
    // Deps record (header, output_id, mtime_lo, mtime_hi).
    if (size < 4) return result;

    result.kind = InputRecord::DepsRecord;
    result.size = size;
    result.u.deps.output_id = log.words[index + 1];
    result.u.deps.mtime =
      (TimeStamp)(((uint64_t)(unsigned int)log.words[index + 3] << 32) |
                  (uint64_t)(unsigned int)log.words[index + 2]);
    result.u.deps.deps = &log.words[index + 4];
    result.u.deps.deps_count = size - 4;
    return result;
  }
}

struct DepsLogInputFile {
  std::unique_ptr<LoadedFile> file;
  DepsLogData data;
};

static bool OpenDepsLogForReading(const std::string& path,
                                  DepsLogInputFile* log,
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

  bool valid_header = false;
  int version = 0;
  if (log->file->content().size() >= kFileHeaderSize ||
      log->file->content().substr(0, kFileSignature.size()) == kFileSignature) {
    valid_header = true;
    memcpy(&version,
           log->file->content().data() + kFileSignature.size(),
           sizeof(version));
  }

  // Note: For version differences, this should migrate to the new format.
  // But the v1 format could sometimes (rarely) end up with invalid data, so
  // don't migrate v1 to v3 to force a rebuild. (v2 only existed for a few days,
  // and there was no release with it, so pretend that it never happened.)
  if (!valid_header || version != kCurrentVersion) {
    if (version == 1)
      *err = "deps log version change; rebuilding";
    else
      *err = "bad deps log signature or version; starting over";
    log->file.reset();
    unlink(path.c_str());
    // Don't report this as a failure.  An empty deps log will cause
    // us to rebuild the outputs anyway.
    return true;
  }

  log->data.words =
          reinterpret_cast<const uint32_t*>(
          log->file->content().data() + kFileHeaderSize);
  log->data.size =
      (log->file->content().size() - kFileHeaderSize) / sizeof(uint32_t);

  return true;
}

template <typename Func>
static bool ForEachRecord(DepsLogData log, DepsLogWordSpan chunk,
                          Func&& callback) {
  InputRecord record {};
  for (size_t index = chunk.begin; index < chunk.end; index += record.size) {
    record = ParseRecord(log, index);
    if (!callback(static_cast<const InputRecord&>(record))) return false;
    if (record.kind == InputRecord::InvalidHeader) break;
  }
  return true;
}

struct DepsLogNodeSpan {
  int begin; // starting node ID of span
  int end; // stopping node ID of span
};

/// Determine the range of node IDs for each chunk of the deps log. The range
/// for a chunk will only be used if the preceding chunks are valid.
static std::vector<DepsLogNodeSpan>
FindInitialNodeSpans(DepsLogData log,
                     const std::vector<DepsLogWordSpan>& chunk_words,
                     ThreadPool* thread_pool) {
  // First count the number of path records (nodes) in each chunk.
  std::vector<int> chunk_node_counts = ParallelMap(thread_pool, chunk_words,
      [log](DepsLogWordSpan chunk) {
    int node_count = 0;
    ForEachRecord(log, chunk, [&node_count](const InputRecord& record) {
      if (record.kind == InputRecord::PathRecord) {
        ++node_count;
      }
      return true;
    });
    return node_count;
  });

  // Compute an initial [begin, end) node ID range for each chunk.
  std::vector<DepsLogNodeSpan> result;
  int next_node_id = 0;
  for (int num_nodes : chunk_node_counts) {
    result.push_back({ next_node_id, next_node_id + num_nodes });
    next_node_id += num_nodes;
  }
  return result;
}

static bool IsValidRecord(const InputRecord& record, int next_node_id) {
  auto is_valid_id = [next_node_id](int id) {
    return id >= 0 && id < next_node_id;
  };

  switch (record.kind) {
  case InputRecord::InvalidHeader:
    return false;
  case InputRecord::PathRecord:
    // Validate the path's checksum.
    if (record.u.path.checksum != ~next_node_id) {
      return false;
    }
    break;
  case InputRecord::DepsRecord:
    // Verify that input/output node IDs are valid.
    if (!is_valid_id(record.u.deps.output_id)) {
      return false;
    }
    for (size_t i = 0; i < record.u.deps.deps_count; ++i) {
      if (!is_valid_id(record.u.deps.deps[i])) {
        return false;
      }
    }
    break;
  }
  return true;
}

/// Validate the deps log. If there is an invalid record, the function truncates
/// the word+node span vectors just before the invalid record.
static void ValidateDepsLog(DepsLogData log,
                            std::vector<DepsLogWordSpan>* chunk_words,
                            std::vector<DepsLogNodeSpan>* chunk_nodes,
                            ThreadPool* thread_pool) {
  std::atomic<size_t> num_valid_chunks { chunk_words->size() };

  ParallelMap(thread_pool, IntegralRange<size_t>(0, num_valid_chunks),
      [&](size_t chunk_index) {

    size_t next_word_idx = (*chunk_words)[chunk_index].begin;
    int next_node_id = (*chunk_nodes)[chunk_index].begin;

    bool success = ForEachRecord(log, (*chunk_words)[chunk_index],
        [&](const InputRecord& record) {
      if (!IsValidRecord(record, next_node_id)) {
        return false;
      }
      next_word_idx += record.size;
      if (record.kind == InputRecord::PathRecord) {
        ++next_node_id;
      }
      return true;
    });

    if (success) {
      assert(next_word_idx == (*chunk_words)[chunk_index].end);
      assert(next_node_id == (*chunk_nodes)[chunk_index].end);
    } else {
      (*chunk_words)[chunk_index].end = next_word_idx;
      (*chunk_nodes)[chunk_index].end = next_node_id;
      AtomicUpdateMinimum(&num_valid_chunks, chunk_index + 1);
    }
  });

  chunk_words->resize(num_valid_chunks);
  chunk_nodes->resize(num_valid_chunks);
}

bool DepsLog::Load(const string& path, State* state, string* err) {
  METRIC_RECORD(".ninja_deps load");

  assert(nodes_.empty());
  DepsLogInputFile log_file;

  if (!OpenDepsLogForReading(path, &log_file, err)) return false;
  if (log_file.file.get() == nullptr) return true;

  DepsLogData log = log_file.data;

  std::unique_ptr<ThreadPool> thread_pool = CreateThreadPool();

  std::vector<DepsLogWordSpan> chunk_words = SplitDepsLog(log, thread_pool.get());
  std::vector<DepsLogNodeSpan> chunk_nodes =
      FindInitialNodeSpans(log, chunk_words, thread_pool.get());

  // Validate the log and truncate the vectors after an invalid record.
  ValidateDepsLog(log, &chunk_words, &chunk_nodes, thread_pool.get());
  assert(chunk_words.size() == chunk_nodes.size());

  const size_t chunk_count = chunk_words.size();
  const int node_count = chunk_nodes.empty() ? 0 : chunk_nodes.back().end;

  // The state path hash table doesn't automatically resize, so make sure that
  // it has at least one bucket for each node in this deps log.
  state->paths_.reserve(node_count);

  nodes_.resize(node_count);

  // A map from a node ID to the final file index of the deps record outputting
  // the given node ID.
  std::vector<std::atomic<ssize_t>> dep_index(node_count);
  for (auto& index : dep_index) {
    // Write a value of -1 to indicate that no deps record outputs this ID. We
    // don't need these stores to be synchronized with other threads, so use
    // relaxed stores, which are much faster.
    index.store(-1, std::memory_order_relaxed);
  }

  // Add the nodes into the build graph, find the last deps record
  // outputting each node, and count the total number of deps records.
  const std::vector<size_t> dep_record_counts = ParallelMap(thread_pool.get(),
      IntegralRange<size_t>(0, chunk_count),
      [log, state, chunk_words, chunk_nodes, &dep_index,
        this](size_t chunk_index) {
    size_t next_word_idx = chunk_words[chunk_index].begin;
    int next_node_id = chunk_nodes[chunk_index].begin;
    int stop_node_id = chunk_nodes[chunk_index].end;
    (void)stop_node_id; // suppress unused variable compiler warning
    size_t dep_record_count = 0;

    ForEachRecord(log, chunk_words[chunk_index],
        [&, this](const InputRecord& record) {
      assert(record.kind != InputRecord::InvalidHeader);
      if (record.kind == InputRecord::PathRecord) {
        int node_id = next_node_id++;
        assert(node_id < stop_node_id);
        assert(record.u.path.checksum == ~node_id);

        // It is not necessary to pass in a correct slash_bits here. It will
        // either be a Node that's in the manifest (in which case it will
        // already have a correct slash_bits that GetNode will look up), or it
        // is an implicit dependency from a .d which does not affect the build
        // command (and so need not have its slashes maintained).
	fprintf(stderr, "deps_log 10\n");
        Node* node = state->GetNode(record.u.path.path, 0);
        assert(node->id() < 0);
        node->set_id(node_id);
        nodes_[node_id] = node;
      } else if (record.kind == InputRecord::DepsRecord) {
        const int output_id = record.u.deps.output_id;
        assert(static_cast<size_t>(output_id) < dep_index.size());
        AtomicUpdateMaximum(&dep_index[output_id],
                            static_cast<ssize_t>(next_word_idx));
        ++dep_record_count;
      }
      next_word_idx += record.size;
      return true;
    });
    assert(next_node_id == stop_node_id);
    return dep_record_count;
  });

  // Count the number of total and unique deps records.
  const size_t total_dep_record_count =
      std::accumulate(dep_record_counts.begin(), dep_record_counts.end(),
                      static_cast<size_t>(0));
  size_t unique_dep_record_count = 0;
  for (auto& index : dep_index) {
    if (index.load(std::memory_order_relaxed) != -1) {
      ++unique_dep_record_count;
    }
  }

  // Add the deps records.
  deps_.resize(node_count);
  ParallelMap(thread_pool.get(), IntegralRange<int>(0, node_count),
      [this, log, &dep_index](int node_id) {
    ssize_t index = dep_index[node_id];
    if (index == -1) return;

    InputRecord record = ParseRecord(log, index);
    assert(record.kind == InputRecord::DepsRecord);
    assert(record.u.deps.output_id == node_id);

    Deps* deps = new Deps(record.u.deps.mtime, record.u.deps.deps_count);
    for (size_t i = 0; i < record.u.deps.deps_count; ++i) {
      const int input_id = record.u.deps.deps[i];
      assert(static_cast<size_t>(input_id) < nodes_.size());
      Node* node = nodes_[input_id];
      assert(node != nullptr);
      deps->nodes[i] = node;
    }
    deps_[node_id] = deps;
  });

  const size_t actual_file_size = log_file.file->content().size();
  const size_t parsed_file_size = kFileHeaderSize +
      (chunk_words.empty() ? 0 : chunk_words.back().end) * sizeof(uint32_t);
  assert(parsed_file_size <= actual_file_size);
  if (parsed_file_size < actual_file_size) {
    // An error occurred while loading; try to recover by truncating the file to
    // the last fully-read record.
    *err = "premature end of file";
    log_file.file.reset();

    if (!Truncate(path, parsed_file_size, err))
      return false;

    // The truncate succeeded; we'll just report the load error as a
    // warning because the build can proceed.
    *err += "; recovering";
    return true;
  }

  // Rebuild the log if there are too many dead records.
  const unsigned kMinCompactionEntryCount = 1000;
  const unsigned kCompactionRatio = 3;
  if (total_dep_record_count > kMinCompactionEntryCount &&
      total_dep_record_count > unique_dep_record_count * kCompactionRatio) {
    needs_recompaction_ = true;
  }

  return true;
}

DepsLog::Deps* DepsLog::GetDeps(Node* node) {
  // Abort if the node has no id (never referenced in the deps) or if
  // there's no deps recorded for the node.
  if (node->id() < 0 || node->id() >= (int)deps_.size())
    return NULL;
  return deps_[node->id()];
}

bool DepsLog::Recompact(const string& path, const DiskInterface& disk, string* err) {
  METRIC_RECORD(".ninja_deps recompact");

  Close();
  string temp_path = path + ".recompact";

  // OpenForWrite() opens for append.  Make sure it's not appending to a
  // left-over file from a previous recompaction attempt that crashed somehow.
  unlink(temp_path.c_str());

  DepsLog new_log;
  if (!new_log.OpenForWrite(temp_path, disk, err))
    return false;

  // Clear all known ids so that new ones can be reassigned.  The new indices
  // will refer to the ordering in new_log, not in the current log.
  for (vector<Node*>::iterator i = nodes_.begin(); i != nodes_.end(); ++i)
    (*i)->set_id(-1);
  
  // Write out all deps again.
  for (int old_id = 0; old_id < (int)deps_.size(); ++old_id) {
    Deps* deps = deps_[old_id];
    if (!deps) continue;  // If nodes_[old_id] is a leaf, it has no deps.

    Node* node = nodes_[old_id];
    if (node->in_edge()) {
      // If the current manifest defines this edge, skip if it's not dep
      // producing.
      if (node->in_edge()->GetBinding("deps").empty()) continue;
    } else {
      // If the current manifest does not define this edge, skip if it's missing
      // from the disk.
      string err;
      TimeStamp mtime = disk.LStat(node->path(), nullptr, &err);
      if (mtime == -1)
        Error("%s", err.c_str()); // log and ignore LStat() errors
      if (mtime == 0)
        continue;
    }

    if (!new_log.RecordDeps(nodes_[old_id], deps->mtime,
                            deps->node_count, deps->nodes)) {
      new_log.Close();
      return false;
    }
  }

  new_log.Close();

  // All nodes now have ids that refer to new_log, so steal its data.
  deps_.swap(new_log.deps_);
  nodes_.swap(new_log.nodes_);

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

bool DepsLog::IsDepsEntryLiveFor(Node* node) {
  // Skip entries that don't have in-edges or whose edges don't have a
  // "deps" attribute. They were in the deps log from previous builds, but the
  // files they were for were removed from the build.
  return node->in_edge() && !node->in_edge()->GetBinding("deps").empty();
}

bool DepsLog::UpdateDeps(int out_id, Deps* deps) {
  if (out_id >= (int)deps_.size())
    deps_.resize(out_id + 1);

  bool delete_old = deps_[out_id] != NULL;
  if (delete_old)
    delete deps_[out_id];
  deps_[out_id] = deps;
  return delete_old;
}

bool DepsLog::RecordId(Node* node) {
  int path_size = node->path().size();
  int padding = (4 - path_size % 4) % 4;  // Pad path to 4 byte boundary.

  unsigned size = path_size + padding + 4;
  if (size > kMaxRecordSize) {
    errno = ERANGE;
    return false;
  }
  if (fwrite(&size, 4, 1, file_) < 1)
    return false;
  if (fwrite(node->path().data(), path_size, 1, file_) < 1) {
    fprintf(stderr, "Node path: -%s-\n", node->path().c_str());
    assert(node->path().size() > 0);
    return false;
  }
  if (padding && fwrite("\0\0", padding, 1, file_) < 1)
    return false;
  int id = nodes_.size();
  unsigned checksum = ~(unsigned)id;
  if (fwrite(&checksum, 4, 1, file_) < 1)
    return false;
  if (fflush(file_) != 0)
    return false;

  node->set_id(id);
  nodes_.push_back(node);

  return true;
}
