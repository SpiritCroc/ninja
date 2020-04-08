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

#include "graph.h"

#include <assert.h>
#include <stdio.h>

#include "build_log.h"
#include "debug_flags.h"
#include "depfile_parser.h"
#include "deps_log.h"
#include "disk_interface.h"
#include "metrics.h"
#include "parallel_map.h"
#include "state.h"
#include "util.h"

bool Node::PrecomputeStat(DiskInterface* disk_interface, std::string* err) {
  if (in_edge() != nullptr) {
    if (in_edge()->IsPhonyOutput()) {
      return true;
    }
    return (precomputed_mtime_ = disk_interface->LStat(path_.str(), nullptr, err)) != -1;
  } else {
    return (precomputed_mtime_ = disk_interface->Stat(path_.str(), err)) != -1;
  }
}

bool Node::Stat(DiskInterface* disk_interface, string* err) {
  if (in_edge() != nullptr) {
    assert(!in_edge()->IsPhonyOutput());
    return (mtime_ = disk_interface->LStat(path_.str(), nullptr, err)) != -1;
  } else {
    return (mtime_ = disk_interface->Stat(path_.str(), err)) != -1;
  }
}

bool Node::LStat(DiskInterface* disk_interface, bool* is_dir, string* err) {
  assert(in_edge() != nullptr);
  assert(!in_edge()->IsPhonyOutput());
  return (mtime_ = disk_interface->LStat(path_.str(), is_dir, err)) != -1;
}

bool DependencyScan::RecomputeNodesDirty(const std::vector<Node*>& initial_nodes,
                                         std::string* err) {
  METRIC_RECORD("dep scan");
  std::vector<Node*> all_nodes;
  std::vector<Edge*> all_edges;
  std::unique_ptr<ThreadPool> thread_pool = CreateThreadPool();

  {
    METRIC_RECORD("dep scan : collect nodes+edges");
    for (Node* node : initial_nodes)
      CollectPrecomputeLists(node, &all_nodes, &all_edges);
  }

  bool success = true;
  if (!PrecomputeNodesDirty(all_nodes, all_edges, thread_pool.get(), err))
    success = false;

  if (success) {
    METRIC_RECORD("dep scan : main pass");
    vector<Node*> stack;
    for (Node* node : initial_nodes) {
      stack.clear();
      if (!RecomputeNodeDirty(node, &stack, err)) {
        success = false;
        break;
      }
    }
  }

  {
    // Ensure that the precomputed mtime information can't be used after this
    // dependency scan finishes.
    METRIC_RECORD("dep scan : clear pre-stat");
    ParallelMap(thread_pool.get(), all_nodes, [](Node* node) {
      node->ClearPrecomputedStat();
    });
  }

  return success;
}

void DependencyScan::CollectPrecomputeLists(Node* node,
                                            std::vector<Node*>* nodes,
                                            std::vector<Edge*>* edges) {
  if (node->precomputed_dirtiness())
    return;
  node->set_precomputed_dirtiness(true);
  nodes->push_back(node);

  Edge* edge = node->in_edge();
  if (edge && !edge->precomputed_dirtiness_) {
    edge->precomputed_dirtiness_ = true;
    edges->push_back(edge);
    for (Node* node : edge->inputs_) {
      // Duplicate the dirtiness check here to avoid an unnecessary function
      // call. (The precomputed_dirtiness() will be inlined, but the recursive
      // call can't be.)
      if (!node->precomputed_dirtiness())
        CollectPrecomputeLists(node, nodes, edges);
    }
  }

  // Collect dependencies from the deps log. This pass could also examine
  // depfiles, but it would be a more intrusive design change, because we don't
  // want to parse a depfile twice.
  if (DepsLog::Deps* deps = deps_log()->GetDeps(node)) {
    for (int i = 0; i < deps->node_count; ++i) {
      Node* node = deps->nodes[i];
      // Duplicate the dirtiness check here to avoid an unnecessary function
      // call.
      if (!node->precomputed_dirtiness()) {
        CollectPrecomputeLists(node, nodes, edges);
      }
    }
  }
}

bool DependencyScan::PrecomputeNodesDirty(const std::vector<Node*>& nodes,
                                          const std::vector<Edge*>& edges,
                                          ThreadPool* thread_pool,
                                          std::string* err) {
  // Optimize the "null build" case by calling Stat in parallel on every node in
  // the transitive closure.
  //
  // The Windows RealDiskInterface::Stat uses a directory-based cache that isn't
  // thread-safe. Various tests also uses a non-thread-safe Stat, so disable the
  // parallelized stat'ing for them as well.
  if (disk_interface_->IsStatThreadSafe() &&
      GetOptimalThreadPoolJobCount() > 1) {
    METRIC_RECORD("dep scan : pre-stat nodes");
    if (!PropagateError(err, ParallelMap(thread_pool, nodes,
        [this](Node* node) {
      // Each node is guaranteed to appear at most once in the collected list
      // of nodes, so it's safe to modify the nodes from worker threads.
      std::string err;
      node->PrecomputeStat(disk_interface_, &err);
      return err;
    }))) {
      return false;
    }
  }

  {
    METRIC_RECORD("dep scan : precompute edge info");
    if (!PropagateError(err, ParallelMap(thread_pool, edges, [](Edge* edge) {
      // As with the node list, each edge appears at most once in the
      // collected list, so it's safe to modify the edges from worker threads.
      std::string err;
      edge->PrecomputeDepScanInfo(&err);
      return err;
    }))) {
      return false;
    }
  }

  return true;
}

bool DependencyScan::RecomputeNodeDirty(Node* node, std::vector<Node*>* stack,
                                        std::string* err) {
  Edge* edge = node->in_edge();
  if (!edge) {
    // If we already visited this leaf node then we are done.
    if (node->status_known())
      return true;
    // This node has no in-edge; it is dirty if it is missing.
    if (!node->StatIfNecessary(disk_interface_, err))
      return false;
    if (!node->exists())
      EXPLAIN("%s has no in-edge and is missing", node->path().c_str());
    node->set_dirty(!node->exists());
    return true;
  }

  // If we already finished this edge then we are done.
  if (edge->mark_ == Edge::VisitDone)
    return true;

  // If we encountered this edge earlier in the call stack we have a cycle.
  if (!VerifyDAG(node, stack, err))
    return false;

  // Mark the edge temporarily while in the call stack.
  edge->mark_ = Edge::VisitInStack;
  stack->push_back(node);

  bool dirty = false;
  bool phony_output = edge->IsPhonyOutput();
  edge->outputs_ready_ = true;
  edge->deps_missing_ = false;

  if (phony_output) {
    EXPLAIN("edge with output %s is a phony output, so is always dirty",
            node->path().c_str());
    dirty = true;

    if (edge->UsesDepsLog() || edge->UsesDepfile()) {
      *err = "phony output " + node->path() + " has deps, which does not make sense.";
      return false;
    }
  } else {
    // Load output mtimes so we can compare them to the most recent input below.
    for (vector<Node*>::iterator o = edge->outputs_.begin();
         o != edge->outputs_.end(); ++o) {
      if (!(*o)->StatIfNecessary(disk_interface_, err))
        return false;
    }

    if (!dep_loader_.LoadDeps(edge, err)) {
      if (!err->empty())
        return false;
      // Failed to load dependency info: rebuild to regenerate it.
      // LoadDeps() did EXPLAIN() already, no need to do it here.
      dirty = edge->deps_missing_ = true;
    }
  }

  // Visit all inputs; we're dirty if any of the inputs are dirty.
  Node* most_recent_input = NULL;
  for (vector<Node*>::iterator i = edge->inputs_.begin();
       i != edge->inputs_.end(); ++i) {
    // Visit this input.
    if (!RecomputeNodeDirty(*i, stack, err))
      return false;

    // If an input is not ready, neither are our outputs.
    Edge* in_edge = (*i)->in_edge();
    if (in_edge != nullptr) {
      if (!in_edge->outputs_ready_)
        edge->outputs_ready_ = false;
    }

    if (!phony_output && !edge->is_order_only(i - edge->inputs_.begin())) {
      if (in_edge != nullptr && in_edge->IsPhonyOutput()) {
        *err = "real file '" + node->path() +
               "' depends on phony output '" + (*i)->path() + "'\n";
        return false;
      }

      // If a regular input is dirty (or missing), we're dirty.
      // Otherwise consider mtime.
      if ((*i)->dirty()) {
        EXPLAIN("%s is dirty", (*i)->path().c_str());
        dirty = true;
      } else {
        if (!most_recent_input || (*i)->mtime() > most_recent_input->mtime()) {
          most_recent_input = *i;
        }
      }
    }
  }

  // We may also be dirty due to output state: missing outputs, out of
  // date outputs, etc.  Visit all outputs and determine whether they're dirty.
  if (!dirty)
    if (!RecomputeOutputsDirty(edge, most_recent_input, &dirty, err))
      return false;

  // Finally, visit each output and update their dirty state if necessary.
  for (vector<Node*>::iterator o = edge->outputs_.begin();
       o != edge->outputs_.end(); ++o) {
    if (dirty)
      (*o)->MarkDirty();
  }

  // If an edge is dirty, its outputs are normally not ready.  (It's
  // possible to be clean but still not be ready in the presence of
  // order-only inputs.)
  // But phony edges with no inputs have nothing to do, so are always
  // ready.
  if (dirty && !(edge->is_phony() && edge->inputs_.empty()))
    edge->outputs_ready_ = false;

  // Mark the edge as finished during this walk now that it will no longer
  // be in the call stack.
  edge->mark_ = Edge::VisitDone;
  assert(stack->back() == node);
  stack->pop_back();

  return true;
}

bool DependencyScan::VerifyDAG(Node* node, vector<Node*>* stack, string* err) {
  Edge* edge = node->in_edge();
  assert(edge != NULL);

  // If we have no temporary mark on the edge then we do not yet have a cycle.
  if (edge->mark_ != Edge::VisitInStack)
    return true;

  // We have this edge earlier in the call stack.  Find it.
  vector<Node*>::iterator start = stack->begin();
  while (start != stack->end() && (*start)->in_edge() != edge)
    ++start;
  assert(start != stack->end());

  // Make the cycle clear by reporting its start as the node at its end
  // instead of some other output of the starting edge.  For example,
  // running 'ninja b' on
  //   build a b: cat c
  //   build c: cat a
  // should report a -> c -> a instead of b -> c -> a.
  *start = node;

  // Construct the error message rejecting the cycle.
  *err = "dependency cycle: ";
  for (vector<Node*>::const_iterator i = start; i != stack->end(); ++i) {
    err->append((*i)->path());
    err->append(" -> ");
  }
  err->append((*start)->path());

  if ((start + 1) == stack->end() && edge->maybe_phonycycle_diagnostic()) {
    // The manifest parser would have filtered out the self-referencing
    // input if it were not configured to allow the error.
    err->append(" [-w phonycycle=err]");
  }

  return false;
}

bool DependencyScan::RecomputeOutputsDirty(Edge* edge, Node* most_recent_input,
                                           bool* outputs_dirty, string* err) {
  assert(!edge->IsPhonyOutput());

  uint64_t command_hash = edge->GetCommandHash();
  for (vector<Node*>::iterator o = edge->outputs_.begin();
       o != edge->outputs_.end(); ++o) {
    if (edge->is_phony()) {
      // Phony edges don't write any output.  Outputs are only dirty if
      // there are no inputs and we're missing the output.
      if (edge->inputs_.empty() && !(*o)->exists()) {
        // For phony targets defined in the ninja file, error when using dirty phony edges.
        // The phony edges automatically created from depfiles still need the old behavior.
        if (missing_phony_is_err_ && !edge->phony_from_depfile_) {
          *err = "output " + (*o)->path() + " of phony edge doesn't exist. Missing 'phony_output = true'?";
          return false;
        } else {
          EXPLAIN("output %s of phony edge with no inputs doesn't exist",
                  (*o)->path().c_str());
          *outputs_dirty = true;
          return true;
        }
      }
      continue;
    }
    if (RecomputeOutputDirty(edge, most_recent_input, command_hash, *o)) {
      *outputs_dirty = true;
      return true;
    }
  }
  return true;
}

bool DependencyScan::RecomputeOutputDirty(Edge* edge,
                                          Node* most_recent_input,
                                          uint64_t command_hash,
                                          Node* output) {
  assert(!edge->is_phony());

  BuildLog::LogEntry* entry = 0;

  // Dirty if we're missing the output.
  if (!output->exists()) {
    EXPLAIN("output %s doesn't exist", output->path().c_str());
    return true;
  }

  // Dirty if the output is older than the input.
  if (most_recent_input && output->mtime() < most_recent_input->mtime()) {
    TimeStamp output_mtime = output->mtime();

    // If this is a restat rule, we may have cleaned the output with a restat
    // rule in a previous run and stored the most recent input mtime in the
    // build log.  Use that mtime instead, so that the file will only be
    // considered dirty if an input was modified since the previous run.
    bool used_restat = false;
    if (edge->IsRestat() && build_log() &&
        (entry = build_log()->LookupByOutput(output->path_hashed()))) {
      output_mtime = entry->mtime;
      used_restat = true;
    }

    if (output_mtime < most_recent_input->mtime()) {
      EXPLAIN("%soutput %s older than most recent input %s "
              "(%" PRId64 " vs %" PRId64 ")",
              used_restat ? "restat of " : "", output->path().c_str(),
              most_recent_input->path().c_str(),
              output_mtime, most_recent_input->mtime());
      return true;
    }
  }

  if (build_log()) {
    bool generator = edge->IsGenerator();
    if (entry || (entry = build_log()->LookupByOutput(output->path_hashed()))) {
      if (!generator &&
          command_hash != entry->command_hash) {
        // May also be dirty due to the command changing since the last build.
        // But if this is a generator rule, the command changing does not make us
        // dirty.
        EXPLAIN("command line changed for %s", output->path().c_str());
        return true;
      }
      if (most_recent_input && entry->mtime < most_recent_input->mtime()) {
        // May also be dirty due to the mtime in the log being older than the
        // mtime of the most recent input.  This can occur even when the mtime
        // on disk is newer if a previous run wrote to the output file but
        // exited with an error or was interrupted.
        EXPLAIN("recorded mtime of %s older than most recent input %s (%" PRId64 " vs %" PRId64 ")",
                output->path().c_str(), most_recent_input->path().c_str(),
                entry->mtime, most_recent_input->mtime());
        return true;
      }
    }
    if (!entry && !generator) {
      EXPLAIN("command line not found in log for %s", output->path().c_str());
      return true;
    }
  }

  return false;
}

bool Edge::AllInputsReady() const {
  for (vector<Node*>::const_iterator i = inputs_.begin();
       i != inputs_.end(); ++i) {
    if ((*i)->in_edge() && !(*i)->in_edge()->outputs_ready())
      return false;
  }
  return true;
}

static const HashedStrView kIn        { "in" };
static const HashedStrView kInNewline { "in_newline" };
static const HashedStrView kOut       { "out" };

bool EdgeEval::EvaluateVariable(std::string* out_append,
                                const HashedStrView& var,
                                std::string* err) {
  if (var == kIn || var == kInNewline) {
    int explicit_deps_count = edge_->inputs_.size() - edge_->implicit_deps_ -
      edge_->order_only_deps_;
    AppendPathList(out_append,
                   edge_->inputs_.begin(),
                   edge_->inputs_.begin() + explicit_deps_count,
                   var == kIn ? ' ' : '\n');
    return true;
  } else if (var == kOut) {
    int explicit_outs_count = edge_->outputs_.size() - edge_->implicit_outs_;
    AppendPathList(out_append,
                   edge_->outputs_.begin(),
                   edge_->outputs_.begin() + explicit_outs_count,
                   ' ');
    return true;
  }

  if (edge_->EvaluateVariableSelfOnly(out_append, var))
    return true;

  // Search for a matching rule binding.
  if (const std::string* binding_pattern = edge_->rule().GetBinding(var)) {
    // Detect recursive rule variable usage.
    if (recursion_count_ == kEvalRecursionLimit) {
      std::string cycle = recursion_vars_[0].AsString();
      for (int i = 1; i < kEvalRecursionLimit; ++i) {
        cycle += " -> " + recursion_vars_[i].AsString();
        if (recursion_vars_[i] == recursion_vars_[0])
          break;
      }
      *err = "cycle in rule variables: " + cycle;
      return false;
    }
    recursion_vars_[recursion_count_++] = var.str_view();

    return EvaluateBindingOnRule(out_append, *binding_pattern, this, err);
  }

  // Fall back to the edge's enclosing scope.
  if (eval_phase_ == EdgeEval::kParseTime) {
    Scope::EvaluateVariableAtPos(out_append, var, edge_->pos_.scope_pos());
  } else {
    Scope::EvaluateVariable(out_append, var, edge_->pos_.scope());
  }
  return true;
}

void EdgeEval::AppendPathList(std::string* out_append,
                              std::vector<Node*>::iterator begin,
                              std::vector<Node*>::iterator end,
                              char sep) {
  for (auto it = begin; it != end; ++it) {
    if (it != begin)
      out_append->push_back(sep);

    const string& path = (*it)->PathDecanonicalized();
    if (escape_in_out_ == kShellEscape) {
#if _WIN32
      GetWin32EscapedString(path, out_append);
#else
      GetShellEscapedString(path, out_append);
#endif
    } else {
      out_append->append(path);
    }
  }
}

static const HashedStrView kCommand         { "command" };
static const HashedStrView kDepfile         { "depfile" };
static const HashedStrView kRspfile         { "rspfile" };
static const HashedStrView kRspFileContent  { "rspfile_content" };

bool Edge::EvaluateCommand(std::string* out_append, bool incl_rsp_file,
                           std::string* err) {
  METRIC_RECORD("eval command");
  if (!EvaluateVariable(out_append, kCommand, err))
    return false;
  if (incl_rsp_file) {
    std::string rspfile_content;
    if (!EvaluateVariable(&rspfile_content, kRspFileContent, err))
      return false;
    if (!rspfile_content.empty()) {
      out_append->append(";rspfile=");
      out_append->append(rspfile_content);
    }
  }
  return true;
}

std::string Edge::EvaluateCommand(bool incl_rsp_file) {
  std::string command;
  std::string err;
  if (!EvaluateCommand(&command, incl_rsp_file, &err))
    Fatal("%s", err.c_str());
  return command;
}

static const HashedStrView kRestat      { "restat" };
static const HashedStrView kGenerator   { "generator" };
static const HashedStrView kDeps        { "deps" };
static const HashedStrView kPhonyOutput  { "phony_output" };

bool Edge::PrecomputeDepScanInfo(std::string* err) {
  if (dep_scan_info_.valid)
    return true;

  // Precompute boolean flags.
  auto get_bool_var = [this, err](const HashedStrView& var,
                                  EdgeEval::EscapeKind escape, bool* out) {
    std::string value;
    if (!EvaluateVariable(&value, var, err, EdgeEval::kFinalScope, escape))
      return false;
    *out = !value.empty();
    return true;
  };
  if (!get_bool_var(kRestat,      EdgeEval::kShellEscape, &dep_scan_info_.restat))       return false;
  if (!get_bool_var(kGenerator,   EdgeEval::kShellEscape, &dep_scan_info_.generator))    return false;
  if (!get_bool_var(kDeps,        EdgeEval::kShellEscape, &dep_scan_info_.deps))         return false;
  if (!get_bool_var(kDepfile,     EdgeEval::kDoNotEscape, &dep_scan_info_.depfile))      return false;
  if (!get_bool_var(kPhonyOutput, EdgeEval::kShellEscape, &dep_scan_info_.phony_output)) return false;

  // Precompute the command hash.
  std::string command;
  if (!EvaluateCommand(&command, /*incl_rsp_file=*/true, err))
    return false;
  dep_scan_info_.command_hash = BuildLog::LogEntry::HashCommand(command);

  dep_scan_info_.valid = true;
  return true;
}

/// Returns dependency-scanning info or exits with a fatal error.
const Edge::DepScanInfo& Edge::ComputeDepScanInfo() {
  std::string err;
  if (!PrecomputeDepScanInfo(&err))
    Fatal("%s", err.c_str());
  return dep_scan_info_;
}

bool Edge::EvaluateVariable(std::string* out_append, const HashedStrView& key,
                            std::string* err, EdgeEval::EvalPhase phase,
                            EdgeEval::EscapeKind escape) {
  EdgeEval eval(this, phase, escape);
  return eval.EvaluateVariable(out_append, key, err);
}

std::string Edge::GetBindingImpl(const HashedStrView& key,
                                 EdgeEval::EvalPhase phase,
                                 EdgeEval::EscapeKind escape) {
  std::string result;
  std::string err;
  if (!EvaluateVariable(&result, key, &err, phase, escape))
    Fatal("%s", err.c_str());
  return result;
}

std::string Edge::GetBinding(const HashedStrView& key) {
  return GetBindingImpl(key, EdgeEval::kFinalScope, EdgeEval::kShellEscape);
}

std::string Edge::GetUnescapedDepfile() {
  return GetBindingImpl(kDepfile, EdgeEval::kFinalScope, EdgeEval::kDoNotEscape);
}

std::string Edge::GetUnescapedRspfile() {
  return GetBindingImpl(kRspfile, EdgeEval::kFinalScope, EdgeEval::kDoNotEscape);
}

void Edge::Dump(const char* prefix) const {
  printf("%s[ ", prefix);
  for (vector<Node*>::const_iterator i = inputs_.begin();
       i != inputs_.end() && *i != NULL; ++i) {
    printf("%s ", (*i)->path().c_str());
  }
  printf("--%s-> ", rule_->name().c_str());
  for (vector<Node*>::const_iterator i = outputs_.begin();
       i != outputs_.end() && *i != NULL; ++i) {
    printf("%s ", (*i)->path().c_str());
  }
  if (pool_) {
    if (!pool_->name().empty()) {
      printf("(in pool '%s')", pool_->name().c_str());
    }
  } else {
    printf("(null pool?)");
  }
  printf("] 0x%p\n", this);
}

bool Edge::is_phony() const {
  return rule_ == &State::kPhonyRule;
}

bool Edge::use_console() const {
  return pool() == &State::kConsolePool;
}

bool Edge::maybe_phonycycle_diagnostic() const {
  // CMake 2.8.12.x and 3.0.x produced self-referencing phony rules
  // of the form "build a: phony ... a ...".   Restrict our
  // "phonycycle" diagnostic option to the form it used.
  return is_phony() && outputs_.size() == 1 && implicit_outs_ == 0 &&
      implicit_deps_ == 0 && order_only_deps_ == 0;
}

bool Edge::EvaluateVariableSelfOnly(std::string* out_append,
                                    const HashedStrView& var) const {
  // ninja allows declaring the same binding repeatedly on an edge. Use the
  // last matching binding.
  const auto it_end = unevaled_bindings_.rend();
  for (auto it = unevaled_bindings_.rbegin(); it != it_end; ++it) {
    if (var == it->first) {
      EvaluateBindingInScope(out_append, it->second, pos_.scope_pos());
      return true;
    }
  }
  return false;
}

// static
string Node::PathDecanonicalized(const string& path, uint64_t slash_bits) {
  string result = path;
#ifdef _WIN32
  uint64_t mask = 1;
  for (char* c = &result[0]; (c = strchr(c, '/')) != NULL;) {
    if (slash_bits & mask)
      *c = '\\';
    c++;
    mask <<= 1;
  }
#endif
  return result;
}

Node::~Node() {
  EdgeList* node = out_edges_.load();
  while (node != nullptr) {
    EdgeList* next = node->next;
    delete node;
    node = next;
  }
}

// Does the node have at least one out edge?
bool Node::has_out_edge() const {
  return out_edges_.load() != nullptr;
}

std::vector<Edge*> Node::GetOutEdges() const {
  // Include out-edges from the manifest.
  std::vector<Edge*> result;
  for (EdgeList* node = out_edges_.load(); node != nullptr; node = node->next) {
    result.push_back(node->edge);
  }
  std::sort(result.begin(), result.end(), EdgeCmp());

  // Add extra out-edges from depfiles and the deps log. Preserve the order
  // of these extra edges; don't sort them.
  fprintf(stderr, "graph1\n");
  std::copy(dep_scan_out_edges_.begin(), dep_scan_out_edges_.end(),
            std::back_inserter(result));

  return result;
}

void Node::AddOutEdge(Edge* edge) {
  EdgeList* new_node = new EdgeList { edge };
  while (true) {
    EdgeList* cur_head = out_edges_.load();
    new_node->next = cur_head;
    if (out_edges_.compare_exchange_weak(cur_head, new_node))
      break;
  }
}

void Node::Dump(const char* prefix) const {
  printf("%s <%s 0x%p> mtime: %" PRId64 "%s, (:%s), ",
         prefix, path().c_str(), this,
         mtime(), mtime() ? "" : " (:missing)",
         dirty() ? " dirty" : " clean");
  if (in_edge()) {
    in_edge()->Dump("in-edge: ");
  } else {
    printf("no in-edge\n");
  }
  printf(" out edges:\n");
  const std::vector<Edge*> out_edges = GetOutEdges();
  for (vector<Edge*>::const_iterator e = out_edges.begin();
       e != out_edges.end() && *e != NULL; ++e) {
    (*e)->Dump(" +- ");
  }
}

bool ImplicitDepLoader::LoadDeps(Edge* edge, string* err) {
  if (edge->UsesDepsLog())
    return LoadDepsFromLog(edge, err);

  if (edge->UsesDepfile()) {
    std::string depfile = edge->GetUnescapedDepfile();
    assert(!depfile.empty() &&
           "UsesDepfile was set, so the depfile should be non-empty");
    return LoadDepFile(edge, depfile, err);
  }

  // No deps to load.
  return true;
}

bool ImplicitDepLoader::LoadDepFile(Edge* edge, const string& path,
                                    string* err) {
  METRIC_RECORD("depfile load");
  // Read depfile content.  Treat a missing depfile as empty.
  string content;
  switch (disk_interface_->ReadFile(path, &content, err)) {
  case DiskInterface::Okay:
    break;
  case DiskInterface::NotFound:
    err->clear();
    break;
  case DiskInterface::OtherError:
    *err = "loading '" + path + "': " + *err;
    return false;
  }
  // On a missing depfile: return false and empty *err.
  if (content.empty()) {
    EXPLAIN("depfile '%s' is missing", path.c_str());
    return false;
  }

  DepfileParser depfile;
  string depfile_err;
  if (!depfile.Parse(&content, &depfile_err)) {
    *err = path + ": " + depfile_err;
    return false;
  }

  uint64_t unused;
  if (!CanonicalizePath(const_cast<char*>(depfile.out_.str_),
                        &depfile.out_.len_, &unused, err)) {
    *err = path + ": " + *err;
    return false;
  }

  // Check that this depfile matches the edge's output, if not return false to
  // mark the edge as dirty.
  Node* first_output = edge->outputs_[0];
  if (first_output->path() != depfile.out_) {
    EXPLAIN("expected depfile '%s' to mention '%s', got '%s'", path.c_str(),
            first_output->path().c_str(), depfile.out_.AsString().c_str());
    return false;
  }

  // Preallocate space in edge->inputs_ to be filled in below.
  vector<Node*>::iterator implicit_dep =
      PreallocateSpace(edge, depfile.ins_.size());

  // Add all its in-edges.
  for (vector<StringPiece>::iterator i = depfile.ins_.begin();
       i != depfile.ins_.end(); ++i, ++implicit_dep) {
    uint64_t slash_bits;
    if (!CanonicalizePath(const_cast<char*>(i->str_), &i->len_, &slash_bits,
                          err))
      return false;

    fprintf(stderr, "graph 10\n");
    Node* node = state_->GetNode(*i, slash_bits);
    *implicit_dep = node;
    node->AddOutEdgeDepScan(edge);
    CreatePhonyInEdge(node);
  }

  return true;
}

bool ImplicitDepLoader::LoadDepsFromLog(Edge* edge, string* err) {
  // NOTE: deps are only supported for single-target edges.
  Node* output = edge->outputs_[0];
  DepsLog::Deps* deps = deps_log_->GetDeps(output);
  if (!deps) {
    EXPLAIN("deps for '%s' are missing", output->path().c_str());
    return false;
  }

  // Deps are invalid if the output is newer than the deps.
  if (output->mtime() > deps->mtime) {
    EXPLAIN("stored deps info out of date for '%s' (%" PRId64 " vs %" PRId64 ")",
            output->path().c_str(), deps->mtime, output->mtime());
    return false;
  }

  vector<Node*>::iterator implicit_dep =
      PreallocateSpace(edge, deps->node_count);
  for (int i = 0; i < deps->node_count; ++i, ++implicit_dep) {
    Node* node = deps->nodes[i];
    *implicit_dep = node;
    node->AddOutEdgeDepScan(edge);
    CreatePhonyInEdge(node);
  }
  return true;
}

vector<Node*>::iterator ImplicitDepLoader::PreallocateSpace(Edge* edge,
                                                            int count) {
  fprintf(stderr, "graph2\n");
  edge->inputs_.insert(edge->inputs_.end() - edge->order_only_deps_,
                       (size_t)count, 0);
  edge->implicit_deps_ += count;
  return edge->inputs_.end() - edge->order_only_deps_ - count;
}

void ImplicitDepLoader::CreatePhonyInEdge(Node* node) {
  if (node->in_edge())
    return;

  Edge* phony_edge = state_->AddEdge(&State::kPhonyRule);
  node->set_in_edge(phony_edge);
  phony_edge->outputs_.push_back(node);
  ++phony_edge->explicit_outs_;

  // RecomputeDirty might not be called for phony_edge if a previous call
  // to RecomputeDirty had caused the file to be stat'ed.  Because previous
  // invocations of RecomputeDirty would have seen this node without an
  // input edge (and therefore ready), we have to set outputs_ready_ to true
  // to avoid a potential stuck build.  If we do call RecomputeDirty for
  // this node, it will simply set outputs_ready_ to the correct value.
  phony_edge->outputs_ready_ = true;

  phony_edge->phony_from_depfile_ = true;
}
