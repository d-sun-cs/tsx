// SPDX-License-Identifier: LGPL-2.1-or-later
// Standalone plugin-contract test: load the installed .so through the registry.
#include <cerrno>
#include <iostream>
#include <random>
#include <sstream>
#include <stdexcept>
#include "common/ceph_context.h"
#include "common/config.h"
#include "erasure-code/ErasureCodePlugin.h"
#include "global/global_context.h"
#include "global/global_init.h"

using namespace std;
using namespace ceph;
using Chunks = map<int, bufferlist>;
using Ranges = map<int, vector<pair<int, int>>>;

static void check(bool ok, const string& message)
{
  if (!ok) {
    throw runtime_error(message);
  }
}

static set<int> all_ids()
{
  return {0,1,2,3,4,5,6,7,8,9,10,11};
}

static ErasureCodeInterfaceRef load(const string& directory,
                                   ErasureCodeProfile profile, int expected = 0)
{
  ErasureCodeInterfaceRef code;
  ostringstream errors;
  int r = ErasureCodePluginRegistry::instance().factory(
    "tsx_lrc", directory, profile, &code, &errors);
  check(r == expected, "factory: " + std::to_string(r) + " " + errors.str());
  check(bool(code) == (expected == 0), "factory published an invalid object");
  return code;
}

static void repair(ErasureCodeInterfaceRef code, const Chunks& all,
                   const set<int>& erased, const set<int>& want,
                   bool supported, const set<int>& expected_helpers = {})
{
  set<int> available = all_ids();
  Chunks healthy;
  for (int id : erased) {
    available.erase(id);
  }
  for (int id : available) {
    healthy[id] = all.at(id);
  }
  Ranges minimum;
  int r = code->minimum_to_decode(want, available, &minimum);
  Chunks decoded;
  if (!supported) {
    check(r == -EIO, "unsupported plan must return -EIO");
    decoded[99].append("sentinel");
    check(code->decode(want, healthy, &decoded, 0) == -EIO,
          "unsupported decode must return -EIO");
    check(decoded.empty(), "failed decode published output");
    return;
  }
  check(r == 0, "supported recovery plan failed");
  Chunks selected;
  set<int> helpers;
  for (const auto& entry : minimum) {
    check(available.count(entry.first), "planner selected an unavailable shard");
    check(entry.second == vector<pair<int, int>>{{0,1}}, "invalid subchunk range");
    helpers.insert(entry.first);
    selected[entry.first] = all.at(entry.first);
  }
  if (!expected_helpers.empty()) {
    check(helpers == expected_helpers, "unexpected local/global helper set");
  }
  // Deliberately pass ONLY planned inputs, not the full healthy codeword.
  r = code->decode(want, selected, &decoded, 0);
  check(r == 0, "decode failed with its own planned inputs");
  check(decoded.size() == want.size(), "published unread/uninitialised spare shards");
  for (int id : want) {
    check(decoded.count(id) && decoded.at(id).contents_equal(all.at(id)),
          "recovered bytes differ for shard " + std::to_string(id));
  }
}

static bool unsupported_triple(const set<int>& erased)
{
  vector<int> data;
  unsigned globals = 0;
  for (int id : erased) {
    if (id < 8) {
      data.push_back(id);
    } else if (id < 10) {
      ++globals;
    }
  }
  // A layer-by-layer two-parity scaffold cannot solve these jointly.
  // This is a planner boundary, not a claim of mathematical unsolvability.
  return erased.size() == 3 && !data.empty() &&
    data.front()/4 == data.back()/4 &&
    (data.size() == 3 || (data.size() == 2 && globals == 1));
}

int main(int argc, char** argv)
{
  vector<const char*> args(argv + 1, argv + argc);
  auto cct = global_init(nullptr, args, CEPH_ENTITY_TYPE_CLIENT,
                         CODE_ENVIRONMENT_UTILITY, CINIT_FLAG_NO_DEFAULT_CONFIG_FILE);
  common_init_finish(g_ceph_context);
  g_ceph_context->_conf.apply_changes(nullptr);
  try {
    const string directory = g_conf().get_val<string>("erasure_code_dir");
    load(directory, {}, -EINVAL);
    load(directory, {{"mode","fasttt"}}, -EOPNOTSUPP);
    for (const auto& bad : vector<pair<string,string>>{
           {"k","6"}, {"m","4"}, {"locality","3"},
           {"l","4"}, {"mapping","DD_"}, {"layers","[]"}}) {
      load(directory, {{"mode","scaffold"}, bad}, -EINVAL);
    }
    auto code = load(directory, {{"mode","scaffold"}});
    check(code->get_data_chunk_count() == 8 && code->get_chunk_count() == 12 &&
          code->get_coding_chunk_count() == 4, "incorrect 8+2+2 layout");
    check(code->get_profile().at("mode") == "scaffold", "mode identity lost");
    unsigned recovered = 0, rejected = 0;
    for (unsigned size : {4096, 65536}) {
      mt19937 rng(17 + size);
      string bytes(8 * size, '\0');
      for (char& byte : bytes) {
        byte = static_cast<char>(rng() & 0xff);
      }
      bufferlist raw;
      raw.append(bytes);
      Chunks all;
      check(code->encode(all_ids(), raw, &all) == 0 && all.size() == 12,
            "encode did not produce twelve shards");
      vector<string> original;
      for (int id : all_ids()) {
        original.push_back(all.at(id).to_str());
        check(all.at(id).length() == size, "unexpected scaffold shard size");
      }
      for (int id = 0; id < 8; ++id) {
        check(original[id] == bytes.substr(id * size, size), "data mapping changed");
      }
      for (int group = 0; group < 2; ++group) {
        string parity(size, '\0');
        for (int d = group * 4; d < group * 4 + 4; ++d) {
          for (unsigned b = 0; b < size; ++b) {
            parity[b] ^= original[d][b];
          }
        }
        check(parity == original[10 + group], "local parity is not four-data XOR");
      }
      repair(code, all, {}, {2}, true, {2});
      repair(code, all, {2}, {2}, true, {0,1,3,10});
      repair(code, all, {2,11}, {2}, true, {0,1,3,10});
      repair(code, all, {10}, {10}, true, {0,1,2,3});
      repair(code, all, {0,1}, {0,1}, true, {2,3,4,5,6,7,8,9});
      repair(code, all, {0,10}, {0,10}, true); // global, then local again
      for (unsigned mask = 1; mask < (1u << 12); ++mask) {
        if (__builtin_popcount(mask) > 3) {
          continue;
        }
        set<int> erased;
        for (int id = 0; id < 12; ++id) {
          if (mask & (1u << id)) {
            erased.insert(id);
          }
        }
        bool supported = !unsupported_triple(erased);
        repair(code, all, erased, erased, supported);
        supported ? ++recovered : ++rejected;
      }
      repair(code, all, {0,1,8,9}, {0,1}, false);
      for (int id : all_ids()) {
        check(all.at(id).to_str() == original[id], "decoder modified an input shard");
      }
      Chunks decoded;
      check(code->decode({2}, {}, &decoded, 0) == -EINVAL, "empty input accepted");
      check(code->decode({12}, all, &decoded, 0) == -EINVAL, "invalid ID accepted");
      check(code->decode({}, {}, &decoded, 0) == 0 && decoded.empty(), "empty want failed");
      Chunks malformed = all;
      bufferlist shorter;
      shorter.substr_of(all.at(1), 0, size - 1);
      malformed[1] = shorter;
      check(code->decode({2}, malformed, &decoded, 0) == -EINVAL, "unequal lengths accepted");
      bufferlist empty;
      check(code->encode(all_ids(), empty, &decoded) == -EINVAL, "empty encode accepted");
    }
    check(recovered == 532 && rejected == 64, "unexpected scaffold recovery coverage");
    cout << "PASS tsx_lrc scaffold: 4KiB/64KiB, 532 repairs + 64 expected planner rejections; "
         << "four-helper local repair, global fallback, global-to-local repair, "
         << "byte comparisons, output filtering and error paths" << endl;
    return 0;
  } catch (const exception& error) {
    cerr << "FAIL tsx_lrc: " << error.what() << endl;
    return 1;
  }
}
