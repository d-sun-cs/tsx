// SPDX-License-Identifier: LGPL-2.1-or-later
// Plugin contract and concurrent-request benchmark for installed tsx_mt.
#include <algorithm>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <future>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <memory>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <hiredis/hiredis.h>

#include "common/code_environment.h"
#include "erasure-code/ErasureCodePlugin.h"
#include "global/global_context.h"
#include "global/global_init.h"

using namespace std;
using namespace ceph;

namespace {
using Chunks = map<int, bufferlist>;
using Clock = chrono::steady_clock;
constexpr int k = 8;
constexpr int p0 = 9;
constexpr int symbol_bytes = 32;
constexpr size_t stripe_bytes = k * 2 * 1024 * 1024;
const set<int> all_ids{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10};

void check(bool ok, const string& message)
{
  if (!ok) {
    throw runtime_error(message);
  }
}

int positive_int(string_view text, int limit)
{
  int value{};
  const auto [end, error] = from_chars(text.data(), text.data() + text.size(), value);
  check(error == std::errc{} && end == text.data() + text.size() &&
        value >= 1 && value <= limit, "invalid argument: " + string(text));
  return value;
}

ErasureCodeInterfaceRef load(const string& directory, int port, int threads, int expected = 0)
{
  ErasureCodeProfile profile{{"k", "8"}, {"m", "3"}, {"threads", to_string(threads)},
                             {"redis-port", to_string(port)}};
  ErasureCodeInterfaceRef code;
  ostringstream errors;
  const int r = ErasureCodePluginRegistry::instance().factory(
    "tsx_mt", directory, profile, &code, &errors);
  check(r == expected && bool(code) == (expected == 0),
        "tsx_mt factory: " + to_string(r) + " " + errors.str());
  return code;
}

bufferlist random_input(size_t size, unsigned seed)
{
  mt19937 generator(seed);
  string bytes(size, '\0');
  for (char& byte : bytes) {
    byte = static_cast<char>(generator() & 0xff);
  }
  bufferlist raw;
  raw.append(bytes);
  return raw;
}

Chunks select_helpers(ErasureCodeInterface& code, const Chunks& all, const set<int>& want)
{
  set<int> available;
  set_difference(all_ids.begin(), all_ids.end(), want.begin(), want.end(),
                 inserter(available, available.end()));
  map<int, vector<pair<int, int>>> minimum;
  check(code.minimum_to_decode(want, available, &minimum) == 0 && minimum.size() == k,
        "could not select eight helpers");
  Chunks helpers;
  for (const auto& [id, ranges] : minimum) {
    check(available.count(id) && ranges == vector<pair<int, int>>{{0, 1}},
          "unexpected helper or subchunk range");
    helpers.emplace(id, all.at(id));
  }
  return helpers;
}

void verify(const Chunks& expected, const Chunks& got, const set<int>& want)
{
  for (int id : want) {
    check(got.count(id), "missing output shard " + to_string(id));
  }
  for (const auto& [id, shard] : got) {
    check(expected.count(id) && expected.at(id).contents_equal(shard),
          "wrong bytes for shard " + to_string(id));
  }
}

void check_contract(const string& directory, int port, ErasureCodeInterface& code)
{
  load(directory, port, 0, -EINVAL);
  check(code.get_chunk_count() == 11 && code.get_data_chunk_count() == k, "wrong layout");
  unique_ptr<redisContext, decltype(&redisFree)> connection(redisConnect("127.0.0.1", port),
                                                            redisFree);
  check(connection && !connection->err, "test Redis connection failed");
  for (const size_t length : {size_t{1}, size_t{k * 16 * symbol_bytes + 1}}) {
    const auto raw = random_input(length, length);
    Chunks all;
    check(code.encode(all_ids, raw, &all) == 0 && all.size() == all_ids.size(), "encode failed");
    const size_t size = code.get_chunk_size(length);
    string padded = raw.to_str();
    padded.resize(k * size, '\0');
    for (int id = 0; id < k; ++id) {
      check(all.at(id).to_str() == padded.substr(id * size, size), "data padding or order changed");
    }
    string tails;
    for (int parity : {8, 9, 10}) {
      string bytes(size + (parity == p0 ? 0 : (k - 1) * symbol_bytes), '\0');
      for (int id = 0; id < k; ++id) {
        const size_t shift = (parity == 8 ? id : parity == 10 ? k - 1 - id : 0) * symbol_bytes;
        for (size_t i = 0; i < size; ++i) {
          bytes[shift + i] ^= padded[id * size + i];
        }
      }
      check(all.at(parity).to_str() == bytes.substr(0, size), "parity disagrees with FastTT formula");
      tails += bytes.substr(size);
    }
    const auto verify_tails = [&] {
      const auto key = all.at(p0).to_str().substr(0, symbol_bytes);
      unique_ptr<redisReply, decltype(&freeReplyObject)> reply(
        static_cast<redisReply*>(redisCommand(connection.get(), "GET %b", key.data(), key.size())),
        freeReplyObject);
      check(reply && reply->type == REDIS_REPLY_STRING &&
            string_view(reply->str, reply->len) == tails, "legacy Redis tail format changed");
    };
    verify_tails();
    vector<set<int>> targets{{0, 8}, {8, 9, 10}};
    for (int id : all_ids) {
      targets.push_back({id});
    }
    for (const auto& want : targets) {
      const auto helpers = select_helpers(code, all, want);
      Chunks decoded;
      check(code.decode(want, helpers, &decoded, size) == 0, "selected-helper decode failed");
      verify(all, decoded, want);
    }
    verify_tails();
    auto insufficient = all;
    insufficient.erase(0);
    insufficient.erase(1);
    Chunks rejected;
    check(code.decode({0, 1}, insufficient, &rejected, size) == -EOPNOTSUPP &&
          rejected.empty(), "double data erasure must report unsupported");
  }
  cerr << "PASS tsx_mt: scalar parity and Redis tails, all single erasures, mixed repair, "
          "unsupported double data erasure\n";
}

struct Request {
  bufferlist raw;
  Chunks encoded;
  Chunks helpers;
  set<int> want;
};

struct Result {
  Chunks output;
  vector<double> latency_us;
};

void run_concurrent(ErasureCodeInterface& code, const vector<Request>& requests,
                    int threads, int iterations, bool decoding)
{
  promise<void> release;
  shared_future<void> start = release.get_future();
  vector<future<Result>> clients;
  vector<Result> results;
  clients.reserve(requests.size());
  results.reserve(requests.size());
  try {
    for (const auto& request : requests) {
      clients.push_back(async(launch::async, [&, request = &request] {
        Result result;
        result.latency_us.reserve(iterations);
        start.get();
        for (int i = 0; i < iterations; ++i) {
          result.output.clear();
          const auto begin = Clock::now();
          const int r = decoding
            ? code.decode(request->want, request->helpers, &result.output, stripe_bytes / k)
            : code.encode(all_ids, request->raw, &result.output);
          const auto elapsed = Clock::now() - begin;
          check(r == 0, "concurrent request failed: " + to_string(r));
          result.latency_us.push_back(chrono::duration<double, micro>(elapsed).count());
        }
        return result;
      }));
    }
  } catch (...) {
    release.set_exception(current_exception());
    throw;
  }
  const auto begin = Clock::now();
  release.set_value();
  for (auto& client : clients) {
    results.push_back(client.get());
  }
  const double seconds = chrono::duration<double>(Clock::now() - begin).count();
  vector<double> latencies;
  for (size_t i = 0; i < results.size(); ++i) {
    const auto& result = results[i];
    verify(requests[i].encoded, result.output, decoding ? requests[i].want : all_ids);
    latencies.insert(latencies.end(), result.latency_us.begin(), result.latency_us.end());
  }
  sort(latencies.begin(), latencies.end());
  const auto percentile = [&](size_t p) { return latencies[(p * latencies.size() + 99) / 100 - 1]; };
  const double throughput = latencies.size() * (stripe_bytes / 1073741824.0) / seconds;
  cout << (decoding ? "decode" : "encode") << ',' << threads << ',' << requests.size() << ','
       << iterations << ',' << latencies.size() << ',' << fixed << setprecision(6)
       << seconds << ',' << throughput << ',' << percentile(50) << ',' << percentile(95)
       << ',' << percentile(99) << '\n';
}
}

int main(int argc, char** argv)
{
  if (argc != 6) {
    cerr << "Usage: " << argv[0] << " plugin-directory redis-port threads requests iterations\n";
    return 2;
  }
  vector<const char*> args;
  auto cct = global_init(nullptr, args, CEPH_ENTITY_TYPE_CLIENT, CODE_ENVIRONMENT_UTILITY_NODOUT,
                         CINIT_FLAG_NO_DEFAULT_CONFIG_FILE | CINIT_FLAG_NO_DAEMON_ACTIONS);
  common_init_finish(g_ceph_context);
  try {
    const string directory = argv[1];
    const int port = positive_int(argv[2], 65535);
    const int threads = positive_int(argv[3], 64);
    const int clients = positive_int(argv[4], 64);
    const int iterations = positive_int(argv[5], 100000);
    auto code = load(directory, port, threads);
    check_contract(directory, port, *code);
    auto reference = load(directory, port, 1);
    vector<Request> requests;
    requests.reserve(clients);
    for (int i = 0; i < clients; ++i) {
      auto& request = requests.emplace_back();
      request.raw = random_input(stripe_bytes, 1 + i);
      check(reference->encode(all_ids, request.raw, &request.encoded) == 0, "baseline encode failed");
      request.want = {i % k};
      request.helpers = select_helpers(*code, request.encoded, request.want);
    }
    cout << "operation,threads,requests,iterations_per_request,calls,wall_seconds,"
            "stripe_GiB_s,p50_us,p95_us,p99_us\n";
    run_concurrent(*code, requests, threads, iterations, false);
    run_concurrent(*code, requests, threads, iterations, true);
  } catch (const exception& error) {
    cerr << error.what() << '\n';
    return 1;
  }
}
