// SPDX-License-Identifier: LGPL-2.1-or-later
#include "ErasureCodeTsxMt.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <iterator>
#include <memory>
#include <hiredis/hiredis.h>
#include <immintrin.h>
#include <omp.h>

#include "common/code_environment.h"
#include "include/buffer.h"

using namespace std;
using namespace ceph;

namespace {
constexpr int data_count = 8;
constexpr int parity_count = 3;
constexpr int first_parity = data_count;
constexpr int xor_parity = first_parity + 1;
constexpr int shard_count = data_count + parity_count;
constexpr size_t symbol_bytes = sizeof(__m256i);
constexpr size_t tail_symbols = data_count - 1;
constexpr size_t tail_bytes = tail_symbols * symbol_bytes;
using Chunks = map<int, bufferlist>;
using Inputs = array<const char*, data_count>;
using Tails = array<char, 2 * tail_bytes>;

bool valid_ids(const set<int>& ids)
{
  return ids.empty() || (*ids.begin() >= 0 && *ids.rbegin() < shard_count);
}

bool valid_shard(const bufferlist& shard, size_t bytes)
{
  return shard.length() == bytes && shard.is_contiguous();
}

__m256i load_symbol(const char* data)
{
  return _mm256_loadu_si256(reinterpret_cast<const __m256i*>(data));
}

void store_symbol(char* data, __m256i value)
{
  _mm256_storeu_si256(reinterpret_cast<__m256i*>(data), value);
}

// Each destination symbol is reduced in registers and stored once. The
// inputs already point to the first symbol of their contribution.
void xor_shards(const Inputs& input, size_t count, char* output)
{
  constexpr size_t unroll = 4;
  size_t t = 0;
  for (; t + unroll <= count; t += unroll) {
    __m256i sum[unroll]{};
    for (const char* shard : input) {
      #pragma GCC unroll 4
      for (size_t j = 0; j < unroll; ++j) {
        sum[j] = _mm256_xor_si256(sum[j], load_symbol(shard + (t + j) * symbol_bytes));
      }
    }
    #pragma GCC unroll 4
    for (size_t j = 0; j < unroll; ++j) {
      store_symbol(output + (t + j) * symbol_bytes, sum[j]);
    }
  }
  for (; t < count; ++t) {
    auto sum = _mm256_setzero_si256();
    for (const char* shard : input) {
      sum = _mm256_xor_si256(sum, load_symbol(shard + t * symbol_bytes));
    }
    store_symbol(output + t * symbol_bytes, sum);
  }
}

void write_parity_range(const Inputs& input, int parity, size_t symbols,
                        size_t begin, size_t end, char* output)
{
  if (parity == xor_parity) {
    Inputs start = input;
    for (auto& shard : start) {
      shard += begin * symbol_bytes;
    }
    xor_shards(start, end - begin, output);
    return;
  }
  array<size_t, data_count> shift;
  for (size_t i = 0; i < shift.size(); ++i) {
    shift[i] = parity == first_parity ? i : tail_symbols - i;
  }
  const auto edge = [&](size_t first, size_t last) {
    for (size_t t = first; t < last; ++t) {
      auto sum = _mm256_setzero_si256();
      for (size_t i = 0; i < input.size(); ++i) {
        if (t >= shift[i] && t - shift[i] < symbols) {
          sum = _mm256_xor_si256(sum, load_symbol(input[i] + (t - shift[i]) * symbol_bytes));
        }
      }
      store_symbol(output + (t - begin) * symbol_bytes, sum);
    }
  };
  const size_t middle = clamp(tail_symbols, begin, end);
  const size_t middle_end = clamp(symbols, middle, end);
  edge(begin, middle);
  if (middle < middle_end) {
    Inputs start;
    for (size_t i = 0; i < input.size(); ++i) {
      start[i] = input[i] + (middle - shift[i]) * symbol_bytes;
    }
    xor_shards(start, middle_end - middle, output + (middle - begin) * symbol_bytes);
  }
  edge(middle_end, end);
}

int store_tails(int port, const Inputs& input, const Tails& tails)
{
  // Match ordinary TSX's 32-byte P0 key and concatenated G+/G- values.
  array<char, symbol_bytes> key;
  xor_shards(input, 1, key.data());
  constexpr timeval timeout{1, 0};
  unique_ptr<redisContext, decltype(&redisFree)> connection(
    redisConnectWithTimeout("127.0.0.1", port, timeout), redisFree);
  if (!connection || connection->err || redisSetTimeout(connection.get(), timeout) != REDIS_OK) {
    return -EIO;
  }
  unique_ptr<redisReply, decltype(&freeReplyObject)> reply(
    static_cast<redisReply*>(redisCommand(connection.get(), "SET %b %b",
                                        key.data(), key.size(), tails.data(), tails.size())),
    freeReplyObject);
  return reply && reply->type == REDIS_REPLY_STATUS && reply->len == 2 &&
         memcmp(reply->str, "OK", 2) == 0 ? 0 : -EIO;
}
}

int ErasureCodeTsxMt::init(ErasureCodeProfile& profile, ostream* ss)
{
  if (g_code_env != CODE_ENVIRONMENT_UTILITY && g_code_env != CODE_ENVIRONMENT_UTILITY_NODOUT) {
    *ss << "tsx_mt is an in-memory experiment";
    return -EOPNOTSUPP;
  }
  int k, m;
  int r = to_int("k", profile, &k, "8", ss);
  r |= to_int("m", profile, &m, "3", ss);
  r |= to_int("threads", profile, &threads, "4", ss);
  r |= to_int("redis-port", profile, &redis_port, "6379", ss);
  if (r) {
    return r;
  }
  const auto mapping = profile.find("mapping");
  if (k != data_count || m != parity_count || threads < 1 || threads > max_threads ||
      redis_port < 1 || redis_port > 65535 ||
      (mapping != profile.end() && !mapping->second.empty())) {
    *ss << "tsx_mt requires k=8 m=3, default chunk order, threads=1.."
        << max_threads << " and redis-port=1..65535";
    return -EINVAL;
  }
  r = ErasureCode::init(profile, ss);
  if (!r) {
    slots.release(threads);
  }
  return r;
}

unsigned int ErasureCodeTsxMt::get_chunk_size(unsigned int object_size) const
{
  constexpr auto alignment = data_count * symbol_bytes;
  const size_t symbols = (size_t{object_size} + alignment - 1) / alignment;
  return max(size_t{1}, symbols) * symbol_bytes;
}

template<typename Work>
void ErasureCodeTsxMt::for_output_ranges(size_t symbols, Work&& work)
{
  // Internal boundaries are in 64-byte units. Workers only use the
  // caller-prepared pointers, and the implicit barrier protects their lifetime.
  const size_t units = (symbols + 1) / 2;
  const int limit = min(static_cast<size_t>(threads), units);
  slots.acquire();
  int team = 1;
  while (team < limit && slots.try_acquire()) {
    ++team;
  }
  if (team == 1) {
    work(0, symbols, true);
  } else {
    #pragma omp parallel num_threads(team)
    {
      const size_t rank = omp_get_thread_num();
      const size_t count = omp_get_num_threads();
      const size_t begin = (units * rank / count) * 2;
      const size_t end = min(symbols, (units * (rank + 1) / count) * 2);
      work(begin, end, rank + 1 == count);
    }
  }
  slots.release(team);
}

int ErasureCodeTsxMt::write_parities(size_t symbols, const set<int>& want, Chunks& chunks)
{
  Inputs inputs;
  array<char*, parity_count> outputs{};
  for (int i = 0; i < data_count; ++i) {
    inputs[i] = chunks.at(i).c_str();
  }
  for (int id : want) {
    outputs[id - first_parity] = chunks.at(id).c_str();
  }
  const bool save_tails = outputs.front() || outputs.back();
  Tails tails;
  for_output_ranges(symbols, [&](size_t begin, size_t end, bool last) {
    for (int i = 0; i < parity_count; ++i) {
      if (outputs[i]) {
        write_parity_range(inputs, first_parity + i, symbols, begin, end,
                           outputs[i] + begin * symbol_bytes);
      }
    }
    if (last && save_tails) {
      write_parity_range(inputs, first_parity, symbols, symbols, symbols + tail_symbols,
                         tails.data());
      write_parity_range(inputs, shard_count - 1, symbols, symbols, symbols + tail_symbols,
                         tails.data() + tail_bytes);
    }
  });
  return save_tails ? store_tails(redis_port, inputs, tails) : 0;
}

int ErasureCodeTsxMt::encode_chunks(const set<int>& want, Chunks* encoded)
{
  if (!valid_ids(want) || encoded->size() != shard_count) {
    return -EINVAL;
  }
  const size_t bytes = encoded->begin()->second.length();
  if (!bytes || bytes % symbol_bytes) {
    return -EINVAL;
  }
  for (int id = 0; id < shard_count; ++id) {
    auto shard = encoded->find(id);
    if (shard == encoded->end() || !valid_shard(shard->second, bytes)) {
      return -EINVAL;
    }
  }
  return write_parities(bytes / symbol_bytes, {first_parity, xor_parity, shard_count - 1}, *encoded);
}

int ErasureCodeTsxMt::_minimum_to_decode(const set<int>& want, const set<int>& available,
                                        set<int>* minimum)
{
  minimum->clear();
  if (!valid_ids(want) || !valid_ids(available)) {
    return -EINVAL;
  }
  if (includes(available.begin(), available.end(), want.begin(), want.end())) {
    *minimum = want;
    return 0;
  }
  if (available.size() < data_count) {
    return -EIO;
  }
  set<int> helpers;
  for (int id = 0; id < data_count; ++id) {
    if (available.count(id)) {
      helpers.insert(id);
    }
  }
  if (helpers.size() < data_count) {
    if (helpers.size() != data_count - 1 || !available.count(xor_parity)) {
      return -EOPNOTSUPP;
    }
    helpers.insert(xor_parity);
  }
  set_intersection(want.begin(), want.end(), available.begin(), available.end(),
                   inserter(helpers, helpers.end()));
  *minimum = std::move(helpers);
  return 0;
}

int ErasureCodeTsxMt::decode_chunks(const set<int>& want, const Chunks& chunks, Chunks* decoded)
{
  const auto fail = [decoded](int r) {
    decoded->clear();
    return r;
  };
  set<int> available;
  for (const auto& [id, shard] : chunks) {
    available.insert(id);
  }
  set<int> helpers;
  const int r = _minimum_to_decode(want, available, &helpers);
  if (r) {
    return fail(r);
  }
  if (includes(available.begin(), available.end(), want.begin(), want.end())) {
    erase_if(*decoded, [&](const auto& entry) { return !available.count(entry.first); });
    return 0;
  }
  const size_t bytes = chunks.begin()->second.length();
  if (!bytes || bytes % symbol_bytes) {
    return fail(-EINVAL);
  }
  for (int id = 0; id < shard_count; ++id) {
    auto work = decoded->find(id);
    const auto input = chunks.find(id);
    if (work == decoded->end() || work->second.length() != bytes ||
        (input != chunks.end() && input->second.length() != bytes)) {
      return fail(-EINVAL);
    }
  }
  const size_t symbols = bytes / symbol_bytes;
  for (int id = 0; id < data_count; ++id) {
    if (available.count(id)) {
      continue;
    }
    Inputs inputs;
    for (int j = 0; j < data_count; ++j) {
      inputs[j] = decoded->at(j == id ? xor_parity : j).c_str();
    }
    char* output = decoded->at(id).c_str();
    for_output_ranges(symbols, [&](size_t begin, size_t end, bool) {
      Inputs start = inputs;
      for (auto& shard : start) {
        shard += begin * symbol_bytes;
      }
      xor_shards(start, end - begin, output + begin * symbol_bytes);
    });
    available.insert(id);
    break;
  }
  set<int> parities;
  set_difference(want.lower_bound(first_parity), want.end(), available.begin(),
                 available.end(), inserter(parities, parities.end()));
  if (!parities.empty()) {
    const int result = write_parities(symbols, parities, *decoded);
    if (result) {
      return fail(result);
    }
    available.insert(parities.begin(), parities.end());
  }
  erase_if(*decoded, [&](const auto& entry) { return !available.count(entry.first); });
  return 0;
}
