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

#include "state.h"

#include <assert.h>
#include <stdio.h>

#include "edit_distance.h"
#include "graph.h"
#include "metrics.h"
#include "util.h"

Pool::Pool(const HashedStrView& name, int depth) : name_(name), depth_(depth) {
  pos_.base = new BasePosition {{ &State::kBuiltinScope, 0 }}; // leaked
}

void Pool::EdgeScheduled(const Edge& edge) {
  if (depth_ != 0)
    current_use_ += edge.weight();
}

void Pool::EdgeFinished(const Edge& edge) {
  if (depth_ != 0)
    current_use_ -= edge.weight();
}

void Pool::DelayEdge(Edge* edge) {
  assert(depth_ != 0);
  fprintf(stderr, "state\n");
  delayed_.insert(edge);
}

void Pool::RetrieveReadyEdges(EdgeSet* ready_queue) {
  DelayedEdges::iterator it = delayed_.begin();
  while (it != delayed_.end()) {
    Edge* edge = *it;
    if (current_use_ + edge->weight() > depth_)
      break;
  fprintf(stderr, "state1\n");
    ready_queue->insert(edge);
    EdgeScheduled(*edge);
    ++it;
  }
  delayed_.erase(delayed_.begin(), it);
}

void Pool::Dump() const {
  printf("%s (%d/%d) ->\n", name_.c_str(), current_use_, depth_);
  for (DelayedEdges::const_iterator it = delayed_.begin();
       it != delayed_.end(); ++it)
  {
    printf("\t");
    (*it)->Dump();
  }
}

Scope State::kBuiltinScope({});
Pool State::kDefaultPool("", 0);
Pool State::kConsolePool("console", 1);
Rule State::kPhonyRule("phony");

State::State() {
  // Reserve scope position (root, 0) for built-in rules.
  root_scope_.AllocDecls(1);

  AddBuiltinRule(&kPhonyRule);
  AddPool(&kDefaultPool);
  AddPool(&kConsolePool);
}

// Add a built-in rule at the top of the root scope.
void State::AddBuiltinRule(Rule* rule) {
  root_scope_.AddRule(rule);
}

bool State::AddPool(Pool* pool) {
  fprintf(stderr, "state2\n");
  return pools_.insert({ pool->name_hashed(), pool }).second;
}

Edge* State::AddEdge(const Rule* rule) {
  Edge* edge = new Edge();
  edge->pos_.base = new BasePosition {{ &root_scope_, 0 }}; // leaked
  edge->rule_ = rule;
  edge->pool_ = &State::kDefaultPool;
  edge->id_ = edges_.size();
  edges_.push_back(edge);
  return edge;
}

Pool* State::LookupPool(const HashedStrView& pool_name) {
  auto i = pools_.find(pool_name);
  if (i == pools_.end())
    return nullptr;
  return i->second;
}

Pool* State::LookupPoolAtPos(const HashedStrView& pool_name,
                             DeclIndex dfs_location) {
  Pool* result = LookupPool(pool_name);
  if (result == nullptr) return nullptr;
  return result->dfs_location() < dfs_location ? result : nullptr;
}

Node* State::GetNode(const HashedStrView& path, uint64_t slash_bits) {
  if (Node** opt_node = paths_.Lookup(path))
    return *opt_node;
  // Create a new node and try to insert it.
  std::unique_ptr<Node> node(new Node(path, slash_bits));
  fprintf(stderr, "state3\n");
  if (paths_.insert({node->path_hashed(), node.get()}).second) {
  fprintf(stderr, "state3 release done\n");
    return node.release();
  }
  fprintf(stderr, "state3 done\n");
  // Another thread beat us to it. Use its node instead.
  return *paths_.Lookup(path);
}

Node* State::LookupNode(const HashedStrView& path) const {
  if (Node* const* opt_node = paths_.Lookup(path))
    return *opt_node;
  return nullptr;
}

Node* State::LookupNodeAtPos(const HashedStrView& path,
                             DeclIndex dfs_location) const {
  Node* result = LookupNode(path);
  return result && result->dfs_location() < dfs_location ? result : nullptr;
}

Node* State::SpellcheckNode(StringPiece path) {
  const bool kAllowReplacements = true;
  const int kMaxValidEditDistance = 3;

  int min_distance = kMaxValidEditDistance + 1;
  Node* result = NULL;
  for (Paths::iterator i = paths_.begin(); i != paths_.end(); ++i) {
    int distance = EditDistance(
        i->first.str_view(), path, kAllowReplacements, kMaxValidEditDistance);
    if (distance < min_distance && i->second) {
      min_distance = distance;
      result = i->second;
    }
  }
  return result;
}

void State::AddIn(Edge* edge, StringPiece path, uint64_t slash_bits) {
  fprintf(stderr, "state 11\n");
  Node* node = GetNode(path, slash_bits);
  edge->inputs_.push_back(node);
  node->AddOutEdge(edge);
}

bool State::AddOut(Edge* edge, StringPiece path, uint64_t slash_bits) {
  fprintf(stderr, "state 12\n");
  Node* node = GetNode(path, slash_bits);
  if (node->in_edge())
    return false;
  edge->outputs_.push_back(node);
  node->set_in_edge(edge);
  return true;
}

vector<Node*> State::RootNodes(string* err) const {
  vector<Node*> root_nodes;
  // Search for nodes with no output.
  for (vector<Edge*>::const_iterator e = edges_.begin();
       e != edges_.end(); ++e) {
    for (vector<Node*>::const_iterator out = (*e)->outputs_.begin();
         out != (*e)->outputs_.end(); ++out) {
      if (!(*out)->has_out_edge())
        root_nodes.push_back(*out);
    }
  }

  if (!edges_.empty() && root_nodes.empty())
    *err = "could not determine root nodes of build graph";

  return root_nodes;
}

vector<Node*> State::DefaultNodes(string* err) const {
  return defaults_.empty() ? RootNodes(err) : defaults_;
}

DeclIndex State::AllocDfsLocation(DeclIndex count) {
  DeclIndex result = dfs_location_;
  dfs_location_ += count;
  return result;
}

void State::Reset() {
  for (Paths::iterator i = paths_.begin(); i != paths_.end(); ++i)
    i->second->ResetState();
  for (vector<Edge*>::iterator e = edges_.begin(); e != edges_.end(); ++e) {
    (*e)->outputs_ready_ = false;
    (*e)->mark_ = Edge::VisitNone;
    (*e)->precomputed_dirtiness_ = false;
  }
}

void State::Dump() {
  for (Paths::iterator i = paths_.begin(); i != paths_.end(); ++i) {
    Node* node = i->second;
    printf("%s %s [id:%d]\n",
           node->path().c_str(),
           node->status_known() ? (node->dirty() ? "dirty" : "clean")
                                : "unknown",
           node->id());
  }
  if (!pools_.empty()) {
    printf("resource_pools:\n");
    for (auto it = pools_.begin(); it != pools_.end(); ++it) {
      if (!it->second->name().empty()) {
        it->second->Dump();
      }
    }
  }
}
