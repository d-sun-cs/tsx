// SPDX-License-Identifier: LGPL-2.1-or-later
#include "ErasureCodeTsxLrc.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <numeric>
#include <span>
#include <hiredis/hiredis.h>
#include <immintrin.h>
#include "common/code_environment.h"
#include "include/buffer.h"
#include "erasure-code/ErasureCodePlugin.h"

using namespace std;
using namespace ceph;

namespace {
constexpr unsigned shift_bytes = 32;
static_assert(shift_bytes == sizeof(__m256i));
constexpr size_t window_bytes = 16 * shift_bytes;

// Only the two global equations contribute positive exponents. Their
// determinant has degree at most 2*(k-1), which must fit in Polynomial.
using Polynomial = uint64_t;
constexpr int max_data_chunks = (numeric_limits<Polynomial>::digits + 1) / 2;
constexpr size_t max_shift_bytes = (max_data_chunks - 1) * shift_bytes;
constexpr size_t max_feedback_bytes = 2 * max_shift_bytes;

struct Layout {
  int k;

  int group_size() const { return k / 2; }
  int group_of(int data) const { return data < group_size() ? 0 : 1; }
  int local_id(int group) const { return k + 2 + group; }
  bool is_global(int id) const { return id == k || id == k + 1; }
  unsigned tail_size() const { return (k - 1) * shift_bytes; }
  unsigned shard_size(int id, unsigned data_size) const
  {
    return data_size + (is_global(id) ? tail_size() : 0);
  }

  // Exponents are in symbols. -1 excludes an input from the equation.
  int parity_shift(int parity, int data) const
  {
    if (parity == k) {
      return data;
    }
    if (parity == k + 1) {
      return k - 1 - data;
    }
    return local_id(group_of(data)) == parity ? 0 : -1;
  }
};

string tail_key(int id, const char* shard)
{
  // Use the first raw symbol as the experimental key, as in ordinary TSX.
  string key = "fasttt:lrc:raw-tail:v1:" + to_string(id) + ":";
  key.append(shard, shift_bytes);
  return key;
}

using RedisReply = unique_ptr<redisReply, decltype(&freeReplyObject)>;

// One lazy connection per call. A local repair has no Redis operations.
class RedisTails {
  const int port;
  const Layout layout;
  unique_ptr<redisContext, decltype(&redisFree)> connection{nullptr, redisFree};

  RedisReply command(const vector<string>& args)
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
  RedisTails(int port, Layout layout) : port(port), layout(layout) {}
  bool enabled() const { return port != 0; }

  int load(int id, const char* shard, RedisReply* tail)
  {
    auto reply = command({"GET", tail_key(id, shard)});
    if (!reply) {
      return -EIO;
    }
    if (reply->type == REDIS_REPLY_NIL) {
      return -ENOENT;
    }
    if (reply->type != REDIS_REPLY_STRING || reply->len != layout.tail_size()) {
      return -EIO;
    }
    *tail = std::move(reply);
    return 0;
  }

  int store(const set<int>& ids, unsigned size, map<int, bufferlist>& work)
  {
    if (ids.empty()) {
      return 0;
    }
    const auto tail_size = layout.tail_size();
    vector<string> args{"MSET"};
    for (int id : ids) {
      const char* full = work.at(id).c_str();
      args.push_back(tail_key(id, full));
      args.emplace_back(full + size, tail_size);
    }
    const auto reply = command(args);
    if (!reply || reply->type != REDIS_REPLY_STATUS || strcmp(reply->str, "OK") != 0) {
      return -EIO;
    }
    for (int id : ids) {
      work.at(id).splice(size, tail_size);
    }
    return 0;
  }
};

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

char* prepare_global(Layout layout, int id, unsigned size, map<int, bufferlist>& work)
{
  auto& output = work.at(id);
  const auto length = layout.shard_size(id, size);
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
  size_t i = interior_begin;
  constexpr unsigned unroll = 4;
  for (; i + unroll * shift_bytes <= size; i += unroll * shift_bytes) {
    __m256i sums[unroll]{};
    for (const auto& input : inputs) {
      const char* source = input.data + (i - input.offset);
      #pragma GCC unroll 4
      for (unsigned s = 0; s < unroll; ++s) {
        sums[s] = _mm256_xor_si256(sums[s], load_symbol(source + s * shift_bytes));
      }
    }
    #pragma GCC unroll 4
    for (unsigned s = 0; s < unroll; ++s) {
      store(i + s * shift_bytes, sums[s]);
    }
  }
  for (; i < size; i += shift_bytes) {
    auto sum = _mm256_setzero_si256();
    for (const auto& input : inputs) {
      sum = _mm256_xor_si256(sum, load_symbol(input.data + (i - input.offset)));
    }
    store(i, sum);
  }
  visit_edge(max(interior_begin, size), length);
}

void write_local(int target, const vector<int>& group, unsigned size,
                 map<int, bufferlist>& work)
{
  char* output = work.at(target).c_str();
  array<ShiftedInput, max_data_chunks / 2> inputs;
  unsigned index = 0;
  for (int id : group) {
    if (id != target) {
      inputs[index++] = {work.at(id).c_str(), 0};
    }
  }
  if (index != 4) {
    visit_shifted_sum(span(inputs.data(), index), size, size,
                      [output](size_t i, __m256i value) { store_symbol(output + i, value); });
    return;
  }
  #pragma GCC unroll 4
  for (size_t i = 0; i < size; i += shift_bytes) {
    const auto left = _mm256_xor_si256(load_symbol(inputs[0].data + i), load_symbol(inputs[1].data + i));
    const auto right = _mm256_xor_si256(load_symbol(inputs[2].data + i), load_symbol(inputs[3].data + i));
    store_symbol(output + i, _mm256_xor_si256(left, right));
  }
}

void write_global(Layout layout, int id, unsigned size, map<int, bufferlist>& work)
{
  char* output = prepare_global(layout, id, size, work);
  array<ShiftedInput, max_data_chunks> inputs;
  for (int data = 0; data < layout.k; ++data) {
    inputs[data] = {work.at(data).c_str(), size_t(layout.parity_shift(id, data)) * shift_bytes};
  }
  visit_shifted_sum(span(inputs.data(), layout.k), size, layout.shard_size(id, size),
                    [output](size_t i, __m256i value) { store_symbol(output + i, value); });
}

struct ParityWindow {
  char* output;
  size_t overlap_begin;
  size_t overlap_end;

  void add(size_t offset, __m256i symbol) const
  {
    if (offset >= overlap_begin && offset < overlap_end) {
      xor_symbol(output + offset, symbol);
    } else {
      store_symbol(output + offset, symbol);
    }
  }
};

struct WrittenParity {
  char* output;
  size_t begin = 0;
  size_t end = 0;

  ParityWindow add_window(size_t start, size_t stop)
  {
    const ParityWindow window{
      output + start,
      clamp(begin, start, stop) - start,
      clamp(end, start, stop) - start,
    };
    // Consecutive pairs are two symbols apart and contribute at least
    // two symbols, so their union stays contiguous in either direction.
    begin = begin == end ? start : min(begin, start);
    end = max(end, stop);
    return window;
  }
};

template<bool WriteLocals>
void write_parities(Layout layout, unsigned size, map<int, bufferlist>& work)
{
  WrittenParity forward{prepare_global(layout, layout.k, size, work)};
  WrittenParity reverse{prepare_global(layout, layout.k + 1, size, work)};
  array<char*, 2> locals{};
  if constexpr (WriteLocals) {
    locals = {work.at(layout.local_id(0)).c_str(), work.at(layout.local_id(1)).c_str()};
  }
  array<const char*, max_data_chunks> inputs;
  for (int data = 0; data < layout.k; ++data) {
    inputs[data] = work.at(data).c_str();
  }
  for (size_t begin = 0; begin < size; begin += window_bytes) {
    const size_t length = min(window_bytes, size - begin);
    const size_t pair_length = length + shift_bytes;
    for (int data = 0; data < layout.k; data += 2) {
      const size_t plus_offset = begin + layout.parity_shift(layout.k, data) * shift_bytes;
      const size_t minus_offset = begin + layout.parity_shift(layout.k + 1, data + 1) * shift_bytes;
      const auto plus = forward.add_window(plus_offset, plus_offset + pair_length);
      const auto minus = reverse.add_window(minus_offset, minus_offset + pair_length);
      const char* even = inputs[data] + begin;
      const char* odd = inputs[data + 1] + begin;
      char* local = nullptr;
      char* next_local = nullptr;
      bool first_pair = false;
      if constexpr (WriteLocals) {
        const int group = layout.group_of(data);
        local = locals[group] + begin;
        first_pair = data == group * layout.group_size();
        if (group != layout.group_of(data + 1)) {
          next_local = locals[group + 1] + begin;
        }
      }
      auto prev_even = _mm256_setzero_si256();
      auto prev_odd = _mm256_setzero_si256();
      // Adjacent shards have shifts one symbol apart. The two parities
      // reuse each pair of loads, in opposite shift order.
      for (size_t i = 0; i < length; i += shift_bytes) {
        const auto current_even = load_symbol(even + i);
        const auto current_odd = load_symbol(odd + i);
        plus.add(i, _mm256_xor_si256(current_even, prev_odd));
        minus.add(i, _mm256_xor_si256(current_odd, prev_even));
        if constexpr (WriteLocals) {
          // An odd-sized group can end in the middle of this pair.
          if (next_local) {
            xor_symbol(local + i, current_even);
            store_symbol(next_local + i, current_odd);
          } else {
            const auto pair_sum = _mm256_xor_si256(current_even, current_odd);
            if (first_pair) {
              store_symbol(local + i, pair_sum);
            } else {
              xor_symbol(local + i, pair_sum);
            }
          }
        }
        prev_even = current_even;
        prev_odd = current_odd;
      }
      plus.add(length, prev_odd);
      minus.add(length, prev_even);
    }
  }
}

// A bit at position s represents z^s, where z shifts by one 32-byte unit.
// At most three erased data shards participate in a joint solve.
using Matrix = array<array<Polynomial, 3>, 3>;

unsigned first_term(Polynomial p)
{
  return __builtin_ctzll(p);
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

Matrix make_matrix(Layout layout, const vector<int>& equations, const vector<int>& missing)
{
  Matrix matrix{};
  for (unsigned row = 0; row < equations.size(); ++row) {
    for (unsigned col = 0; col < missing.size(); ++col) {
      const int shift = layout.parity_shift(equations[row], missing[col]);
      if (shift >= 0) {
        matrix[row][col] = Polynomial{1} << shift;
      }
    }
  }
  return matrix;
}

vector<int> choose_equations(Layout layout, const set<int>& known, const vector<int>& missing)
{
  // Try the local equations before a second global, when either suffices.
  const array<int, 4> parity_ids{layout.local_id(0), layout.local_id(1), layout.k, layout.k + 1};
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
    if (determinant(make_matrix(layout, equations, missing), missing.size())) {
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
  array<char, max_feedback_bytes> tail;
};

bool zero_window(const char* input, size_t length)
{
  for (size_t i = 0; i < length; i += shift_bytes) {
    const auto value = load_symbol(input + i);
    if (!_mm256_testz_si256(value, value)) {
      return false;
    }
  }
  return true;
}

void divide_output_window(RecoveryColumn& column, size_t begin, size_t end,
                          size_t size, span<const size_t> feedback,
                          size_t global_count)
{
  const auto globals = span(column.global_outputs.data(), global_count);
  // Each symbol becomes the quotient in place once earlier feedback has
  // reached it. Only the contributions beyond the output need scratch.
  for (size_t offset = begin; offset < end; offset += shift_bytes) {
    const auto value = load_symbol(column.output + offset);
    for (char* parity : globals) {
      xor_symbol(parity + offset, value);
    }
    for (const size_t distance : feedback) {
      const size_t next = offset + distance;
      char* destination = next < size ? column.output + next
                                      : column.tail.data() + (next - size);
      xor_symbol(destination, value);
    }
  }
}

// Keep the input shard read-only. When Redis supplies a tail, its reply
// owns the second segment until the equation has been consumed.
struct ParityInput {
  const char* main;
  size_t main_size;
  RedisReply tail{nullptr, freeReplyObject};

  size_t size() const { return main_size + (tail ? tail->len : 0); }
  const char* symbol_at(size_t offset) const
  {
    return offset < main_size ? main + offset : tail->str + (offset - main_size);
  }

  void copy_window(char* output, size_t begin, size_t end) const
  {
    const size_t main_end = min(end, main_size);
    if (begin < main_end) {
      memcpy(output, main + begin, main_end - begin);
    }
    const size_t tail_begin = max(begin, main_size);
    const size_t available_end = min(end, size());
    if (tail_begin < available_end) {
      memcpy(output + (tail_begin - begin), tail->str + (tail_begin - main_size),
             available_end - tail_begin);
    }
    const size_t zero_begin = max(begin, available_end);
    if (zero_begin < end) {
      memset(output + (zero_begin - begin), 0, end - zero_begin);
    }
  }
};

// The history serves cofactor shifts. The lookahead receives contributions
// from the current data window whose parity offsets lie in later windows.
struct ResidualWindow {
  size_t history = 0;
  size_t lookahead = 0;
  array<char, max_feedback_bytes + window_bytes + max_shift_bytes> bytes;

  char* current() { return bytes.data() + history; }
};

// A data shard appears in at most one local and two global parities.
struct SubstitutionInput {
  const char* data;
  array<char*, 3> outputs;
  unsigned residual_count = 0;
  unsigned output_count = 0;
};

template<unsigned Count>
void xor_shared(const char* input, const array<char*, 3>& outputs, size_t length)
{
  for (size_t i = 0; i < length; i += shift_bytes) {
    const auto value = load_symbol(input + i);
    for (unsigned dst = 0; dst < Count; ++dst) {
      xor_symbol(outputs[dst] + i, value);
    }
  }
}

void substitute_known(const SubstitutionInput& input, size_t begin, size_t length)
{
  auto outputs = input.outputs;
  // Residual pointers already point to the current window. Reconstructed
  // globals use absolute offsets into the output shard.
  for (unsigned g = input.residual_count; g < input.output_count; ++g) {
    outputs[g] += begin;
  }
  const char* data = input.data + begin;
  switch (input.output_count) {
  case 1:
    xor_bytes(outputs[0], data, length);
    break;
  case 2:
    xor_shared<2>(data, outputs, length);
    break;
  case 3:
    xor_shared<3>(data, outputs, length);
    break;
  }
}

bool recover_single(const ParityInput& parity, span<const ShiftedInput> inputs,
                    size_t size, size_t shift, char* output)
{
  bool consistent = true;
  visit_shifted_sum(inputs, size, parity.size(), [&](size_t i, __m256i known) {
    const auto value = _mm256_xor_si256(load_symbol(parity.symbol_at(i)), known);
    if (i < shift || i >= shift + size) {
      consistent &= _mm256_testz_si256(value, value);
    } else {
      store_symbol(output + (i - shift), value);
    }
  });
  return consistent;
}

int recover_fasttt(Layout layout, const vector<int>& equations, const set<int>& helpers,
                   const set<int>& targets, unsigned size,
                   map<int, bufferlist>& work, RedisTails& tails)
{
  vector<int> missing;
  for (int id : targets) {
    if (id < layout.k) {
      missing.push_back(id);
    }
  }
  const auto count = missing.size();
  if (!count) {
    if (targets.count(layout.k) && targets.count(layout.k + 1)) {
      write_parities<false>(layout, size, work);
      return 0;
    }
    for (int id : targets) {
      if (layout.is_global(id)) {
        write_global(layout, id, size, work);
      }
    }
    return 0;
  }
  // A missing global parity accumulates contributions from both known and
  // newly solved data, using the same shifts as in encoding.
  vector<pair<int, char*>> globals;
  for (int id : targets) {
    if (layout.is_global(id)) {
      char* output = prepare_global(layout, id, size, work);
      memset(output, 0, layout.shard_size(id, size));
      globals.emplace_back(id, output);
    }
  }
  array<ParityInput, 3> parities;
  for (unsigned row = 0; row < count; ++row) {
    const int parity = equations[row];
    auto& input = parities[row];
    input.main = work.at(parity).c_str();
    input.main_size = layout.shard_size(parity, size);
    if (tails.enabled() && layout.is_global(parity)) {
      input.main_size = size;
      const int r = tails.load(parity, input.main, &input.tail);
      if (r) {
        return r;
      }
    }
  }
  vector<pair<int, const char*>> known_data;
  for (int id : helpers) {
    if (id < layout.k) {
      known_data.emplace_back(id, work.at(id).c_str());
    }
  }
  const auto map_outputs = [&globals, layout](int data, array<char*, 2>& outputs) {
    for (unsigned g = 0; g < globals.size(); ++g) {
      const auto [parity, output] = globals[g];
      outputs[g] = output + layout.parity_shift(parity, data) * shift_bytes;
    }
  };
  if (count == 1 && globals.empty()) {
    const int shift = layout.parity_shift(equations.front(), missing.front());
    if (shift < 0) {
      return -EIO;
    }
    vector<ShiftedInput> residual_inputs;
    for (const auto& [id, data] : known_data) {
      const int input_shift = layout.parity_shift(equations.front(), id);
      if (input_shift >= 0) {
        residual_inputs.push_back({data, size_t(input_shift) * shift_bytes});
      }
    }
    return recover_single(parities.front(), residual_inputs, size,
                          size_t(shift) * shift_bytes, work.at(missing.front()).c_str()) ? 0 : -EIO;
  }
  // When a global parity is also missing, share the windowed input loads
  // even for one erased data shard. Its determinant is a single shift.
  const auto matrix = make_matrix(layout, equations, missing);
  const auto divisor = determinant(matrix, count);
  if (!divisor) {
    return -EIO;
  }
  array<RecoveryColumn, 3> columns;
  array<ResidualWindow, 3> residuals;
  for (unsigned col = 0; col < count; ++col) {
    auto& column = columns[col];
    column.output = work.at(missing[col]).c_str();
    for (unsigned row = 0; row < count; ++row) {
      const auto terms = cofactor(matrix, count, row, col);
      column.cofactors[row] = terms;
      if (terms) {
        const size_t history = (bit_width(terms) - 1) * shift_bytes;
        residuals[row].history = max(residuals[row].history, history);
      }
    }
    map_outputs(missing[col], column.global_outputs);
  }
  vector<SubstitutionInput> substitutions;
  substitutions.reserve(known_data.size());
  for (const auto& [id, data] : known_data) {
    SubstitutionInput input{data, {}};
    for (unsigned row = 0; row < count; ++row) {
      const int shift = layout.parity_shift(equations[row], id);
      if (shift >= 0) {
        const size_t offset = size_t(shift) * shift_bytes;
        auto& residual = residuals[row];
        residual.lookahead = max(residual.lookahead, offset);
        input.outputs[input.residual_count++] = residual.current() + offset;
      }
    }
    input.output_count = input.residual_count;
    for (const auto& [parity, output] : globals) {
      input.outputs[input.output_count++] = output + layout.parity_shift(parity, id) * shift_bytes;
    }
    substitutions.push_back(input);
  }
  const auto lead_term = first_term(divisor);
  const size_t lead = lead_term * shift_bytes;
  vector<size_t> feedback;
  for (auto terms = divisor & (divisor - 1); terms; terms &= terms - 1) {
    feedback.push_back((first_term(terms) - lead_term) * shift_bytes);
  }
  const size_t carry = feedback.empty() ? 0 : feedback.back();
  const size_t data_end = lead + size;
  // A cofactor times its residual contains at most two global shifts.
  const size_t extent = size_t{size} + 2 * layout.tail_size();
  for (unsigned col = 0; col < count; ++col) {
    memset(columns[col].tail.data(), 0, extent - data_end);
  }
  for (unsigned row = 0; row < count; ++row) {
    auto& residual = residuals[row];
    fill_n(residual.bytes.data(), residual.history, 0);
    parities[row].copy_window(residual.current(), 0, residual.lookahead);
  }
  array<char, window_bytes> prefix;
  // Cramer's rule over GF(2)[z]. With nonnegative cofactor shifts, this
  // numerator window uses the current residual window and bounded history.
  for (size_t begin = 0; begin < extent;) {
    size_t end = min(begin + window_bytes, extent);
    const bool in_data = begin >= lead && begin < data_end;
    // A window lies entirely in the prefix, the destination, or the tail.
    if (begin < lead) {
      end = min(end, lead);
    } else if (in_data) {
      end = min(end, data_end);
    }
    const size_t length = end - begin;
    for (unsigned row = 0; row < count; ++row) {
      auto& residual = residuals[row];
      parities[row].copy_window(residual.current() + residual.lookahead,
                               begin + residual.lookahead, end + residual.lookahead);
    }
    if (begin < size) {
      const size_t data_length = min(end, size_t{size}) - begin;
      for (const auto& input : substitutions) {
        substitute_known(input, begin, data_length);
      }
    }
    for (unsigned col = 0; col < count; ++col) {
      auto& column = columns[col];
      char* window;
      size_t previous = 0;
      if (begin < lead) {
        window = prefix.data();
      } else if (in_data) {
        const size_t offset = begin - lead;
        window = column.output + offset;
        previous = offset ? min(carry, length) : 0;
        // The first cofactor will initialize fresh bytes in this window.
        // Only future bytes that can receive feedback need to be zeroed.
        const size_t known_end = offset ? offset + carry : 0;
        const size_t clear_begin = min(size_t{size}, max(end - lead, known_end));
        const size_t clear_end = min(size_t{size}, end - lead + carry);
        memset(column.output + clear_begin, 0, clear_end - clear_begin);
      } else {
        window = column.tail.data() + (begin - data_end);
        previous = length;
      }
      bool initialized = previous == length;
      for (unsigned row = 0; row < count; ++row) {
        const auto& residual = residuals[row];
        for (auto terms = column.cofactors[row]; terms; terms &= terms - 1) {
          const auto offset = residual.history - first_term(terms) * shift_bytes;
          const char* source = residual.bytes.data() + offset;
          if (initialized) {
            xor_bytes(window, source, length);
          } else {
            // A nonzero determinant guarantees at least one cofactor.
            // Preserve feedback in the prefix and copy the fresh range.
            xor_bytes(window, source, previous);
            memcpy(window + previous, source + previous, length - previous);
            initialized = true;
          }
        }
      }
      if (in_data) {
        divide_output_window(column, begin - lead, end - lead, size, feedback, globals.size());
      } else if (!zero_window(window, length)) {
        return -EIO;
      }
    }
    for (unsigned row = 0; row < count; ++row) {
      auto& residual = residuals[row];
      const size_t keep = residual.history + residual.lookahead;
      if (keep) {
        memmove(residual.bytes.data(), residual.bytes.data() + length, keep);
      }
    }
    begin = end;
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
  r |= to_int("locality", profile, &locality, std::to_string(k / 2), ss);
  if (r) {
    return r;
  }
  if (k < 4 || k > max_data_chunks || k % 2 || m != 2 || locality != k / 2 ||
      profile.count("l") ||
      profile.count("mapping") || profile.count("layers") ||
      profile.count("crush-locality") || profile.count("crush-steps")) {
    *ss << "tsx_lrc requires even k in [4," << max_data_chunks
        << "], m=2 and locality=k/2 with two equal local groups";
    return -EINVAL;
  }
  data_chunks = k;
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
  const Layout layout{data_chunks};
  layers.clear();
  vector<int> data(layout.k);
  iota(data.begin(), data.end(), 0);
  const auto middle = data.begin() + layout.group_size();
  layers.push_back({data, {layout.k, layout.k + 1}, {}, {}});
  layers.push_back({{data.begin(), middle}, {layout.local_id(0)}, {}, {}});
  layers.push_back({{middle, data.end()}, {layout.local_id(1)}, {}, {}});
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
    const unsigned stripe_alignment = data_chunks * shift_bytes;
    const auto stripes = (uint64_t{object_size} + stripe_alignment - 1) / stripe_alignment;
    return max<uint64_t>(1, stripes) * shift_bytes;
  }
  return layers.empty() ? 0 : layers.front().erasure_code->get_chunk_size(object_size);
}

bool ErasureCodeTsxLrc::valid_ids(const set<int>& ids) const
{
  return ids.empty() || (*ids.begin() >= 0 && static_cast<unsigned>(*ids.rbegin()) < get_chunk_count());
}

int ErasureCodeTsxLrc::make_repair_plan(const set<int>& want,
                                      const set<int>& available,
                                      RepairPlan* plan) const
{
  *plan = {};
  if (layers.size() != 3 || !valid_ids(want) || !valid_ids(available)) {
    return -EINVAL;
  }
  const Layout layout{data_chunks};
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
          step.equations = choose_equations(layout, known, missing);
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
  if (layers.size() != 3 || !valid_ids(want) || encoded->size() != get_chunk_count()) {
    return -EINVAL;
  }
  const Layout layout{data_chunks};
  const auto size = encoded->begin()->second.length();
  if (fasttt && (size % shift_bytes || size > numeric_limits<unsigned>::max() - layout.tail_size())) {
    return -EINVAL;
  }
  for (const auto& chunk : *encoded) {
    if (size == 0 || chunk.first < 0 || static_cast<unsigned>(chunk.first) >= get_chunk_count() ||
        chunk.second.length() != size || !chunk.second.is_contiguous()) {
      return -EINVAL;
    }
  }
  if (fasttt) {
    write_parities<true>(layout, size, *encoded);
    RedisTails tails(redis_port, layout);
    return tails.enabled() ? tails.store({layout.k, layout.k + 1}, size, *encoded) : 0;
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
  const Layout layout{data_chunks};
  const auto& first = *chunks.begin();
  const auto prepared_size = first.second.length();
  const bool inline_tails = fasttt && !redis_port;
  const unsigned tail = inline_tails && layout.is_global(first.first) ? layout.tail_size() : 0;
  if (prepared_size <= tail) {
    return fail(-EINVAL);
  }
  const auto size = prepared_size - tail;
  if (fasttt && (size % shift_bytes || size > numeric_limits<unsigned>::max() - layout.tail_size())) {
    return fail(-EINVAL);
  }
  const auto expected_length = [inline_tails, size, layout](int id) {
    return inline_tails ? layout.shard_size(id, size) : size;
  };
  for (const auto& chunk : chunks) {
    if (chunk.second.length() != expected_length(chunk.first)) {
      return fail(-EINVAL);
    }
  }
  for (unsigned id = 0; id < get_chunk_count(); ++id) {
    auto chunk = decoded->find(id);
    const auto length = known.count(id) ? expected_length(id) : prepared_size;
    if (chunk == decoded->end() || chunk->second.length() != length) {
      // Low-level entry requires Ceph's prepared work buffers.
      return fail(-EINVAL);
    }
  }
  RedisTails tails(redis_port, layout);
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
        r = recover_fasttt(layout, step.equations, step.helpers, step.targets, size, *decoded, tails);
        if (r) {
          return fail(r);
        }
        if (tails.enabled()) {
          for (int id : step.targets) {
            if (layout.is_global(id)) {
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
