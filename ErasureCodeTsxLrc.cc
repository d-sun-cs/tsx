// SPDX-License-Identifier: LGPL-2.1-or-later
#include "ErasureCodeTsxLrc.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <span>
#include <hiredis/hiredis.h>
#include <immintrin.h>
#include <openssl/sha.h>
#include "common/code_environment.h"
#include "include/buffer.h"
#include "erasure-code/ErasureCodePlugin.h"

using namespace std;
using namespace ceph;

namespace {
constexpr unsigned shift_bytes = 32;
static_assert(shift_bytes == sizeof(__m256i));
constexpr unsigned tail_bytes = 7 * shift_bytes;
constexpr size_t window_bytes = 16 * shift_bytes;
constexpr unsigned locator_bytes = SHA256_DIGEST_LENGTH;
constexpr unsigned redis_value_bytes = tail_bytes + locator_bytes;
static_assert(locator_bytes <= shift_bytes);

bool is_global(int id)
{
  return id == 8 || id == 9;
}

unsigned shard_size(int id, unsigned data_size)
{
  return data_size + (is_global(id) ? tail_bytes : 0);
}

using Digest = array<unsigned char, locator_bytes>;

Digest parity_digest(const char* data, size_t length)
{
  Digest digest;
  SHA256(reinterpret_cast<const unsigned char*>(data), length, digest.data());
  return digest;
}

string tail_key(int id, const unsigned char* digest)
{
  constexpr char hex[] = "0123456789abcdef";
  string key = "fasttt:lrc:tail:v1:" + to_string(id) + ":";
  for (unsigned i = 0; i < locator_bytes; ++i) {
    key += hex[digest[i] >> 4];
    key += hex[digest[i] & 15];
  }
  return key;
}

// One lazy connection per call. A local repair has no Redis operations.
class RedisTails {
  const int port;
  unique_ptr<redisContext, decltype(&redisFree)> connection{nullptr, redisFree};
  using Reply = unique_ptr<redisReply, decltype(&freeReplyObject)>;

  Reply command(const vector<string>& args)
  {
    if (!connection) {
      constexpr timeval timeout{1, 0};
      connection.reset(redisConnectWithTimeout("127.0.0.1", port, timeout));
      if (!connection || connection->err || redisSetTimeout(connection.get(), timeout) != REDIS_OK) {
        return {nullptr, freeReplyObject};
      }
    }
    vector<const char*> argv;
    vector<size_t> lengths;
    for (const auto& arg : args) {
      argv.push_back(arg.data());
      lengths.push_back(arg.size());
    }
    return {static_cast<redisReply*>(
              redisCommandArgv(connection.get(), argv.size(), argv.data(), lengths.data())),
            freeReplyObject};
  }

public:
  explicit RedisTails(int port) : port(port) {}
  bool enabled() const { return port != 0; }

  int load(int id, const char* shard, unsigned size, vector<char>* full)
  {
    const auto prefix = size - locator_bytes;
    const auto* locator = reinterpret_cast<const unsigned char*>(shard + prefix);
    const auto reply = command({"GET", tail_key(id, locator)});
    if (!reply) {
      return -EIO;
    }
    if (reply->type == REDIS_REPLY_NIL) {
      return -ENOENT;
    }
    if (reply->type != REDIS_REPLY_STRING || reply->len != redis_value_bytes) {
      return -EIO;
    }
    full->assign(shard, shard + prefix);
    full->insert(full->end(), reply->str, reply->str + reply->len);
    const auto digest = parity_digest(full->data(), full->size());
    return memcmp(digest.data(), locator, digest.size()) == 0 ? 0 : -EIO;
  }

  int store(const set<int>& ids, unsigned size, map<int, bufferlist>& work)
  {
    if (ids.empty()) {
      return 0;
    }
    const auto prefix = size - locator_bytes;
    vector<string> args{"MSET"};
    map<int, bufferlist> packed;
    for (int id : ids) {
      const char* full = work.at(id).c_str();
      const auto digest = parity_digest(full, shard_size(id, size));
      args.push_back(tail_key(id, digest.data()));
      args.emplace_back(full + prefix, redis_value_bytes);
      bufferptr output(buffer::create_aligned(size, shift_bytes));
      memcpy(output.c_str(), full, prefix);
      memcpy(output.c_str() + prefix, digest.data(), digest.size());
      packed[id].push_back(std::move(output));
    }
    const auto reply = command(args);
    if (!reply || reply->type != REDIS_REPLY_STATUS || strcmp(reply->str, "OK") != 0) {
      return -EIO;
    }
    for (auto& entry : packed) {
      work.at(entry.first) = std::move(entry.second);
    }
    return 0;
  }
};

// Exponents are in 32-byte units. -1 means the data shard is absent
// from this equation, whereas 0 means it participates without a shift.
int parity_shift(int parity, int data)
{
  if (parity == 8) {
    return data;
  }
  if (parity == 9) {
    return 7 - data;
  }
  return data / 4 == parity - 10 ? 0 : -1;
}

// All FastTT shifts and ranges are multiples of one symbol. Redis and
// equation scratch buffers need not be 32-byte aligned.
__m256i load_symbol(const char* data)
{
  return _mm256_loadu_si256(reinterpret_cast<const __m256i*>(data));
}

void store_symbol(char* data, __m256i value)
{
  _mm256_storeu_si256(reinterpret_cast<__m256i*>(data), value);
}

void xor_symbol(char* output, __m256i value)
{
  store_symbol(output, _mm256_xor_si256(load_symbol(output), value));
}

void xor_bytes(char* out, const char* in, size_t size)
{
  for (size_t i = 0; i < size; i += shift_bytes) {
    xor_symbol(out + i, load_symbol(in + i));
  }
}

void xor_window(char* out, size_t begin, size_t end,
                const char* input, size_t offset, size_t length)
{
  const size_t first = max(begin, offset);
  const size_t last = min(end, offset + length);
  if (first < last) {
    xor_bytes(out + (first - begin), input + (first - offset), last - first);
  }
}

void write_local(int target, const vector<int>& group, unsigned size,
                 map<int, bufferlist>& work)
{
  char* output = work.at(target).c_str();
  array<const char*, 4> inputs;
  unsigned index = 0;
  for (int id : group) {
    if (id != target) {
      inputs[index++] = work.at(id).c_str();
    }
  }
  #pragma GCC unroll 4
  for (size_t i = 0; i < size; i += shift_bytes) {
    const auto left = _mm256_xor_si256(load_symbol(inputs[0] + i), load_symbol(inputs[1] + i));
    const auto right = _mm256_xor_si256(load_symbol(inputs[2] + i), load_symbol(inputs[3] + i));
    store_symbol(output + i, _mm256_xor_si256(left, right));
  }
}

char* prepare_global(int id, unsigned size, map<int, bufferlist>& work)
{
  auto& output = work.at(id);
  const auto length = shard_size(id, size);
  if (output.length() != length) {
    output.clear();
    output.push_back(buffer::create_aligned(length, shift_bytes));
  }
  return output.c_str();
}

struct ShiftedInput {
  const char* data;
  size_t offset;
};

template<typename Store>
void visit_shifted_sum(span<const ShiftedInput> inputs, size_t size,
                       size_t length, Store&& store)
{
  size_t interior_begin = 0;
  for (const auto& input : inputs) {
    interior_begin = max(interior_begin, input.offset);
  }
  const auto visit_edge = [&](size_t begin, size_t end) {
    for (size_t i = begin; i < end; i += shift_bytes) {
      auto sum = _mm256_setzero_si256();
      for (const auto& input : inputs) {
        if (i >= input.offset && i - input.offset < size) {
          sum = _mm256_xor_si256(sum, load_symbol(input.data + (i - input.offset)));
        }
      }
      store(i, sum);
    }
  };
  // Every input contributes between the last start and the first end.
  visit_edge(0, interior_begin);
  for (size_t i = interior_begin; i < size; i += shift_bytes) {
    auto sum = _mm256_setzero_si256();
    for (const auto& input : inputs) {
      sum = _mm256_xor_si256(sum, load_symbol(input.data + (i - input.offset)));
    }
    store(i, sum);
  }
  visit_edge(max(interior_begin, size), length);
}

void write_global(int id, unsigned size, map<int, bufferlist>& work)
{
  char* output = prepare_global(id, size, work);
  array<ShiftedInput, 8> inputs;
  for (unsigned data = 0; data < inputs.size(); ++data) {
    inputs[data] = {work.at(data).c_str(), size_t(parity_shift(id, data)) * shift_bytes};
  }
  visit_shifted_sum(inputs, size, shard_size(id, size),
                    [output](size_t i, __m256i value) { store_symbol(output + i, value); });
}

void write_globals(unsigned size, map<int, bufferlist>& work)
{
  char* forward = prepare_global(8, size, work);
  char* reverse = prepare_global(9, size, work);
  array<const char*, 8> inputs;
  for (unsigned data = 0; data < inputs.size(); ++data) {
    inputs[data] = work.at(data).c_str();
  }
  memset(forward, 0, tail_bytes);
  memset(reverse, 0, tail_bytes);
  for (size_t begin = 0; begin < size; begin += window_bytes) {
    const size_t length = min(window_bytes, size - begin);
    // Earlier windows may have written into the first seven symbols.
    // Initialize only the newly reachable part of each parity.
    memset(forward + begin + tail_bytes, 0, length);
    memset(reverse + begin + tail_bytes, 0, length);
    for (unsigned data = 0; data < inputs.size(); data += 2) {
      char* plus = forward + begin + parity_shift(8, data) * shift_bytes;
      char* minus = reverse + begin + parity_shift(9, data + 1) * shift_bytes;
      const char* even = inputs[data] + begin;
      const char* odd = inputs[data + 1] + begin;
      auto prev_even = _mm256_setzero_si256();
      auto prev_odd = _mm256_setzero_si256();
      // Adjacent shards have shifts one symbol apart. The two parities
      // reuse each pair of loads, in opposite shift order.
      for (size_t i = 0; i < length; i += shift_bytes) {
        const auto current_even = load_symbol(even + i);
        const auto current_odd = load_symbol(odd + i);
        xor_symbol(plus + i, _mm256_xor_si256(current_even, prev_odd));
        xor_symbol(minus + i, _mm256_xor_si256(current_odd, prev_even));
        prev_even = current_even;
        prev_odd = current_odd;
      }
      xor_symbol(plus + length, prev_odd);
      xor_symbol(minus + length, prev_even);
    }
  }
}

// A bit at position s represents z^s, where z shifts by one 32-byte unit.
// At most three erased data shards participate, so degrees are at most 21.
using Polynomial = uint32_t;
using Matrix = array<array<Polynomial, 3>, 3>;

unsigned first_term(Polynomial p)
{
  return __builtin_ctz(p);
}

Polynomial multiply(Polynomial a, Polynomial b)
{
  Polynomial product = 0;
  for (; b; b &= b - 1) {
    product ^= a << first_term(b);
  }
  return product;
}

Polynomial determinant(const Matrix& matrix, unsigned count)
{
  array<unsigned, 3> columns{0, 1, 2};
  Polynomial result = 0;
  do {
    Polynomial term = 1;
    for (unsigned row = 0; row < count; ++row) {
      term = multiply(term, matrix[row][columns[row]]);
    }
    // In characteristic two, the permutation signs are all +1.
    result ^= term;
  } while (next_permutation(columns.begin(), columns.begin() + count));
  return result;
}

Matrix make_matrix(const vector<int>& equations, const vector<int>& missing)
{
  Matrix matrix{};
  for (unsigned row = 0; row < equations.size(); ++row) {
    for (unsigned col = 0; col < missing.size(); ++col) {
      const int shift = parity_shift(equations[row], missing[col]);
      if (shift >= 0) {
        matrix[row][col] = Polynomial{1} << shift;
      }
    }
  }
  return matrix;
}

vector<int> choose_equations(const set<int>& known, const vector<int>& missing)
{
  // Try the local equations before a second global, when either suffices.
  constexpr array<int, 4> parity_ids{10, 11, 8, 9};
  unsigned available = 0;
  for (unsigned i = 0; i < parity_ids.size(); ++i) {
    if (known.count(parity_ids[i])) {
      available |= 1u << i;
    }
  }
  for (unsigned mask = 1; mask < (1u << parity_ids.size()); ++mask) {
    if ((mask & available) != mask ||
        static_cast<size_t>(__builtin_popcount(mask)) != missing.size()) {
      continue;
    }
    vector<int> equations;
    for (unsigned i = 0; i < parity_ids.size(); ++i) {
      if (mask & (1u << i)) {
        equations.push_back(parity_ids[i]);
      }
    }
    if (determinant(make_matrix(equations, missing), missing.size())) {
      return equations;
    }
  }
  return {};
}

Polynomial cofactor(const Matrix& matrix, unsigned count,
                    unsigned skip_row, unsigned skip_col)
{
  Matrix minor{};
  unsigned dest_row = 0;
  for (unsigned row = 0; row < count; ++row) {
    if (row == skip_row) {
      continue;
    }
    unsigned dest_col = 0;
    for (unsigned col = 0; col < count; ++col) {
      if (col != skip_col) {
        minor[dest_row][dest_col++] = matrix[row][col];
      }
    }
    ++dest_row;
  }
  return determinant(minor, count - 1);
}

struct RecoveryColumn {
  char* output;
  array<Polynomial, 3> cofactors;
  array<char*, 2> global_outputs;
  array<char, window_bytes + 3 * tail_bytes> numerator{};
};

bool divide_window(RecoveryColumn& column, size_t begin, size_t end,
                   size_t size, size_t lead, span<const size_t> feedback,
                   size_t carry, size_t global_count)
{
  auto& numerator = column.numerator;
  // The lowest exponent supplies the next output block. Subtract the
  // resulting terms from the numerator in ascending offset order.
  const size_t length = end - begin;
  for (size_t i = 0; i < length; i += shift_bytes) {
    const auto value = load_symbol(numerator.data() + i);
    const size_t position = begin + i;
    if (position < lead || position >= lead + size) {
      if (!_mm256_testz_si256(value, value)) {
        return false;
      }
      continue;
    }
    const size_t offset = position - lead;
    store_symbol(column.output + offset, value);
    for (char* parity : span(column.global_outputs.data(), global_count)) {
      xor_symbol(parity + offset, value);
    }
    for (const size_t distance : feedback) {
      xor_symbol(numerator.data() + i + distance, value);
    }
  }
  // Carry contributions that leave this window into the next window.
  memmove(numerator.data(), numerator.data() + length, carry);
  memset(numerator.data() + carry, 0, length);
  return true;
}

void substitute_window(char* out, size_t begin, size_t end, unsigned size,
                       const vector<ShiftedInput>& inputs)
{
  for (const auto& input : inputs) {
    xor_window(out, begin, end, input.data, input.offset, size);
  }
}

bool recover_single(const vector<char>& residual, span<const ShiftedInput> inputs,
                    size_t size, size_t shift, char* output,
                    span<char* const> global_outputs)
{
  bool consistent = true;
  visit_shifted_sum(inputs, size, residual.size(), [&](size_t i, __m256i known) {
    const auto value = _mm256_xor_si256(load_symbol(residual.data() + i), known);
    if (i < shift || i >= shift + size) {
      consistent &= _mm256_testz_si256(value, value);
    } else {
      const size_t offset = i - shift;
      store_symbol(output + offset, value);
      for (char* parity : global_outputs) {
        xor_symbol(parity + offset, value);
      }
    }
  });
  return consistent;
}

int recover_fasttt(const vector<int>& equations, const set<int>& helpers,
                   const set<int>& targets, unsigned size,
                   map<int, bufferlist>& work, RedisTails& tails)
{
  vector<int> missing;
  for (int id : targets) {
    if (id < 8) {
      missing.push_back(id);
    }
  }
  const auto count = missing.size();
  if (!count) {
    if (targets.count(8) && targets.count(9)) {
      write_globals(size, work);
      return 0;
    }
    for (int id : targets) {
      if (is_global(id)) {
        write_global(id, size, work);
      }
    }
    return 0;
  }
  // A missing global parity accumulates contributions from both known and
  // newly solved data, using the same shifts as in encoding.
  vector<pair<int, char*>> globals;
  for (int id : targets) {
    if (is_global(id)) {
      char* output = prepare_global(id, size, work);
      memset(output, 0, shard_size(id, size));
      globals.emplace_back(id, output);
    }
  }
  array<vector<char>, 3> residuals;
  array<vector<ShiftedInput>, 3> residual_inputs;
  array<vector<ShiftedInput>, 2> global_inputs;
  for (unsigned row = 0; row < count; ++row) {
    const int parity = equations[row];
    const char* data = work.at(parity).c_str();
    auto& residual = residuals[row];
    if (tails.enabled() && is_global(parity)) {
      const int r = tails.load(parity, data, size, &residual);
      if (r) {
        return r;
      }
    } else {
      residual.assign(data, data + shard_size(parity, size));
    }
  }
  for (int id : helpers) {
    if (id >= 8) {
      continue;
    }
    const char* data = work.at(id).c_str();
    for (unsigned row = 0; row < count; ++row) {
      const int shift = parity_shift(equations[row], id);
      if (shift >= 0) {
        residual_inputs[row].push_back({data, size_t(shift) * shift_bytes});
      }
    }
    for (unsigned g = 0; g < globals.size(); ++g) {
      const int parity = globals[g].first;
      global_inputs[g].push_back({data, size_t(parity_shift(parity, id)) * shift_bytes});
    }
  }
  const auto map_outputs = [&globals](int data, array<char*, 2>& outputs) {
    for (unsigned g = 0; g < globals.size(); ++g) {
      const auto [parity, output] = globals[g];
      outputs[g] = output + parity_shift(parity, data) * shift_bytes;
    }
  };
  if (count == 1) {
    const int shift = parity_shift(equations.front(), missing.front());
    if (shift < 0) {
      return -EIO;
    }
    for (unsigned g = 0; g < globals.size(); ++g) {
      for (const auto& input : global_inputs[g]) {
        xor_bytes(globals[g].second + input.offset, input.data, size);
      }
    }
    array<char*, 2> outputs;
    map_outputs(missing.front(), outputs);
    return recover_single(residuals.front(), residual_inputs.front(), size,
                          size_t(shift) * shift_bytes, work.at(missing.front()).c_str(),
                          span(outputs.data(), globals.size())) ? 0 : -EIO;
  }
  const auto matrix = make_matrix(equations, missing);
  const auto divisor = determinant(matrix, count);
  if (!divisor) {
    return -EIO;
  }
  array<RecoveryColumn, 3> columns;
  for (unsigned col = 0; col < count; ++col) {
    auto& column = columns[col];
    column.output = work.at(missing[col]).c_str();
    for (unsigned row = 0; row < count; ++row) {
      column.cofactors[row] = cofactor(matrix, count, row, col);
    }
    map_outputs(missing[col], column.global_outputs);
  }
  const auto lead_term = first_term(divisor);
  const size_t lead = lead_term * shift_bytes;
  vector<size_t> feedback;
  for (auto terms = divisor & (divisor - 1); terms; terms &= terms - 1) {
    feedback.push_back((first_term(terms) - lead_term) * shift_bytes);
  }
  const size_t carry = feedback.empty() ? 0 : feedback.back();
  const size_t extent = size_t{size} + count * tail_bytes;
  const size_t global_length = size_t{size} + tail_bytes;
  // Cramer's rule over GF(2)[z]. With nonnegative cofactor shifts, this
  // numerator window only uses residuals prepared up through its end.
  for (size_t begin = 0; begin < extent; begin += window_bytes) {
    const size_t end = min(begin + window_bytes, extent);
    for (unsigned row = 0; row < count; ++row) {
      auto& residual = residuals[row];
      if (begin < residual.size()) {
        substitute_window(residual.data() + begin, begin, min(end, residual.size()),
                          size, residual_inputs[row]);
      }
    }
    if (begin < global_length) {
      for (unsigned g = 0; g < globals.size(); ++g) {
        substitute_window(globals[g].second + begin, begin, min(end, global_length),
                          size, global_inputs[g]);
      }
    }
    for (unsigned col = 0; col < count; ++col) {
      auto& column = columns[col];
      for (unsigned row = 0; row < count; ++row) {
        const auto& residual = residuals[row];
        for (auto terms = column.cofactors[row]; terms; terms &= terms - 1) {
          xor_window(column.numerator.data(), begin, end, residual.data(),
                     first_term(terms) * shift_bytes, residual.size());
        }
      }
      if (!divide_window(column, begin, end, size, lead, feedback, carry, globals.size())) {
        return -EIO;
      }
    }
  }
  return 0;
}

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
  const auto& mode = profile["mode"];
  if (mode != "scaffold" && mode != "fasttt" && mode != "fasttt-redis") {
    *ss << "tsx_lrc requires mode=scaffold, mode=fasttt or mode=fasttt-redis";
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
    *ss << "tsx_lrc supports only k=8 m=2 locality=4 and its fixed layout";
    return -EINVAL;
  }
  fasttt = mode != "scaffold";
  if (fasttt && g_code_env != CODE_ENVIRONMENT_UTILITY &&
      g_code_env != CODE_ENVIRONMENT_UTILITY_NODOUT) {
    *ss << "tsx_lrc FastTT modes are experimental and require an in-memory utility";
    return -EOPNOTSUPP;
  }
  redis_port = 0;
  if (mode == "fasttt-redis") {
    r = to_int("redis-port", profile, &redis_port, "6379", ss);
    if (r || redis_port < 1 || redis_port > 65535) {
      *ss << "redis-port must be between 1 and 65535";
      return -EINVAL;
    }
  } else if (profile.count("redis-port")) {
    *ss << "redis-port requires mode=fasttt-redis";
    return -EINVAL;
  }
  r = layers_init(ss);
  if (r) {
    return r;
  }
  return ErasureCode::init(profile, ss);
}

int ErasureCodeTsxLrc::layers_init(ostream* ss)
{
  layers.clear();
  layers.push_back({{0,1,2,3,4,5,6,7}, {8,9}, {}, {}});
  layers.push_back({{0,1,2,3}, {10}, {}, {}});
  layers.push_back({{4,5,6,7}, {11}, {}, {}});
  for (auto& layer : layers) {
    layer.chunks = layer.data;
    layer.chunks.insert(layer.chunks.end(), layer.coding.begin(), layer.coding.end());
    if (fasttt) {
      continue;
    }
    // Scaffold keeps ISA Cauchy globals and ISA m=1 XOR locals.
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
  if (fasttt) {
    constexpr unsigned stripe_alignment = 8 * shift_bytes;
    const auto stripes = (uint64_t{object_size} + stripe_alignment - 1) / stripe_alignment;
    return max<uint64_t>(1, stripes) * shift_bytes;
  }
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
      RepairStep step{static_cast<unsigned int>(i), {}, {}, {}};
      if (fasttt && i == 0) {
        vector<int> missing;
        for (int id : layers[i].data) {
          if (known.count(id)) {
            step.helpers.insert(id);
          } else {
            missing.push_back(id);
          }
        }
        if (!missing.empty()) {
          if (missing.size() > 3) {
            continue;
          }
          step.equations = choose_equations(known, missing);
          if (step.equations.empty()) {
            continue;
          }
          step.helpers.insert(step.equations.begin(), step.equations.end());
          step.targets.insert(missing.begin(), missing.end());
        }
        for (int id : layers[i].coding) {
          if (want.count(id) && !known.count(id)) {
            step.targets.insert(id);
          }
        }
        if (step.targets.empty()) {
          continue;
        }
      } else {
        for (int id : layers[i].chunks) {
          (known.count(id) ? step.helpers : step.targets).insert(id);
        }
        if (step.targets.empty() || step.targets.size() > layers[i].coding.size()) {
          continue;
        }
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

int ErasureCodeTsxLrc::encode_chunks(const set<int>& want,
                                    map<int, bufferlist>* encoded)
{
  if (layers.size() != 3 || !valid_ids(want) || encoded->size() != 12) {
    return -EINVAL;
  }
  const auto size = encoded->begin()->second.length();
  if (fasttt && (size % shift_bytes || size > numeric_limits<unsigned>::max() - tail_bytes)) {
    return -EINVAL;
  }
  for (const auto& chunk : *encoded) {
    if (size == 0 || chunk.first < 0 || chunk.first >= 12 ||
        chunk.second.length() != size || !chunk.second.is_contiguous()) {
      return -EINVAL;
    }
  }
  // Forward order, with the same physical-to-layer remapping as official LRC.
  for (const auto& layer : layers) {
    if (fasttt) {
      if (layer.coding.size() == 2) {
        write_globals(size, *encoded);
      } else {
        write_local(layer.coding.front(), layer.chunks, size, *encoded);
      }
      continue;
    }
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
  RedisTails tails(redis_port);
  return tails.enabled() ? tails.store({8, 9}, size, *encoded) : 0;
}

int ErasureCodeTsxLrc::decode_chunks(const set<int>& want,
                                    const map<int, bufferlist>& chunks,
                                    map<int, bufferlist>* decoded)
{
  auto fail = [decoded](int error) {
    decoded->clear();
    return error;
  };
  set<int> known = ids_of(chunks);
  RepairPlan plan;
  int r = make_repair_plan(want, known, &plan);
  if (r) {
    return fail(r);
  }
  if (chunks.empty() || chunks.begin()->second.length() == 0) {
    return fail(-EINVAL);
  }
  const auto& first = *chunks.begin();
  const auto prepared_size = first.second.length();
  const bool inline_tails = fasttt && !redis_port;
  const unsigned tail = inline_tails && is_global(first.first) ? tail_bytes : 0;
  if (prepared_size <= tail) {
    return fail(-EINVAL);
  }
  const auto size = prepared_size - tail;
  if (fasttt && (size % shift_bytes || size > numeric_limits<unsigned>::max() - tail_bytes)) {
    return fail(-EINVAL);
  }
  const auto expected_length = [inline_tails, size](int id) {
    return inline_tails ? shard_size(id, size) : size;
  };
  for (const auto& chunk : chunks) {
    if (chunk.second.length() != expected_length(chunk.first)) {
      return fail(-EINVAL);
    }
  }
  for (int id = 0; id < 12; ++id) {
    auto chunk = decoded->find(id);
    const auto length = known.count(id) ? expected_length(id) : prepared_size;
    if (chunk == decoded->end() || chunk->second.length() != length) {
      // Low-level entry requires Ceph's prepared work buffers.
      return fail(-EINVAL);
    }
  }
  RedisTails tails(redis_port);
  set<int> rebuilt_globals;
  for (const auto& step : plan.steps) {
    const auto& layer = layers[step.layer];
    // Ceph sizes unfilled slots using the first available shard. Correct
    // only the targets we actually reconstruct, as data and globals differ.
    if (fasttt) {
      for (int id : step.targets) {
        auto& output = decoded->at(id);
        const auto length = expected_length(id);
        if (output.length() != length) {
          output.clear();
          output.push_back(buffer::create_aligned(length, shift_bytes));
        }
      }
      if (step.layer == 0) {
        r = recover_fasttt(step.equations, step.helpers, step.targets, size, *decoded, tails);
        if (r) {
          return fail(r);
        }
        if (tails.enabled()) {
          for (int id : step.targets) {
            if (is_global(id)) {
              rebuilt_globals.insert(id);
            }
          }
        }
      } else {
        write_local(*step.targets.begin(), layer.chunks, size, *decoded);
      }
      known.insert(step.targets.begin(), step.targets.end());
      continue;
    }
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
      return fail(r);
    }
    for (unsigned int j = 0; j < layer.chunks.size(); ++j) {
      (*decoded)[layer.chunks[j]] = layer_decoded[j];
    }
    known.insert(step.targets.begin(), step.targets.end());
  }
  r = tails.store(rebuilt_globals, size, *decoded);
  if (r) {
    return fail(r);
  }
  // Keep Ceph's default encode/decode wrappers. _decode preallocates all
  // absent shards, but a local repair need not fill them all. Return only
  // known inputs and completed results; no extra shard computation or copy.
  for (auto i = decoded->begin(); i != decoded->end();) {
    if (known.count(i->first)) {
      ++i;
    } else {
      i = decoded->erase(i);
    }
  }
  return 0;
}
