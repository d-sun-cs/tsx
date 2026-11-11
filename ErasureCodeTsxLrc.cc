// SPDX-License-Identifier: LGPL-2.1-or-later
#include "ErasureCodeTsxLrc.h"

#include <algorithm>
#include <cerrno>
#include "include/buffer.h"
#include "erasure-code/ErasureCodePlugin.h"

using namespace std;
using namespace ceph;

namespace {
bool contains(const set<int>& available, const set<int>& want)
{
  return includes(available.begin(), available.end(), want.begin(), want.end());
}
set<int> ids_of(const map<int, bufferlist>& chunks)
{
  set<int> ids;
  for (const auto& chunk : chunks) {
    ids.insert(chunk.first);
  }
  return ids;
}
}

int ErasureCodeTsxLrc::init(ErasureCodeProfile& profile, ostream* ss)
{
  if (profile["mode"] != "scaffold" && profile["mode"] != "fasttt") {
    *ss << "tsx_lrc requires mode=scaffold; the FastTT backend is not implemented";
    return -EINVAL;
  }
  int k, m, locality;
  int r = to_int("k", profile, &k, "8", ss);
  r |= to_int("m", profile, &m, "2", ss);
  r |= to_int("locality", profile, &locality, "4", ss);
  if (r) {
    return r;
  }
  if (k != 8 || m != 2 || locality != 4 || profile.count("l") ||
      profile.count("mapping") || profile.count("layers") ||
      profile.count("crush-locality") || profile.count("crush-steps")) {
    *ss << "tsx_lrc scaffold supports only k=8 m=2 locality=4 and its fixed layout";
    return -EINVAL;
  }
  r = layers_init(profile["mode"] == "fasttt", ss);
  if (r) {
    return r;
  }
  return ErasureCode::init(profile, ss);
}

int ErasureCodeTsxLrc::init_fasttt_global_layer(ErasureCodeInterfaceRef* code,
                                              ostream* ss)
{
  // TODO(FastTT-LRC): G+/G- kernel, explicit shift tails and their storage
  // contract; then joint recovery using virtual P0=L0 XOR L1.
  // Existing tsx2 produces G+/P0 and is not a substitute for G+/G-.
  code->reset();
  *ss << "FastTT-LRC global kernel, tails and joint repair are not implemented";
  return -EOPNOTSUPP;
}

int ErasureCodeTsxLrc::layers_init(bool fasttt, ostream* ss)
{
  layers.clear();
  layers.push_back({{0,1,2,3,4,5,6,7}, {8,9}, {}, {}});
  layers.push_back({{0,1,2,3}, {10}, {}, {}});
  layers.push_back({{4,5,6,7}, {11}, {}, {}});
  for (auto& layer : layers) {
    layer.chunks = layer.data;
    layer.chunks.insert(layer.chunks.end(), layer.coding.begin(), layer.coding.end());
    if (fasttt && layer.coding.size() == 2) {
      int r = init_fasttt_global_layer(&layer.erasure_code, ss);
      if (r) {
        return r;
      }
      continue;
    }
    // Executable placeholders. Local ISA m=1 really computes XOR.
    // TODO(FastTT-LRC): replace the global child only after its tail contract
    // is defined; the planner also needs a joint TT/local recovery strategy.
    ErasureCodeProfile child{{"plugin", "isa"},
      {"technique", layer.coding.size() == 2 ? "cauchy" : "reed_sol_van"},
      {"k", std::to_string(layer.data.size())}, {"m", std::to_string(layer.coding.size())}};
    int r = ErasureCodePluginRegistry::instance().factory(
      "isa", directory, child, &layer.erasure_code, ss);
    if (r) {
      return r;
    }
  }
  return 0;
}

unsigned int ErasureCodeTsxLrc::get_chunk_size(unsigned int object_size) const
{
  return layers.empty() ? 0 : layers.front().erasure_code->get_chunk_size(object_size);
}

bool ErasureCodeTsxLrc::valid_ids(const set<int>& ids) const
{
  return ids.empty() || (*ids.begin() >= 0 && *ids.rbegin() < 12);
}

int ErasureCodeTsxLrc::make_repair_plan(const set<int>& want,
                                      const set<int>& available,
                                      RepairPlan* plan) const
{
  *plan = {};
  if (layers.size() != 3 || !valid_ids(want) || !valid_ids(available)) {
    return -EINVAL;
  }
  set<int> known = available;
  vector<RepairStep> candidates;
  while (!contains(known, want)) {
    bool progress = false;
    // Like official LRC: local layers first, then the global layer.
    // Repeat when the global layer unlocks a local parity repair.
    for (int i = static_cast<int>(layers.size()) - 1; i >= 0; --i) {
      RepairStep step{static_cast<unsigned int>(i), {}, {}};
      for (int id : layers[i].chunks) {
        (known.count(id) ? step.helpers : step.targets).insert(id);
      }
      if (step.targets.empty() || step.targets.size() > layers[i].coding.size()) {
        continue;
      }
      candidates.push_back(step);
      known.insert(step.targets.begin(), step.targets.end());
      progress = true;
      if (contains(known, want)) {
        break;
      }
    }
    if (!progress) {
      return -EIO;
    }
  }
  // Keep only dependencies of the requested outputs. An unrelated failed
  // group must not add its helpers to a four-input local repair.
  set<int> needed = want;
  for (auto i = candidates.rbegin(); i != candidates.rend(); ++i) {
    bool used = any_of(i->targets.begin(), i->targets.end(),
                       [&](int id) { return needed.count(id); });
    if (!used) {
      continue;
    }
    plan->steps.push_back(*i);
    for (int id : i->targets) {
      needed.erase(id);
    }
    needed.insert(i->helpers.begin(), i->helpers.end());
  }
  reverse(plan->steps.begin(), plan->steps.end());
  plan->inputs = std::move(needed);
  return contains(available, plan->inputs) ? 0 : -EIO;
}

int ErasureCodeTsxLrc::_minimum_to_decode(const set<int>& want,
                                         const set<int>& available,
                                         set<int>* minimum)
{
  minimum->clear();
  RepairPlan plan;
  int r = make_repair_plan(want, available, &plan);
  if (!r) {
    *minimum = std::move(plan.inputs);
  }
  return r;
}

int ErasureCodeTsxLrc::encode(const set<int>& want, const bufferlist& in,
                             map<int, bufferlist>* encoded)
{
  encoded->clear();
  if (layers.size() != 3 || !valid_ids(want) || in.length() == 0) {
    return -EINVAL;
  }
  map<int, bufferlist> work;
  int r = encode_prepare(in, work);
  if (!r) {
    r = encode_chunks(want, &work);
  }
  if (r) {
    return r; // Ceph18's base encode discards this error; propagate it here.
  }
  for (int id : want) {
    (*encoded)[id] = work.at(id);
  }
  return 0;
}

int ErasureCodeTsxLrc::encode_chunks(const set<int>& want,
                                    map<int, bufferlist>* encoded)
{
  if (layers.size() != 3 || !valid_ids(want) || encoded->size() != 12) {
    return -EINVAL;
  }
  const auto size = encoded->begin()->second.length();
  for (const auto& chunk : *encoded) {
    if (size == 0 || chunk.second.length() != size || !chunk.second.is_contiguous()) {
      return -EINVAL;
    }
  }
  // Forward order, with the same physical-to-layer remapping as official LRC.
  for (const auto& layer : layers) {
    map<int, bufferlist> layer_encoded;
    set<int> layer_want;
    for (unsigned int j = 0; j < layer.chunks.size(); ++j) {
      auto chunk = encoded->find(layer.chunks[j]);
      if (chunk == encoded->end() || chunk->second.length() == 0) {
        return -EINVAL;
      }
      layer_encoded[j] = chunk->second;
      layer_want.insert(j);
    }
    int r = layer.erasure_code->encode_chunks(layer_want, &layer_encoded);
    if (r) {
      return r;
    }
    for (unsigned int j = 0; j < layer.chunks.size(); ++j) {
      (*encoded)[layer.chunks[j]] = std::move(layer_encoded[j]);
    }
  }
  return 0;
}

int ErasureCodeTsxLrc::decode(const set<int>& want,
                             const map<int, bufferlist>& chunks,
                             map<int, bufferlist>* decoded, int chunk_size)
{
  decoded->clear();
  if (layers.size() != 3 || !valid_ids(want) || !valid_ids(ids_of(chunks))) {
    return -EINVAL;
  }
  if (want.empty()) {
    return 0;
  }
  if (chunks.empty() || chunks.begin()->second.length() == 0) {
    return -EINVAL;
  }
  const auto size = chunks.begin()->second.length();
  for (const auto& chunk : chunks) {
    if (chunk.second.length() != size) {
      return -EINVAL;
    }
  }
  map<int, bufferlist> work;
  int r = ErasureCode::_decode(want, chunks, &work);
  if (r) {
    return r;
  }
  // Base _decode allocates all absent IDs, including unread healthy shards.
  // Publish requested outputs only, never those uninitialised spare buffers.
  for (int id : want) {
    (*decoded)[id] = work.at(id);
  }
  return 0;
}

int ErasureCodeTsxLrc::decode_chunks(const set<int>& want,
                                    const map<int, bufferlist>& chunks,
                                    map<int, bufferlist>* decoded)
{
  RepairPlan plan;
  int r = make_repair_plan(want, ids_of(chunks), &plan);
  if (r) {
    return r;
  }
  for (int id = 0; id < 12; ++id) {
    auto chunk = decoded->find(id);
    if (chunk == decoded->end() || chunk->second.length() == 0) {
      return -EINVAL; // Low-level entry requires Ceph's prepared work buffers.
    }
  }
  for (const auto& step : plan.steps) {
    const auto& layer = layers[step.layer];
    map<int, bufferlist> layer_chunks, layer_decoded;
    set<int> layer_want;
    for (unsigned int j = 0; j < layer.chunks.size(); ++j) {
      int id = layer.chunks[j];
      layer_decoded[j] = decoded->at(id);
      if (step.helpers.count(id)) {
        layer_chunks[j] = decoded->at(id);
      }
      if (step.targets.count(id)) {
        layer_want.insert(j);
      }
    }
    r = layer.erasure_code->decode_chunks(layer_want, layer_chunks, &layer_decoded);
    if (r) {
      return r;
    }
    for (unsigned int j = 0; j < layer.chunks.size(); ++j) {
      (*decoded)[layer.chunks[j]] = layer_decoded[j];
    }
  }
  return 0;
}
