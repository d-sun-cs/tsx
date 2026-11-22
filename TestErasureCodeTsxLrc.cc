// SPDX-License-Identifier: LGPL-2.1-or-later
// Standalone plugin-contract test: load the installed .so through the registry.
#include <algorithm>
#include <cerrno>
#include <iostream>
#include <memory>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <hiredis/hiredis.h>
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
  for (int id : want) {
    check(decoded.count(id) && decoded.at(id).contents_equal(all.at(id)),
          "recovered bytes differ for shard " + std::to_string(id));
  }
  // The Ceph interface permits additional valid results, including the
  // helpers and intermediates. Every returned shard must be usable.
  for (const auto& entry : decoded) {
    check(all.count(entry.first) && entry.second.contents_equal(all.at(entry.first)),
          "published an invalid/uninitialised extra shard");
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

static void test_fasttt(const string& directory)
{
  auto code = load(directory, {{"mode", "fasttt"}});
  check(code->get_profile().at("mode") == "fasttt", "mode identity lost");
  unsigned recovered = 0;
  for (unsigned length : {0, 257, 8 * 17 * 32, 8 * 4096, 8 * 65536}) {
    const unsigned size = code->get_chunk_size(length);
    const unsigned expected_size = 32 * max(1u, (length + 255) / 256);
    check(size == expected_size, "incorrect FastTT data alignment");
    mt19937 rng(29 + length);
    string bytes(length, '\0');
    for (char& byte : bytes) {
      byte = static_cast<char>(rng() & 0xff);
    }
    bufferlist raw;
    raw.append(bytes);
    Chunks all;
    check(code->encode(all_ids(), raw, &all) == 0 && all.size() == 12,
          "FastTT encode did not produce twelve shards");
    bytes.resize(8 * size, '\0');
    vector<string> original;
    for (int id : all_ids()) {
      original.push_back(all.at(id).to_str());
    }
    for (int id = 0; id < 8; ++id) {
      check(original[id] == bytes.substr(id * size, size), "FastTT data layout changed");
    }
    // Compare all parity bytes, including both tails, using the raw input
    // as the reference rather than trusting the plugin's data chunks.
    for (int parity = 8; parity < 12; ++parity) {
      string expected(size + (parity < 10 ? 224 : 0), '\0');
      for (unsigned pos = 0; pos < expected.size(); ++pos) {
        for (int d = 0; d < 8; ++d) {
          if (parity >= 10 && d / 4 != parity - 10) {
            continue;
          }
          const unsigned shift = parity == 8 ? d * 32 : (parity == 9 ? (7 - d) * 32 : 0);
          if (pos >= shift && pos - shift < size) {
            expected[pos] ^= bytes[d * size + pos - shift];
          }
        }
      }
      check(original[parity] == expected, "FastTT parity or tail differs at shard " + to_string(parity));
    }
    repair(code, all, {2,11}, {2}, true, {0,1,3,10});
    repair(code, all, {0,1}, {0,1}, true, {2,3,4,5,6,7,8,10});
    repair(code, all, {0,1,8}, {0,1,8}, true, {2,3,4,5,6,7,9,10});
    repair(code, all, {0,1,2}, {0,1,2}, true, {3,4,5,6,7,8,9,10});
    repair(code, all, {0,1,8,9}, {0,1}, false);
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
      try {
        repair(code, all, erased, erased, true);
      } catch (const exception& error) {
        throw runtime_error("FastTT input length " + to_string(length) +
                            " erasure mask " + to_string(mask) + ": " + error.what());
      }
      ++recovered;
    }
    Chunks selected = all;
    selected.erase(0);
    selected.erase(1);
    selected.erase(9); // Force the G+ and L0 reconstruction path.
    bufferlist concatenated;
    check(code->decode_concat(selected, &concatenated) == 0 &&
          concatenated.to_str() == bytes, "FastTT decode_concat returned wrong padded data");

    bufferlist truncated;
    truncated.substr_of(all.at(8), 0, size);
    selected[8] = std::move(truncated);
    Chunks decoded;
    check(code->decode({0,1}, selected, &decoded, 0) == -EINVAL && decoded.empty(),
          "FastTT accepted a truncated global shard");

    string bad = original[8];
    bad.back() ^= 1;
    selected[8].clear();
    selected[8].append(bad);
    check(code->decode({0,1}, selected, &decoded, 0) == -EIO && decoded.empty(),
          "FastTT accepted inconsistent shift equations");

    // With no local equation and only G+ available, the single-equation
    // path must reject a nonzero symbol outside the missing shard's span.
    selected[1] = all.at(1);
    selected.erase(10);
    check(code->decode({0}, selected, &decoded, 0) == -EIO && decoded.empty(),
          "FastTT single-equation path accepted an inconsistent tail");
    selected = all;
    selected.erase(7);
    selected.erase(9);
    selected.erase(11);
    bad = original[8];
    bad.front() ^= 1;
    selected[8].clear();
    selected[8].append(bad);
    check(code->decode({7}, selected, &decoded, 0) == -EIO && decoded.empty(),
          "FastTT single-equation path accepted an inconsistent prefix");
    for (int id : all_ids()) {
      check(all.at(id).to_str() == original[id], "FastTT decoder modified an input shard");
    }
  }
  check(recovered == 5 * 298, "unexpected FastTT recovery coverage");
  cout << "PASS tsx_lrc fasttt: 1490 repairs across 5 input lengths, reference G+/G- and XOR, "
       << "joint repair, tail rejection, decode_concat and immutable inputs" << endl;
}

static void test_fasttt_redis(const string& directory, const string& port)
{
  auto code = load(directory, {{"mode", "fasttt-redis"}, {"redis-port", port}});
  auto reference = load(directory, {{"mode", "fasttt"}});
  unique_ptr<redisContext, decltype(&redisFree)> redis(
    redisConnectWithTimeout("127.0.0.1", stoi(port), {1, 0}), redisFree);
  check(redis && !redis->err && redisSetTimeout(redis.get(), {1, 0}) == REDIS_OK,
        "could not connect to the Redis test instance");
  using Reply = unique_ptr<redisReply, decltype(&freeReplyObject)>;
  auto key_for = [](int id, const string& shard) {
    constexpr char hex[] = "0123456789abcdef";
    string key = "fasttt:lrc:tail:v1:" + to_string(id) + ":";
    for (unsigned char byte : shard.substr(shard.size() - 32)) {
      key += hex[byte >> 4];
      key += hex[byte & 15];
    }
    return key;
  };
  auto store_value = [&](const string& key, const string& value) {
    Reply reply(static_cast<redisReply*>(redisCommand(redis.get(), "SET %s %b", key.c_str(),
                                                     value.data(), value.size())), freeReplyObject);
    check(reply && reply->type == REDIS_REPLY_STATUS, "could not write Redis test fixture");
  };

  constexpr unsigned size = 64;
  mt19937 rng(91);
  string bytes(8 * size, '\0');
  for (char& byte : bytes) {
    byte = static_cast<char>(rng() & 0xff);
  }
  bufferlist raw;
  raw.append(bytes);
  Chunks packed, inline_shards;
  check(code->encode(all_ids(), raw, &packed) == 0 && packed.size() == 12,
        "Redis encode did not produce all shards");
  check(reference->encode(all_ids(), raw, &inline_shards) == 0, "inline reference encode failed");
  string saved;
  for (int id : all_ids()) {
    const auto shard = packed.at(id).to_str();
    const auto full = inline_shards.at(id).to_str();
    check(shard.size() == size, "Redis mode produced unequal shards");
    if (id == 8 || id == 9) {
      check(shard.substr(0, size - 32) == full.substr(0, size - 32), "global prefix differs");
      const auto key = key_for(id, shard);
      Reply reply(static_cast<redisReply*>(redisCommand(redis.get(), "GET %s", key.c_str())),
                  freeReplyObject);
      check(reply && reply->type == REDIS_REPLY_STRING, "global metadata not found in Redis");
      const string value(reply->str, reply->len);
      check(value == full.substr(size - 32), "Redis metadata differs from inline parity");
      if (id == 8) {
        saved = value;
      }
    } else {
      check(shard == full, "data or local parity changed in Redis mode");
    }
  }

  // Same main G+ bytes, different tail: both encodes must remain recoverable.
  bytes.back() ^= 1;
  bufferlist second_raw;
  second_raw.append(bytes);
  Chunks second, second_inline;
  check(code->encode(all_ids(), second_raw, &second) == 0 &&
        reference->encode(all_ids(), second_raw, &second_inline) == 0, "second encode failed");
  check(second_inline.at(8).to_str().substr(0, size) == inline_shards.at(8).to_str().substr(0, size) &&
        !second.at(8).contents_equal(packed.at(8)), "distinct tails did not get distinct locators");
  const set<int> helpers{2,3,4,5,6,7,8,10};
  repair(code, packed, {0,1}, {0,1}, true, helpers);
  repair(code, second, {0,1}, {0,1}, true, helpers);

  const auto key = key_for(8, packed.at(8).to_str());
  Reply removed(static_cast<redisReply*>(redisCommand(redis.get(), "DEL %s", key.c_str())),
                freeReplyObject);
  check(removed && removed->type == REDIS_REPLY_INTEGER && removed->integer == 1,
        "could not remove Redis fixture");
  Chunks selected, decoded;
  for (int id : helpers) {
    selected[id] = packed.at(id);
  }
  check(code->decode({0,1}, selected, &decoded, 0) == -ENOENT && decoded.empty(),
        "missing tail did not fail joint repair");
  repair(code, packed, {2}, {2}, true, {0,1,3,10});
  repair(code, packed, {8}, {8}, true, {0,1,2,3,4,5,6,7});
  repair(code, packed, {0,1}, {0,1}, true, helpers);
  store_value(key, "short");
  check(code->decode({0,1}, selected, &decoded, 0) == -EIO && decoded.empty(),
        "short tail did not fail joint repair");
  string corrupt = saved;
  corrupt.back() ^= 1;
  store_value(key, corrupt);
  check(code->decode({0,1}, selected, &decoded, 0) == -EIO && decoded.empty(),
        "corrupt tail passed the digest check");
  store_value(key, saved);
  repair(code, packed, {0,1}, {0,1}, true, helpers);
  cout << "PASS tsx_lrc fasttt-redis: equal shard sizes, isolated locators, Redis metadata, "
       << "missing/corrupt tails, local repair and metadata regeneration" << endl;
}

int main(int argc, char** argv)
{
  vector<const char*> args(argv + 1, argv + argc);
  const bool check_daemon = !args.empty() && string_view(args.front()) == "--check-daemon-profile";
  constexpr string_view redis_arg = "--check-redis=";
  const bool check_redis = !args.empty() && string_view(args.front()).starts_with(redis_arg);
  const string port = check_redis ? string(args.front() + redis_arg.size()) : string();
  if (check_daemon || check_redis) {
    args.erase(args.begin());
  }
  auto cct = global_init(nullptr, args, CEPH_ENTITY_TYPE_CLIENT,
                         check_daemon ? CODE_ENVIRONMENT_DAEMON : CODE_ENVIRONMENT_UTILITY,
                         CINIT_FLAG_NO_DEFAULT_CONFIG_FILE | CINIT_FLAG_NO_DAEMON_ACTIONS);
  common_init_finish(g_ceph_context);
  g_ceph_context->_conf.apply_changes(nullptr);
  try {
    const string directory = g_conf().get_val<string>("erasure_code_dir");
    if (check_daemon) {
      load(directory, {{"mode", "fasttt"}}, -EOPNOTSUPP);
      load(directory, {{"mode", "fasttt-redis"}}, -EOPNOTSUPP);
      load(directory, {{"mode", "scaffold"}});
      cout << "PASS tsx_lrc daemon profile: experimental FastTT format rejected, scaffold accepted" << endl;
      return 0;
    }
    if (check_redis) {
      test_fasttt_redis(directory, port);
      return 0;
    }
    load(directory, {}, -EINVAL);
    load(directory, {{"mode","unknown"}}, -EINVAL);
    load(directory, {{"mode","fasttt-redis"}, {"redis-port","65536"}}, -EINVAL);
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
      Chunks local_inputs, local_outputs;
      for (int id : {0,1,3,10}) {
        local_inputs[id] = all.at(id);
      }
      check(code->decode({2}, local_inputs, &local_outputs, 0) == 0,
            "inherited decode failed for local repair");
      check(local_outputs.size() == 5 && !local_outputs.count(4) &&
            local_outputs.at(2).contents_equal(all.at(2)),
            "local repair retained unfilled shards or discarded valid helpers");
      const set<int> data_ids{0,1,2,3,4,5,6,7};
      Ranges minimum;
      set<int> available = all_ids();
      available.erase(2);
      check(code->minimum_to_decode(data_ids, available, &minimum) == 0,
            "full-data recovery plan failed");
      Chunks concat_inputs;
      for (const auto& entry : minimum) {
        concat_inputs[entry.first] = all.at(entry.first);
      }
      bufferlist concatenated;
      check(code->decode_concat(concat_inputs, &concatenated) == 0 &&
            concatenated.contents_equal(raw),
            "inherited decode_concat did not reconstruct the logical input");
      Chunks parity_only;
      check(code->encode({8,9,10,11}, raw, &parity_only) == 0 &&
            parity_only.size() == 4, "inherited encode did not filter outputs");
      for (const auto& entry : parity_only) {
        check(entry.second.contents_equal(all.at(entry.first)), "parity-only bytes differ");
      }
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
      check(code->decode({12}, all, &decoded, 0) == -EINVAL, "invalid ID accepted");
      check(code->decode({}, {}, &decoded, 0) == 0 && decoded.empty(), "empty want failed");
      Chunks malformed = all;
      malformed.erase(2); // Exercise the kernel, rather than the already-read fast path.
      bufferlist shorter;
      shorter.substr_of(all.at(1), 0, size - 1);
      malformed[1] = shorter;
      check(code->decode({2}, malformed, &decoded, 0) == -EINVAL, "unequal lengths accepted");
      Chunks unprepared;
      check(code->encode_chunks(all_ids(), &unprepared) == -EINVAL,
            "low-level encode accepted unprepared buffers");
    }
    check(recovered == 532 && rejected == 64, "unexpected scaffold recovery coverage");
    cout << "PASS tsx_lrc scaffold: 4KiB/64KiB, 532 repairs + 64 expected planner rejections; "
         << "four-helper local repair, global fallback, global-to-local repair, "
         << "byte comparisons, valid-output pruning, inherited wrappers/decode_concat "
         << "and kernel error paths" << endl;
    test_fasttt(directory);
    return 0;
  } catch (const exception& error) {
    cerr << "FAIL tsx_lrc: " << error.what() << endl;
    return 1;
  }
}
