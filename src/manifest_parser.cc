// Copyright 2018 Google Inc. All Rights Reserved.
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

#include "manifest_parser.h"

#include <assert.h>

#include <stdio.h>
#include <unordered_map>
#include <vector>

#include "disk_interface.h"
#include "eval_env.h"
#include "lexer.h"
#include "metrics.h"
#include "parallel_map.h"
#include "state.h"
#include "util.h"
#include "version.h"
#include "manifest_chunk_parser.h"

using manifest_chunk::Clump;
using manifest_chunk::DefaultTarget;
using manifest_chunk::Include;
using manifest_chunk::ParserItem;
using manifest_chunk::RequiredVersion;

namespace {

static bool DecorateError(const LoadedFile& file, size_t file_offset,
                          const std::string& message, std::string* err) {
  assert(file_offset <= file.content().size());
  return DecorateErrorWithLocation(file.filename(), file.content().data(),
                                   file_offset, message, err);
}

/// Split a single manifest file into chunks, parse the chunks in parallel, and
/// return the resulting parser output.
static std::vector<ParserItem> ParseManifestChunks(const LoadedFile& file,
                                                   ThreadPool* thread_pool) {
  std::vector<ParserItem> result;
  const std::vector<StringPiece> chunk_views =
    manifest_chunk::SplitManifestIntoChunks(file.content_with_nul());

  METRIC_RECORD(".ninja load : parse chunks");

  for (std::vector<ParserItem>& chunk_items :
      ParallelMap(thread_pool, chunk_views, [&file](StringPiece view) {
    std::vector<ParserItem> chunk_items;
    manifest_chunk::ParseChunk(file, view, &chunk_items);
    return chunk_items;
  })) {
    std::move(chunk_items.begin(), chunk_items.end(),
              std::back_inserter(result));
  }
  return result;
}

static void ReserveSpaceInScopeTables(Scope* scope,
                                      const std::vector<ParserItem>& items) {
  // Reserve extra space in the scope's bindings/rules hash tables.
  METRIC_RECORD(".ninja load : alloc scope tables");
  size_t new_bindings = 0;
  size_t new_rules = 0;
  for (const auto& item : items) {
    if (item.kind == ParserItem::kClump) {
      Clump* clump = item.u.clump;
      new_bindings += clump->bindings_.size();
      new_rules += clump->rules_.size();
    }
  }
  scope->ReserveTableSpace(new_bindings, new_rules);
}

struct ManifestFileSet {
  ManifestFileSet(FileReader* file_reader) : file_reader_(file_reader) {}

  bool LoadFile(const std::string& filename, const LoadedFile** result,
                std::string* err);

  ~ManifestFileSet() {
    // It can take a surprising long time to unmap all the manifest files
    // (e.g. ~70 ms for Android, 25 ms for Chromium).
    METRIC_RECORD(".ninja load : unload files");
    loaded_files_.clear();
  }

private:
  FileReader *file_reader_ = nullptr;

  /// Keep all the manifests in memory until all the manifest parsing work is
  /// complete, because intermediate parsing results (i.e. StringPiece) may
  /// point into the loaded files.
  std::vector<std::unique_ptr<LoadedFile>> loaded_files_;
};

bool ManifestFileSet::LoadFile(const std::string& filename,
                               const LoadedFile** result,
                               std::string* err) {
  std::unique_ptr<LoadedFile> loaded_file;
  if (file_reader_->LoadFile(filename, &loaded_file, err) != FileReader::Okay) {
    *err = "loading '" + filename + "': " + *err;
    return false;
  }
  loaded_files_.push_back(std::move(loaded_file));
  *result = loaded_files_.back().get();
  return true;
}

struct DfsParser {
  DfsParser(ManifestFileSet* file_set, State* state, ThreadPool* thread_pool)
      : file_set_(file_set), state_(state), thread_pool_(thread_pool) {}

private:
  void HandleRequiredVersion(const RequiredVersion& item, Scope* scope);
  bool HandleInclude(const Include& item, const LoadedFile& file, Scope* scope,
                     const LoadedFile** child_file, Scope** child_scope,
                     std::string* err);
  bool HandlePool(Pool* pool, const LoadedFile& file, std::string* err);
  bool HandleClump(Clump* clump, const LoadedFile& file, Scope* scope,
                   std::string* err);

public:
  /// Load the tree of manifest files and do initial parsing of all the chunks.
  /// This function always runs on the main thread.
  bool LoadManifestTree(const LoadedFile& file, Scope* scope,
                        std::vector<Clump*>* out_clumps, std::string* err);

private:
  ManifestFileSet* file_set_;
  State* state_;
  ThreadPool* thread_pool_;
};

void DfsParser::HandleRequiredVersion(const RequiredVersion& item,
                                      Scope* scope) {
  std::string version;
  EvaluateBindingInScope(&version, item.version_,
                         scope->GetCurrentEndOfScope());
  CheckNinjaVersion(version);
}

bool DfsParser::HandleInclude(const Include& include, const LoadedFile& file,
                              Scope* scope, const LoadedFile** child_file,
                              Scope** child_scope, std::string* err) {
  std::string path;
  EvaluatePathInScope(&path, include.path_, scope->GetCurrentEndOfScope());
  if (!file_set_->LoadFile(path, child_file, err)) {
    return DecorateError(file, include.diag_pos_, std::string(*err), err);
  }
  if (include.new_scope_) {
    *child_scope = new Scope(scope->GetCurrentEndOfScope());
  } else {
    *child_scope = scope;
  }
  return true;
}

bool DfsParser::HandlePool(Pool* pool, const LoadedFile& file,
                           std::string* err) {
  std::string depth_string;
  EvaluateBindingInScope(&depth_string, pool->parse_state_.depth,
                         pool->pos_.scope_pos());
  pool->depth_ = atol(depth_string.c_str());
  if (pool->depth_ < 0) {
    return DecorateError(file, pool->parse_state_.depth_diag_pos,
                         "invalid pool depth", err);
  }
  if (!state_->AddPool(pool)) {
    return DecorateError(file, pool->parse_state_.pool_name_diag_pos,
                         "duplicate pool '" + pool->name() + "'", err);
  }
  return true;
}

bool DfsParser::HandleClump(Clump* clump, const LoadedFile& file, Scope* scope,
                            std::string* err) {
  METRIC_RECORD(".ninja load : scope setup");
  // Allocate DFS and scope positions for the clump.
  clump->pos_.scope = scope->AllocDecls(clump->decl_count());
  clump->pos_.dfs_location = state_->AllocDfsLocation(clump->decl_count());
  {
    METRIC_RECORD(".ninja load : scope setup : bindings");
    for (Binding* binding : clump->bindings_) {
      scope->AddBinding(binding);
    }
  }
  {
    METRIC_RECORD(".ninja load : scope setup : rules");
    for (Rule* rule : clump->rules_) {
      if (!scope->AddRule(rule)) {
        return DecorateError(file, rule->parse_state_.rule_name_diag_pos,
                             "duplicate rule '" + rule->name() + "'", err);
      }
    }
  }
  for (Pool* pool : clump->pools_) {
    if (!HandlePool(pool, file, err)) {
      return false;
    }
  }
  return true;
}

bool DfsParser::LoadManifestTree(const LoadedFile& file, Scope* scope,
                                 std::vector<Clump*>* out_clumps,
                                 std::string* err) {
  std::vector<ParserItem> items = ParseManifestChunks(file, thread_pool_);
  ReserveSpaceInScopeTables(scope, items);

  // With the chunks parsed, do a depth-first parse of the ninja manifest using
  // the results of the parallel parse.
  for (const auto& item : items) {
    switch (item.kind) {
    case ParserItem::kError:
      *err = item.u.error->msg_;
      return false;

    case ParserItem::kRequiredVersion:
      HandleRequiredVersion(*item.u.required_version, scope);
      break;

    case ParserItem::kInclude: {
      const Include& include = *item.u.include;
      const LoadedFile* child_file = nullptr;
      Scope* child_scope = nullptr;
      if (!HandleInclude(include, file, scope, &child_file, &child_scope, err))
        return false;
      if (!LoadManifestTree(*child_file, child_scope, out_clumps, err))
        return false;
      break;
    }

    case ParserItem::kClump:
      if (!HandleClump(item.u.clump, file, scope, err))
        return false;
      out_clumps->push_back(item.u.clump);
      break;

    default:
      assert(false && "unrecognized kind of ParserItem");
      abort();
    }
  }

  return true;
}

/// Parse an edge's path and add it to a vector on the Edge object.
static inline bool AddPathToEdge(State* state, const Edge& edge,
                                 std::vector<Node*>* out_vec,
                                 const LoadedFile& file, Lexer& lexer,
                                 std::string* err) {
  HashedStrView key;
  uint64_t slash_bits = 0;

  StringPiece canon_path = lexer.PeekCanonicalPath();
  if (!canon_path.empty()) {
    key = canon_path;
  } else {
    LexedPath path;
    if (!lexer.ReadPath(&path, err) || path.str_.empty()) {
      assert(false && "manifest file apparently changed during parsing");
      abort();
    }

    thread_local std::string tls_work_buf;
    std::string& work_buf = tls_work_buf;
    work_buf.clear();
    EvaluatePathOnEdge(&work_buf, path, edge);

    std::string path_err;
    if (!CanonicalizePath(&work_buf, &slash_bits, &path_err)) {
      return DecorateError(file, edge.parse_state_.final_diag_pos, path_err,
                           err);
    }
    key = work_buf;
  }

  fprintf(stderr, "manifest_parser\n");
  Node* node = state->GetNode(key, 0);
  node->UpdateFirstReference(edge.dfs_location(), slash_bits);
  out_vec->push_back(node);
  return true;
}

static const HashedStrView kPool { "pool" };
static const HashedStrView kDeps { "deps" };

struct ManifestLoader {
private:
  State* const state_ = nullptr;
  ThreadPool* const thread_pool_ = nullptr;
  const ManifestParserOptions options_;
  const bool quiet_ = false;

  bool AddEdgeToGraph(Edge* edge, const LoadedFile& file, std::string* err);

  /// This function runs on a worker thread and adds a clump's declarations to
  /// the build graph.
  bool FinishAddingClumpToGraph(Clump* clump, std::string* err);

  bool FinishLoading(const std::vector<Clump*>& clumps, std::string* err);

public:
  ManifestLoader(State* state, ThreadPool* thread_pool,
                 ManifestParserOptions options, bool quiet)
      : state_(state), thread_pool_(thread_pool), options_(options),
        quiet_(quiet) {}

  bool Load(ManifestFileSet* file_set, const LoadedFile& root_manifest,
            std::string* err);
};

bool ManifestLoader::AddEdgeToGraph(Edge* edge, const LoadedFile& file,
                                    std::string* err) {

  const ScopePosition edge_pos = edge->pos_.scope_pos();

  // Look up the edge's rule.
  edge->rule_ = Scope::LookupRuleAtPos(edge->parse_state_.rule_name, edge_pos);
  if (edge->rule_ == nullptr) {
    std::string msg = "unknown build rule '" +
        edge->parse_state_.rule_name.AsString() + "'";
    return DecorateError(file, edge->parse_state_.rule_name_diag_pos, msg, err);
  }

  // Now that the edge's bindings are available, check whether the edge has a
  // pool. This check requires the full edge+rule evaluation system.
  std::string pool_name;
  if (!edge->EvaluateVariable(&pool_name, kPool, err, EdgeEval::kParseTime))
    return false;
  if (pool_name.empty()) {
    edge->pool_ = &State::kDefaultPool;
  } else {
    edge->pool_ = state_->LookupPoolAtPos(pool_name, edge->pos_.dfs_location());
    if (edge->pool_ == nullptr) {
      return DecorateError(file, edge->parse_state_.final_diag_pos,
                           "unknown pool name '" + pool_name + "'", err);
    }
  }

  edge->outputs_.reserve(edge->explicit_outs_ + edge->implicit_outs_);
  edge->inputs_.reserve(edge->explicit_deps_ + edge->implicit_deps_ +
                        edge->order_only_deps_);

  // Add the input and output nodes. We already lexed them in the first pass,
  // but we couldn't add them because scope bindings weren't available. To save
  // memory, the first pass only recorded the lexer position of each category
  // of input/output nodes, rather than each path's location.
  Lexer lexer(file.filename(), file.content(), file.content().data());
  for (const Edge::DeferredPathList& path_list :
      edge->parse_state_.deferred_path_lists) {
    std::vector<Node*>* vec = path_list.is_output ?
        &edge->outputs_ : &edge->inputs_;
    lexer.ResetPos(path_list.lexer_pos);
    for (int i = 0; i < path_list.count; ++i) {
      if (!AddPathToEdge(state_, *edge, vec, file, lexer, err))
        return false;
    }
    // Verify that there are no more paths to parse.
    LexedPath path;
    if (!lexer.ReadPath(&path, err) || !path.str_.empty()) {
      assert(false && "manifest file apparently changed during parsing");
      abort();
    }
  }

  // This compatibility mode filters nodes from the edge->inputs_ list; do it
  // before linking the edge inputs and nodes.
  if (options_.phony_cycle_action_ == kPhonyCycleActionWarn &&
      edge->maybe_phonycycle_diagnostic()) {
    // CMake 2.8.12.x and 3.0.x incorrectly write phony build statements that
    // reference themselves.  Ninja used to tolerate these in the build graph
    // but that has since been fixed.  Filter them out to support users of those
    // old CMake versions.
    Node* out = edge->outputs_[0];
    std::vector<Node*>::iterator new_end =
        std::remove(edge->inputs_.begin(), edge->inputs_.end(), out);
    if (new_end != edge->inputs_.end()) {
      edge->inputs_.erase(new_end, edge->inputs_.end());
      --edge->explicit_deps_;
      if (!quiet_) {
        Warning("phony target '%s' names itself as an input; ignoring "
                "[-w phonycycle=warn]", out->path().c_str());
      }
    }
  }

  // Multiple outputs aren't (yet?) supported with depslog.
  std::string deps_type;
  if (!edge->EvaluateVariable(&deps_type, kDeps, err, EdgeEval::kParseTime))
    return false;
  if (!deps_type.empty() && edge->outputs_.size() - edge->implicit_outs_ > 1) {
    return DecorateError(file, edge->parse_state_.final_diag_pos,
                         "multiple outputs aren't (yet?) supported by depslog; "
                         "bring this up on the mailing list if it affects you",
                         err);
  }

  return true;
}

bool ManifestLoader::FinishAddingClumpToGraph(Clump* clump, std::string* err) {
  std::string work_buf;

  // Precompute all binding values. Discard each evaluated string -- we just
  // need to make sure each binding's value isn't coming from the mmap'ed
  // manifest anymore.
  for (Binding* binding : clump->bindings_) {
    work_buf.clear();
    binding->Evaluate(&work_buf);
  }

  for (Edge* edge : clump->edges_) {
    if (!AddEdgeToGraph(edge, clump->file_, err))
      return false;
  }

  return true;
}

bool ManifestLoader::FinishLoading(const std::vector<Clump*>& clumps,
                                   std::string* err) {
  {
    // Most of this pass's time is spent adding the edges. (i.e. The time spent
    // evaluating the bindings is negligible.)
    METRIC_RECORD(".ninja load : edge setup");

    size_t output_count = 0;
    for (Clump* clump : clumps)
      output_count += clump->edge_output_count_;

    // Construct the initial graph of input/output nodes. Select an initial size
    // that's likely to keep the number of collisions low. The number of edges'
    // non-implicit outputs is a decent enough proxy for the final number of
    // nodes. (I see acceptable performance even with a much lower number of
    // buckets, e.g. 100 times fewer.)
    state_->paths_.reserve(state_->paths_.size() + output_count * 3);

    if (!PropagateError(err, ParallelMap(thread_pool_, clumps,
        [this](Clump* clump) {
      std::string err;
      FinishAddingClumpToGraph(clump, &err);
      return err;
    }))) {
      return false;
    }
  }
  {
    // Record the in-edge for each node that's built by an edge. Detect
    // duplicate edges.
    //
    // With dupbuild=warn (the default until 1.9.0), when two edges generate the
    // same node, remove the duplicate node from the output list of the later
    // edge. If all of an edge's outputs are removed, remove the edge from the
    // graph.
    METRIC_RECORD(".ninja load : link edge outputs");
    for (Clump* clump : clumps) {
      for (size_t edge_idx = 0; edge_idx < clump->edges_.size(); ) {
        // Scan all Edge outputs and link them to the Node objects.
        Edge* edge = clump->edges_[edge_idx];
        for (size_t i = 0; i < edge->outputs_.size(); ) {
          Node* output = edge->outputs_[i];
          if (output->in_edge() == nullptr) {
            output->set_in_edge(edge);
            ++i;
            continue;
          }
          // Two edges produce the same output node.
          if (options_.dupe_edge_action_ == kDupeEdgeActionError) {
            return DecorateError(clump->file_,
                                 edge->parse_state_.final_diag_pos,
                                 "multiple rules generate " + output->path() +
                                 " [-w dupbuild=err]", err);
          } else {
            if (!quiet_) {
              Warning("multiple rules generate %s. "
                      "builds involving this target will not be correct; "
                      "continuing anyway [-w dupbuild=warn]",
                      output->path().c_str());
            }
            if (edge->is_implicit_out(i))
              --edge->implicit_outs_;
            else
              --edge->explicit_outs_;
            edge->outputs_.erase(edge->outputs_.begin() + i);
          }
        }
        if (edge->outputs_.empty()) {
          // All outputs of the edge are already created by other edges. Remove
          // this edge from the graph. This removal happens before the edge's
          // inputs are linked to nodes.
          clump->edges_.erase(clump->edges_.begin() + edge_idx);
          continue;
        }
        ++edge_idx;
      }
    }
  }
  {
    // Now that all invalid edges are removed from the graph, record an out-edge
    // on each node that's needed by an edge.
    METRIC_RECORD(".ninja load : link edge inputs");
    ParallelMap(thread_pool_, clumps, [](Clump* clump) {
      for (Edge* edge : clump->edges_) {
        for (Node* input : edge->inputs_) {
          input->AddOutEdge(edge);
        }
      }
    });
  }
  {
    METRIC_RECORD(".ninja load : default targets");

    for (Clump* clump : clumps) {
      for (DefaultTarget* target : clump->default_targets_) {
        std::string path;
        EvaluatePathInScope(&path, target->parsed_path_,
                            target->pos_.scope_pos());
        uint64_t slash_bits;  // Unused because this only does lookup.
        std::string path_err;
        if (!CanonicalizePath(&path, &slash_bits, &path_err))
          return DecorateError(clump->file_, target->diag_pos_, path_err, err);

        Node* node = state_->LookupNodeAtPos(path, target->pos_.dfs_location());
        if (node == nullptr) {
          return DecorateError(clump->file_, target->diag_pos_,
                               "unknown target '" + path + "'", err);
        }
        state_->AddDefault(node);
      }
    }
  }
  {
    // Add the clump edges into the global edge vector, and assign edge IDs.
    // Edge IDs are used for custom protobuf-based Ninja frontends. An edge's ID
    // is equal to its index in the global edge vector, so delay the assignment
    // of edge IDs until we've removed duplicate edges above (dupbuild=warn).

    METRIC_RECORD(".ninja load : build edge table");

    // Copy edges to the global edge table.
    size_t old_size = state_->edges_.size();
    size_t new_size = old_size;
    for (Clump* clump : clumps) {
      new_size += clump->edges_.size();
    }
    state_->edges_.reserve(new_size);
    for (Clump* clump : clumps) {
      std::copy(clump->edges_.begin(), clump->edges_.end(),
                std::back_inserter(state_->edges_));
    }
    // Assign edge IDs.
    ParallelMap(thread_pool_, IntegralRange<size_t>(old_size, new_size),
        [this](size_t idx) {
      state_->edges_[idx]->id_ = idx;
    });
  }

  return true;
}

bool ManifestLoader::Load(ManifestFileSet* file_set,
                          const LoadedFile& root_manifest, std::string* err) {
  DfsParser dfs_parser(file_set, state_, thread_pool_);
  std::vector<Clump*> clumps;
  if (!dfs_parser.LoadManifestTree(root_manifest, &state_->root_scope_, &clumps,
                                   err)) {
    return false;
  }
  return FinishLoading(clumps, err);
}

} // anonymous namespace

bool ManifestParser::Load(const string& filename, string* err) {
  METRIC_RECORD(".ninja load");

  ManifestFileSet file_set(file_reader_);
  const LoadedFile* file = nullptr;
  if (!file_set.LoadFile(filename, &file, err))
    return false;

  std::unique_ptr<ThreadPool> thread_pool = CreateThreadPool();
  ManifestLoader loader(state_, thread_pool.get(), options_, false);
  return loader.Load(&file_set, *file, err);
}

bool ManifestParser::ParseTest(const string& input, string* err) {
  ManifestFileSet file_set(file_reader_);
  std::unique_ptr<ThreadPool> thread_pool = CreateThreadPool();
  ManifestLoader loader(state_, thread_pool.get(), options_, true);
  return loader.Load(&file_set, HeapLoadedFile("input", input), err);
}
