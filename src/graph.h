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

#ifndef NINJA_GRAPH_H_
#define NINJA_GRAPH_H_

#include <atomic>
#include <set>
#include <stdio.h>
#include <string>
#include <vector>
using namespace std;

#include "eval_env.h"
#include "timestamp.h"
#include "util.h"

struct BuildLog;
struct DiskInterface;
struct DepsLog;
struct Edge;
struct Node;
struct Pool;
struct State;
struct ThreadPool;

/// A reference to a path from the lexer. This path is unevaluated, stored in
/// mmap'ed memory, and is guaranteed to be followed by a terminating character
/// (e.g. whitespace, a colon or pipe, etc).
struct LexedPath {
  StringPiece str_;
};

#ifdef _WIN32

/// On Windows, we record the '/'-vs-'\' slash direction in the first reference
/// to a path, which determines the path strings used in command-lines.
struct NodeSlashBits {
  NodeSlashBits() {}
  NodeSlashBits(uint64_t value) : slash_bits_(value) {}
  uint64_t slash_bits() const { return slash_bits_; }
private:
  uint64_t slash_bits_ = 0;
};

#else

/// By default, '\' is not recognized as a path separator, and slash_bits is
/// always 0.
struct NodeSlashBits {
  NodeSlashBits() {}
  NodeSlashBits(uint64_t /*value*/) {}
  uint64_t slash_bits() const { return 0; }
};

#endif  // _WIN32

/// Record the earliest reference to the node, which is needed for two uses:
///
///  - To verify that a node exists before a "default" declaration references
///    it.
///  - On Windows, the earliest reference to the node determines the slash_bits
///    value for decanonicalizing the node's path.
///
/// On Windows, atomic operations on this struct probably use a spin lock, but
/// it can be configured to use a DWCAS (e.g. -mcx16 with Clang/libc++). On
/// other targets, the empty base class occupies 0 bytes, and atomic operations
/// will be lock-free.
struct NodeFirstReference : NodeSlashBits {
  NodeFirstReference() {}
  NodeFirstReference(DeclIndex loc, uint64_t slash_bits)
      : NodeSlashBits(slash_bits), loc_(loc) {}
  DeclIndex dfs_location() const { return loc_; }
private:
  DeclIndex loc_ = kLastDeclIndex;
};

inline bool operator<(const NodeFirstReference& x,
                      const NodeFirstReference& y) {
  if (x.dfs_location() < y.dfs_location()) return true;
  if (x.dfs_location() > y.dfs_location()) return false;
  return x.slash_bits() < y.slash_bits();
}

/// Information about a node in the dependency graph: the file, whether
/// it's dirty, mtime, etc.
struct Node {
  Node(const HashedStrView& path, uint64_t initial_slash_bits)
      : path_(path),
        first_reference_({ kLastDeclIndex, initial_slash_bits }) {
		if (path.size() == 0) {
			fprintf(stderr, "Adding empty path\n");
		}
		//fprintf(stderr, "Add -%s-\n", path.data());
	}
  ~Node();

  /// Precompute the node's Stat() call from a worker thread with exclusive
  /// access to this node. Returns false on error.
  bool PrecomputeStat(DiskInterface* disk_interface, string* err);

  /// After the dependency scan is complete, reset the precomputed mtime so it
  /// can't affect later StatIfNecessary() calls.
  void ClearPrecomputedStat() {
    precomputed_mtime_ = -1;
  }

  /// Return false on error.
  /// Uses stat() or lstat() as appropriate.
  bool Stat(DiskInterface* disk_interface, string* err);

  /// Only use when lstat() is desired (output files)
  bool LStat(DiskInterface* disk_interface, bool* is_dir, string* err);

  /// Return false on error.
  bool StatIfNecessary(DiskInterface* disk_interface, string* err) {
    if (status_known())
      return true;
    if (precomputed_mtime_ >= 0) {
      mtime_ = precomputed_mtime_;
      return true;
    }
    return Stat(disk_interface, err);
  }

  /// Mark as not-yet-stat()ed and not dirty.
  void ResetState() {
    mtime_ = -1;
    precomputed_mtime_ = -1;
    dirty_ = false;
    precomputed_dirtiness_ = false;
  }

  /// Mark the Node as already-stat()ed and missing.
  void MarkMissing() {
    mtime_ = 0;
  }

  bool exists() const {
    return mtime_ != 0;
  }

  bool status_known() const {
    return mtime_ != -1;
  }

  const std::string& path() const { return path_.str(); }
  const HashedStr& path_hashed() const { return path_; }
  /// Get |path()| but use slash_bits to convert back to original slash styles.
  string PathDecanonicalized() const {
    return PathDecanonicalized(path_.str(), slash_bits());
  }
  static string PathDecanonicalized(const string& path,
                                    uint64_t slash_bits);

  TimeStamp mtime() const { return mtime_; }

  bool dirty() const { return dirty_; }
  void set_dirty(bool dirty) { dirty_ = dirty; }
  void MarkDirty() { dirty_ = true; }

  bool precomputed_dirtiness() const { return precomputed_dirtiness_; }
  void set_precomputed_dirtiness(bool value) { precomputed_dirtiness_ = value; }

  Edge* in_edge() const { return in_edge_; }
  void set_in_edge(Edge* edge) { in_edge_ = edge; }

  int id() const { return id_; }
  void set_id(int id) { id_ = id; }

  // Thread-safe properties.
  DeclIndex dfs_location() const {
    return first_reference_.load().dfs_location();
  }
  uint64_t slash_bits() const {
    return first_reference_.load().slash_bits();
  }
  void UpdateFirstReference(DeclIndex dfs_location, uint64_t slash_bits) {
    AtomicUpdateMinimum(&first_reference_, { dfs_location, slash_bits });
  }

  // Thread-safe properties.
  bool has_out_edge() const;
  std::vector<Edge*> GetOutEdges() const;
  void AddOutEdge(Edge* edge);

  /// Add an out-edge from the dependency scan. This function differs from
  /// AddOutEdge in several ways:
  ///  - It uses a simple vector, which is faster for single-threaded use.
  ///  - It's not thread-safe.
  ///  - It preserves edge order. Edges added with AddOutEdge come from the
  ///    manifest and are ordered by their position within the manifest
  ///    (represented with the edge ID). Dep scan edges, on the other hand,
  ///    are ordered by a DFS walk from target nodes to their dependencies.
  ///    (I'm not sure whether this order is practically important.)
  void AddOutEdgeDepScan(Edge* edge) { dep_scan_out_edges_.push_back(edge); }

  void Dump(const char* prefix="") const;

  // Used in the inputs debug tool.
  bool InputsChecked() const { return inputs_checked_; }
  void MarkInputsChecked() { inputs_checked_ = true; }

private:
  const HashedStr path_;

  /// Possible values of mtime_:
  ///   -1: file hasn't been examined
  ///   0:  we looked, and file doesn't exist
  ///   >0: actual file's mtime
  TimeStamp mtime_ = -1;

  /// If this value is >= 0, it represents a precomputed mtime for the node.
  TimeStamp precomputed_mtime_ = -1;

  /// Dirty is true when the underlying file is out-of-date.
  /// But note that Edge::outputs_ready_ is also used in judging which
  /// edges to build.
  bool dirty_ = false;

  /// Set to true once the node's stat and command-hash info have been
  /// precomputed.
  bool precomputed_dirtiness_ = false;

  /// A dense integer id for the node, assigned and used by DepsLog.
  int id_ = -1;

  std::atomic<NodeFirstReference> first_reference_;

  Edge* in_edge_ = nullptr;

  struct EdgeList {
    EdgeList(Edge* edge=nullptr, EdgeList* next=nullptr)
        : edge(edge), next(next) {}

    Edge* edge = nullptr;
    EdgeList* next = nullptr;
  };

  /// All Edges that use this Node as an input. The order of this list is
  /// non-deterministic. An accessor function sorts it each time it's used.
  std::atomic<EdgeList*> out_edges_ { nullptr };

  std::vector<Edge*> dep_scan_out_edges_;

  /// Stores if this node's inputs have been already computed. Used in the
  /// inputs debug tool.
  bool inputs_checked_ = false;
};

struct EdgeEval {
  enum EvalPhase { kParseTime, kFinalScope };
  enum EscapeKind { kShellEscape, kDoNotEscape };

  EdgeEval(Edge* edge, EvalPhase eval_phase, EscapeKind escape)
      : edge_(edge),
        eval_phase_(eval_phase),
        escape_in_out_(escape) {}

  /// Looks up the variable and appends its value to the output buffer. Returns
  /// false on error (i.e. a cycle in rule variable expansion).
  bool EvaluateVariable(std::string* out_append, const HashedStrView& var,
                        std::string* err);

  /// There are only a small number of bindings allowed on a rule. If we recurse
  /// enough times, we're guaranteed to repeat a variable.
  static constexpr int kEvalRecursionLimit = 16;

private:
  Edge* edge_ = nullptr;

  EvalPhase eval_phase_ = kFinalScope;

  /// The kind of escaping to do on $in and $out path variables.
  EscapeKind escape_in_out_ = kShellEscape;

  int recursion_count_ = 0;
  StringPiece recursion_vars_[kEvalRecursionLimit];

  void AppendPathList(std::string* out_append,
                      std::vector<Node*>::iterator begin,
                      std::vector<Node*>::iterator end,
                      char sep);
};

/// An edge in the dependency graph; links between Nodes using Rules.
struct Edge {
  enum VisitMark {
    VisitNone,
    VisitInStack,
    VisitDone
  };

  struct DepScanInfo {
    bool valid = false;
    bool restat = false;
    bool generator = false;
    bool deps = false;
    bool depfile = false;
    bool phony_output = false;
    uint64_t command_hash = 0;
  };

  /// Return true if all inputs' in-edges are ready.
  bool AllInputsReady() const;

  /// Expand all variables in a command and return it as a string.
  /// If incl_rsp_file is enabled, the string will also contain the
  /// full contents of a response file (if applicable)
  bool EvaluateCommand(std::string* out_append, bool incl_rsp_file,
                       std::string* err);

  /// Convenience method. This method must not be called from a worker thread,
  /// because it could abort with a fatal error. (For consistency with other
  /// Get*/Evaluate* methods, a better name might be GetCommand.)
  std::string EvaluateCommand(bool incl_rsp_file = false);

  /// Attempts to evaluate info needed for scanning dependencies.
  bool PrecomputeDepScanInfo(std::string* err);

  /// Returns dependency-scanning info or exits with a fatal error. These
  /// methods must not be called until after the manifest has been loaded.
  const DepScanInfo& ComputeDepScanInfo();
  uint64_t GetCommandHash()   { return ComputeDepScanInfo().command_hash; }
  bool IsRestat()             { return ComputeDepScanInfo().restat;       }
  bool IsGenerator()          { return ComputeDepScanInfo().generator;    }
  bool IsPhonyOutput()         { return ComputeDepScanInfo().phony_output;  }
  bool UsesDepsLog()          { return ComputeDepScanInfo().deps;         }
  bool UsesDepfile()          { return ComputeDepScanInfo().depfile;      }

  /// Appends the value of |key| to the output buffer. On error, returns false,
  /// and the content of the output buffer is unspecified.
  bool EvaluateVariable(std::string* out_append, const HashedStrView& key,
                        std::string* err,
                        EdgeEval::EvalPhase phase=EdgeEval::kFinalScope,
                        EdgeEval::EscapeKind escape=EdgeEval::kShellEscape);

private:
  std::string GetBindingImpl(const HashedStrView& key,
                             EdgeEval::EvalPhase phase,
                             EdgeEval::EscapeKind escape);

public:
  /// Convenience method for EvaluateVariable. On failure, it issues a fatal
  /// error. This function must not be called from a worker thread because:
  ///  - Error reporting should be deterministic, and
  ///  - Fatal() destructs static globals, which a worker thread could be using.
  std::string GetBinding(const HashedStrView& key);
  /// Like GetBinding("depfile"), but without shell escaping.
  string GetUnescapedDepfile();
  /// Like GetBinding("rspfile"), but without shell escaping.
  string GetUnescapedRspfile();

  void Dump(const char* prefix="") const;

  /// Temporary fields used only during manifest parsing.
  struct DeferredPathList {
    DeferredPathList(const char* lexer_pos=nullptr, bool is_output=false,
                     int count=0)
        : lexer_pos(lexer_pos), is_output(is_output), count(count) {}

    const char* lexer_pos = nullptr;
    bool is_output = false;
    int count = 0;
  };
  struct {
    StringPiece rule_name;
    size_t rule_name_diag_pos = 0;
    size_t final_diag_pos = 0;
    std::vector<DeferredPathList> deferred_path_lists;
  } parse_state_;

  RelativePosition pos_;
  const Rule* rule_ = nullptr;
  Pool* pool_ = nullptr;
  vector<Node*> inputs_;
  vector<Node*> outputs_;
  std::vector<std::pair<HashedStr, std::string>> unevaled_bindings_;
  VisitMark mark_ = VisitNone;
  bool precomputed_dirtiness_ = false;
  size_t id_ = 0;
  bool outputs_ready_ = false;
  bool deps_missing_ = false;
  bool phony_from_depfile_ = false;
  DepScanInfo dep_scan_info_;

  DeclIndex dfs_location() const { return pos_.dfs_location(); }
  const Rule& rule() const { return *rule_; }
  Pool* pool() const { return pool_; }
  int weight() const { return 1; }
  bool outputs_ready() const { return outputs_ready_; }

  // There are three types of inputs.
  // 1) explicit deps, which show up as $in on the command line;
  // 2) implicit deps, which the target depends on implicitly (e.g. C headers),
  //                   and changes in them cause the target to rebuild;
  // 3) order-only deps, which are needed before the target builds but which
  //                     don't cause the target to rebuild.
  // These are stored in inputs_ in that order, and we keep counts of
  // #2 and #3 when we need to access the various subsets.
  int explicit_deps_ = 0;
  int implicit_deps_ = 0;
  int order_only_deps_ = 0;
  bool is_implicit(size_t index) {
    return index >= inputs_.size() - order_only_deps_ - implicit_deps_ &&
        !is_order_only(index);
  }
  bool is_order_only(size_t index) {
    return index >= inputs_.size() - order_only_deps_;
  }

  // There are two types of outputs.
  // 1) explicit outs, which show up as $out on the command line;
  // 2) implicit outs, which the target generates but are not part of $out.
  // These are stored in outputs_ in that order, and we keep a count of
  // #2 to use when we need to access the various subsets.
  int explicit_outs_ = 0;
  int implicit_outs_ = 0;
  bool is_implicit_out(size_t index) const {
    return index >= outputs_.size() - implicit_outs_;
  }

  bool is_phony() const;
  bool use_console() const;
  bool maybe_phonycycle_diagnostic() const;

  /// Search for a binding on this edge and append its value to the output
  /// string. Does not search the enclosing scope. Use other functions to
  /// include ancestor scopes and rule bindings.
  bool EvaluateVariableSelfOnly(std::string* out_append,
                                const HashedStrView& var) const;
};

struct EdgeCmp {
  bool operator()(const Edge* a, const Edge* b) const {
    return a->id_ < b->id_;
  }
};

typedef set<Edge*, EdgeCmp> EdgeSet;

/// ImplicitDepLoader loads implicit dependencies, as referenced via the
/// "depfile" attribute in build files.
struct ImplicitDepLoader {
  ImplicitDepLoader(State* state, DepsLog* deps_log,
                    DiskInterface* disk_interface)
      : state_(state), disk_interface_(disk_interface), deps_log_(deps_log) {}

  /// Load implicit dependencies for \a edge.
  /// @return false on error (without filling \a err if info is just missing
  //                          or out of date).
  bool LoadDeps(Edge* edge, string* err);

  DepsLog* deps_log() const {
    return deps_log_;
  }

 private:
  /// Load implicit dependencies for \a edge from a depfile attribute.
  /// @return false on error (without filling \a err if info is just missing).
  bool LoadDepFile(Edge* edge, const string& path, string* err);

  /// Load implicit dependencies for \a edge from the DepsLog.
  /// @return false on error (without filling \a err if info is just missing).
  bool LoadDepsFromLog(Edge* edge, string* err);

  /// Preallocate \a count spaces in the input array on \a edge, returning
  /// an iterator pointing at the first new space.
  vector<Node*>::iterator PreallocateSpace(Edge* edge, int count);

  /// If we don't have a edge that generates this input already,
  /// create one; this makes us not abort if the input is missing,
  /// but instead will rebuild in that circumstance.
  void CreatePhonyInEdge(Node* node);

  State* state_;
  DiskInterface* disk_interface_;
  DepsLog* deps_log_;
};


/// DependencyScan manages the process of scanning the files in a graph
/// and updating the dirty/outputs_ready state of all the nodes and edges.
struct DependencyScan {
  DependencyScan(State* state, BuildLog* build_log, DepsLog* deps_log,
                 DiskInterface* disk_interface, bool missing_phony_is_err)
      : build_log_(build_log),
        disk_interface_(disk_interface),
        dep_loader_(state, deps_log, disk_interface),
        missing_phony_is_err_(missing_phony_is_err) {}

  /// Used for tests.
  bool RecomputeDirty(Node* node, std::string* err) {
    return RecomputeNodesDirty({ node }, err);
  }

  /// Update the |dirty_| state of the given node by inspecting its input edge.
  /// Examine inputs, outputs, and command lines to judge whether an edge
  /// needs to be re-run, and update outputs_ready_ and each outputs' |dirty_|
  /// state accordingly.
  /// Returns false on failure.
  bool RecomputeNodesDirty(const std::vector<Node*>& nodes, std::string* err);

  /// Recompute whether any output of the edge is dirty, if so sets |*dirty|.
  /// Returns false on failure.
  bool RecomputeOutputsDirty(Edge* edge, Node* most_recent_input,
                             bool* dirty, string* err);

  BuildLog* build_log() const {
    return build_log_;
  }
  void set_build_log(BuildLog* log) {
    build_log_ = log;
  }

  DepsLog* deps_log() const {
    return dep_loader_.deps_log();
  }

 private:
  /// Find the transitive closure of edges and nodes that the given node depends
  /// on. Each Node and Edge is guaranteed to appear at most once in an output
  /// vector. The returned lists are not guaranteed to be a superset or a subset
  /// of the nodes and edges that RecomputeNodesDirty will initialize.
  void CollectPrecomputeLists(Node* node, std::vector<Node*>* nodes,
                              std::vector<Edge*>* edges);

  bool PrecomputeNodesDirty(const std::vector<Node*>& nodes,
                            const std::vector<Edge*>& edges,
                            ThreadPool* thread_pool, std::string* err);

  bool RecomputeNodeDirty(Node* node, vector<Node*>* stack, string* err);
  bool VerifyDAG(Node* node, vector<Node*>* stack, string* err);

  /// Recompute whether a given single output should be marked dirty.
  /// Returns true if so.
  bool RecomputeOutputDirty(Edge* edge, Node* most_recent_input,
                            uint64_t command_hash, Node* output);

  BuildLog* build_log_;
  DiskInterface* disk_interface_;
  ImplicitDepLoader dep_loader_;

  bool missing_phony_is_err_;
};

#endif  // NINJA_GRAPH_H_
