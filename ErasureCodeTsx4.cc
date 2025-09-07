// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2014 Cloudwatt <libre.licensing@cloudwatt.com>
 * Copyright (C) 2014 Red Hat <contact@redhat.com>
 *
 * Author: Loic Dachary <loic@dachary.org>
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 */

#include <cerrno>
// #include <algorithm>
// #include <unordered_map>
#include <immintrin.h>
// #include <array>
// #include <optional>
// #include <vector>

#include "common/debug.h"
#include "include/stringify.h"
#include "erasure-code/ErasureCodePlugin.h"

#include "ErasureCodeTsx4.h"

#define dout_context g_ceph_context
#define dout_subsys ceph_subsys_osd
#undef dout_prefix
#define dout_prefix _prefix(_dout)

#define Kval 10 // or 12
#define Mval 4
#define SLICE 32
#define SLICE_EXP 5
#define EXTRA_DATA_SIZE 448 // (Kval - 1) * SLICE
#define EXTRA_SLICE_CNT 7
#define ENABLE_ENCODE 1
#define ENABLE_DECODE 1
#define ENABLE_LOG 0
// !important: window_size >= k - 1 = 9
// window_size <= (k - 1) * 2 = 18
#define ENCODE_WINDOW_SIZE 16
#define DECODE_WINDOW_SIZE 24

#if ENABLE_LOG
#include <fstream>
#include <chrono>
#endif

using namespace std;
using namespace ceph;

static ostream &_prefix(std::ostream *_dout)
{
  return *_dout << "ErasureCodeTsx4: ";
}

class ParityChunkPtr
{
public:
  unsigned blocksize;
  uint8_t *chunk_ptr;
  uint8_t *extradata;

  ParityChunkPtr(long blocksize, uint8_t *chunk_ptr, uint8_t *extradata = nullptr)
      : blocksize(blocksize), chunk_ptr(chunk_ptr), extradata(extradata), zeros(nullptr)
  {

    if (chunk_ptr == nullptr)
    {
      zeros = (uint8_t *)aligned_alloc(SLICE, SLICE);
      _mm256_store_si256((__m256i *)zeros, _mm256_setzero_si256());
    }
  }

  ~ParityChunkPtr()
  {
    if (zeros != nullptr)
    {
      free(zeros);
    }
  }

  uint8_t *operator[](size_t idx)
  {
    if (chunk_ptr == nullptr)
    {
      return zeros;
    }
    if (idx >= blocksize)
    {
      return &extradata[idx - blocksize];
    }
    return &chunk_ptr[idx];
  }

  void set_extradata(uint8_t *extradata)
  {
    this->extradata = extradata;
  }

private:
  uint8_t *zeros;
};

class PadChunkPtr
{
public:
  int blocksize;
  uint8_t *chunk_ptr;

  PadChunkPtr(int blocksize, uint8_t *chunk_ptr)
      : blocksize(blocksize), chunk_ptr(chunk_ptr)
  {
    // unimportant for maperasure
    unimportant = (uint8_t *)aligned_alloc(SLICE, SLICE);
    // while for pad_data, it must be zeros
    _mm256_store_si256((__m256i *)unimportant, _mm256_setzero_si256());
  }

  uint8_t *operator[](int idx)
  {
    if (idx < 0 || idx >= blocksize)
    {
      return unimportant;
    }
    else
    {
      // return chunk_ptr? &chunk_ptr[idx]: unimportant;
      return &chunk_ptr[idx];
    }
  }

private:
  uint8_t *unimportant;
};

int ErasureCodeTsx4::init(ErasureCodeProfile &profile, ostream *ss)
{
  int err = 0;
  err |= parse(profile, ss);
  // err |= to_int("k", profile, &k, "8", ss);
  err |= to_int("k", profile, &k, "10", ss);
  err |= to_int("m", profile, &m, "4", ss);
  // chunk_count = k + m;
  if (err)
    return err;

  rctx = redisConnect("127.0.0.1", 6379);
  // rctx = redisAsyncConnect("127.0.0.1", 6379);
  if (rctx == NULL || rctx->err)
  {
    dout(0) << "Error Redis: " << rctx->errstr << dendl;
    return 1;
  }

  return ErasureCode::init(profile, ss);
}

unsigned int ErasureCodeTsx4::get_chunk_size(unsigned int object_size) const
{
  // return (object_size + k - 1) / k;
  return (object_size + k - 1) / k;
}

int ErasureCodeTsx4::encode_chunks(const set<int> &want_to_encode,
                                   map<int, bufferlist> *encoded)
{

  int kval = k;
  uint8_t *chunks[kval + Mval];
  for (int i = 0; i < kval + Mval; ++i)
  {
    chunks[i] = (uint8_t *)(*encoded)[i].c_str();
  }

  // const int k = k;
  int blocksize = (*encoded)[0].length();

#if ENABLE_ENCODE

#if ENABLE_LOG
  std::ofstream encode_log("tsx-encode.log", std::ios::app);
  static int encode_call_count = -1;
  encode_call_count++;
  static double total_encode_time_ms = 0;
  auto encode_start = std::chrono::high_resolution_clock::now();
#endif

  // const int SLICE = 32;
  const int TOTAL_SLICE_NUM = blocksize / SLICE;
  const int window_size = ENCODE_WINDOW_SIZE;
  const int windows_num = TOTAL_SLICE_NUM / window_size;

  uint8_t *parity_buffer = (uint8_t *)aligned_alloc(32, 3 * EXTRA_DATA_SIZE);
  ParityChunkPtr parity1(blocksize, chunks[kval], parity_buffer);
  ParityChunkPtr parity2(blocksize, chunks[kval + 1]);
  ParityChunkPtr parity3(blocksize, chunks[kval + 2], parity_buffer + EXTRA_DATA_SIZE);
  ParityChunkPtr parity4(blocksize, chunks[kval + 3], parity_buffer + 2 * EXTRA_DATA_SIZE);

  // char parity_key[SLICE];

  uint64_t slice_offset_1;
  uint64_t slice_offset_2;
  uint64_t slice_offset_3;
  uint64_t slice_offset_4;
  __m256i mxx1, mxx2, mxx3, mxx4, res1, res3, tmp;

  for (int window_id = 0; window_id < windows_num; window_id++)
  {
    const int slice_id_start = window_id * window_size;
    const int slice_id_end = slice_id_start + window_size;

    // parity 4 after pairty 3
    for (int slice_id = slice_id_start; slice_id < slice_id_end; slice_id++)
    {
      slice_offset_1 = slice_id * SLICE;
      slice_offset_2 = slice_id * SLICE;
      slice_offset_3 = (slice_id + kval - 2) * SLICE;     // kval - 1 - 2
      slice_offset_4 = (slice_id + kval - 1) * 2 * SLICE; // kval - 1

      // Load two data chunks into registers and XOR them
      mxx1 = _mm256_load_si256((__m256i *)&chunks[0][slice_offset_2]);
      mxx2 = _mm256_load_si256((__m256i *)&chunks[1][slice_offset_2]);
      res1 = _mm256_xor_si256(mxx1, mxx2);

      // Store the result into parity chunks
      _mm256_store_si256((__m256i *)parity2[slice_offset_2], res1);

      if (slice_id == slice_id_start)
      {
        res1 = mxx1;
        res3 = mxx2;
      }
      else
      {
        res1 = _mm256_xor_si256(mxx1, mxx4);
        res3 = _mm256_xor_si256(mxx2, mxx3);
      }
      if (window_id != 0)
      {
        if ((slice_id % window_size) < kval - 1)
        {
          tmp = _mm256_load_si256((__m256i *)parity1[slice_offset_1]);
          res1 = _mm256_xor_si256(res1, tmp);
        }
        if (slice_id == slice_id_start)
        {
          tmp = _mm256_load_si256((__m256i *)parity3[slice_offset_3]);
          res3 = _mm256_xor_si256(res3, tmp);
        }
      }

      _mm256_store_si256((__m256i *)parity1[slice_offset_1], res1);
      _mm256_store_si256((__m256i *)parity3[slice_offset_3], res3);

      if (slice_id != slice_id_end - 1)
      {
        mxx3 = mxx1;
        mxx4 = mxx2;
      }
      else
      {
        // [!] Assert window_size >= k - 1, or overwriting occurs
        _mm256_store_si256((__m256i *)parity1[slice_offset_1 + SLICE], mxx2);
        _mm256_store_si256((__m256i *)parity3[slice_offset_3 + SLICE], mxx1);
      }

      // chunk1 for parity4
      if (window_id != 0 || slice_id >= slice_id_start + 2)
      {
        tmp = _mm256_load_si256((__m256i *)parity4[slice_offset_4 - 2 * SLICE]);
        mxx2 = _mm256_xor_si256(mxx2, tmp);
      }
      // store chunk0 to pairty4
      _mm256_store_si256((__m256i *)parity4[slice_offset_4], mxx1);
      // store chunk1 to pairty4
      _mm256_store_si256((__m256i *)parity4[slice_offset_4 - 2 * SLICE], mxx2);
    }

    for (int chunk_idx = 2; chunk_idx < k; chunk_idx += 2)
    {
      for (int slice_id = slice_id_start; slice_id < slice_id_end; slice_id++)
      {
        slice_offset_2 = slice_id * SLICE;
        slice_offset_1 = (slice_id + chunk_idx) * SLICE;
        slice_offset_3 = (slice_id + (kval - 2 - chunk_idx)) * SLICE;     // kval - 2
        slice_offset_4 = (slice_id + (kval - 1 - chunk_idx)) * 2 * SLICE; // kval - 1

        // Load two data chunks into registers and XOR them
        mxx1 = _mm256_load_si256((__m256i *)&chunks[chunk_idx][slice_offset_2]);
        mxx2 = _mm256_load_si256((__m256i *)&chunks[chunk_idx + 1][slice_offset_2]);
        res1 = _mm256_xor_si256(mxx1, mxx2);

        // Store the result into parity chunks
        tmp = _mm256_load_si256((__m256i *)parity2[slice_offset_2]);
        res1 = _mm256_xor_si256(res1, tmp);
        _mm256_store_si256((__m256i *)parity2[slice_offset_2], res1);

        if (slice_id == slice_id_start)
        {
          res1 = mxx1;
          res3 = mxx2;
        }
        else
        {
          res1 = _mm256_xor_si256(mxx1, mxx4);
          res3 = _mm256_xor_si256(mxx2, mxx3);
        }

        if (slice_id > 1)
        {
          tmp = _mm256_load_si256((__m256i *)parity3[slice_offset_3]);
          res3 = _mm256_xor_si256(res3, tmp);
        }
        _mm256_store_si256((__m256i *)parity3[slice_offset_3], res3);

        if (slice_id != slice_id_end - 1)
        {
          tmp = _mm256_load_si256((__m256i *)parity1[slice_offset_1]);
          res1 = _mm256_xor_si256(res1, tmp);
          _mm256_store_si256((__m256i *)parity1[slice_offset_1], res1);
          mxx3 = mxx1;
          mxx4 = mxx2;
        }
        else
        {
          _mm256_store_si256((__m256i *)parity1[slice_offset_1], res1);
          // the slice of shift end
          _mm256_store_si256((__m256i *)parity1[slice_offset_1 + SLICE], mxx2);
          tmp = _mm256_load_si256((__m256i *)parity3[slice_offset_3 + SLICE]);
          res3 = _mm256_xor_si256(mxx1, tmp);
          _mm256_store_si256((__m256i *)parity3[slice_offset_3 + SLICE], res3);
        }

        // chunk01 for parity4
        if (window_id != 0 || slice_id >= slice_id_start + 2)
        {
          tmp = _mm256_load_si256((__m256i *)parity4[slice_offset_4]);
          mxx1 = _mm256_xor_si256(mxx1, tmp);
          tmp = _mm256_load_si256((__m256i *)parity4[slice_offset_4 - 2 * SLICE]);
          mxx2 = _mm256_xor_si256(mxx2, tmp);
        }
        // store chunk01 to pairty4
        _mm256_store_si256((__m256i *)parity4[slice_offset_4], mxx1);
        _mm256_store_si256((__m256i *)parity4[slice_offset_4 - 2 * SLICE], mxx2);
      }
    }

    // parity4 before parity 1
    // for (int slice_id = slice_id_start; slice_id < slice_id_end; slice_id++)
    // {

    //   // chunk 0
    //   slice_offset_1 = slice_id * SLICE;
    //   slice_offset_4 = slice_id * SLICE;
    //   mxx1 = _mm256_load_si256((__m256i *)&chunks[0][slice_offset_1]);
    //   // window size <= 2 * (9 - 1)
    //   if (window_id != 0)
    //   {
    //     tmp = _mm256_load_si256((__m256i *)parity4[slice_offset_4]);
    //     mxx1 = _mm256_xor_si256(mxx1, tmp);
    //   }
    //   _mm256_store_si256((__m256i *)parity4[slice_offset_4], mxx1);

    //   for (int chunk_idx = 1; chunk_idx < k; chunk_idx++)
    //   {
    //     slice_offset_1 = slice_id * SLICE;
    //     slice_offset_4 = (slice_id + 2 * chunk_idx) * SLICE;
    //     mxx1 = _mm256_load_si256((__m256i *)&chunks[chunk_idx][slice_offset_1]);
    //     if (slice_id < slice_id_end - 2)
    //     {
    //       tmp = _mm256_load_si256((__m256i *)parity4[slice_offset_4]);
    //       mxx1 = _mm256_xor_si256(mxx1, tmp);
    //     }
    //     _mm256_store_si256((__m256i *)parity4[slice_offset_4], mxx1);
    //   }
    // }
    // if (window_id == 0) {}
  }

  // memcpy(parity_buffer, parity1.get_extradata(), EXTRA_DATA_SIZE);
  // memcpy(parity_buffer + EXTRA_DATA_SIZE, parity3.get_extradata(), EXTRA_DATA_SIZE);
  redisReply *reply = (redisReply *)redisCommand(
      rctx, "SET %b %b", parity2[0], SLICE, parity_buffer, 3 * EXTRA_DATA_SIZE);
  freeReplyObject(reply);

#if ENABLE_LOG
  auto encode_end = std::chrono::high_resolution_clock::now();
  double encode_duration = std::chrono::duration_cast<std::chrono::microseconds>(encode_end - encode_start).count() / 1000.0;
  total_encode_time_ms += encode_duration;

  encode_log << "encode_call_count: " << encode_call_count
             << ", blocksize: " << blocksize
             << ", total_time_ms: " << total_encode_time_ms << "\n";

  // encode_log << "chunks[0][i] after encoding at: " << encode_call_count << "\n";
  // for (unsigned i = 0; i < blocksize; ++i)
  // {
  //   encode_log << std::hex << std::setw(2) << std::setfill('0') << (int)chunks[0][i] << " ";
  // }
  // encode_log << "\n";
  // encode_log << std::dec << "blocksize: " << blocksize << std::endl;
  encode_log << std::dec << "parity1 after encoding at: " << encode_call_count << "\n";
  for (int i = 0; i < blocksize + EXTRA_DATA_SIZE; ++i)
  {
    encode_log << std::hex
               << std::setw(2) << std::setfill('0')
               << static_cast<int>(*(parity1[i])) << " ";
  }
  encode_log << "\n";

  encode_log.close();

#endif

  return 0;

#else // ENABLE_ENCODE

  return 0;

#endif
}

int ErasureCodeTsx4::decode_chunks(const set<int> &want_to_read,
                                   const map<int, bufferlist> &chunks,
                                   map<int, bufferlist> *decoded)
{

#if ENABLE_LOG
  std::ofstream decode_log("tsx-decode.log", std::ios::app);
  static int decode_call_count = -1;
  decode_call_count++;
  static double total_decode_time_ms = 0;
  auto decode_start = std::chrono::high_resolution_clock::now();
#endif

  const int kval = k; // member -> local var
  int blocksize = (*chunks.begin()).second.length();

  int erasure_data[Mval];
  int erasure_data_count = 0;
  int retension_data[kval + 1];
  int retension_data_count = 0;
  int retension_coding_count = 0;

  uint8_t *data[kval];
  vector<PadChunkPtr> pad_data;
  uint8_t *coding[Mval];

  int coding_data_map[Mval] = {0, 0, kval - 1, kval - 1};
  // int need_decoded[Mval] = {-1, -1, -1, -1}; // distinguish erased data or coding
  uint8_t *maperasure_ptr[Mval] = {nullptr, nullptr, nullptr, nullptr};
  uint8_t *parity_ptr[Mval] = {nullptr, nullptr, nullptr, nullptr}; // used to decode erased data

  for (int i = 0; i < kval + Mval; i++)
  {
    if (i < kval)
    {
      data[i] = (uint8_t *)(*decoded)[i].c_str();
      pad_data.emplace_back(blocksize, data[i]);
      if (chunks.find(i) == chunks.end())
      {
        erasure_data[erasure_data_count] = i;
        erasure_data_count++;
      }
      else
      {
        retension_data[retension_data_count] = i;
        retension_data_count++;
      }
    }
    else
    {
      int parity_idx = i - kval;
      coding[parity_idx] = (uint8_t *)(*decoded)[i].c_str();
      if (chunks.find(i) == chunks.end())
      {
        maperasure_ptr[parity_idx] = coding[parity_idx];
      }
      else
      {
        if (retension_coding_count < erasure_data_count)
        {
          // need_decoded[parity_idx] = (coding_data_map[parity_idx] = erasure_data[retension_coding_count]);
          coding_data_map[parity_idx] = erasure_data[retension_coding_count];
          maperasure_ptr[parity_idx] = data[coding_data_map[parity_idx]];
          parity_ptr[parity_idx] = coding[parity_idx];
        }
        retension_coding_count++;
      }
    }
  }

  retension_data[retension_data_count] = -1;

#if ENABLE_DECODE

  PadChunkPtr maperasure[3] = {
      PadChunkPtr(blocksize, maperasure_ptr[0]),
      PadChunkPtr(blocksize, maperasure_ptr[1]),
      PadChunkPtr(blocksize, maperasure_ptr[2]),
      PadChunkPtr(blocksize, maperasure_ptr[3]),
  };
  ParityChunkPtr parity[3]{
      {blocksize, parity_ptr[0]},
      {blocksize, parity_ptr[1]},
      {blocksize, parity_ptr[2]},
      {blocksize, parity_ptr[3]},
  };

  char *parity_buffer = (char *)aligned_alloc(32, 3 * EXTRA_DATA_SIZE);
  // ParityChunkPtr parity[3]{
  //     {blocksize, parity_ptr[0], (uint8_t *)(parity_buffer)},
  //     {blocksize, parity_ptr[1], (uint8_t *)(parity_buffer)},
  //     {blocksize, parity_ptr[2], (uint8_t *)(parity_buffer + EXTRA_DATA_SIZE)},
  // };

  const int TOTAL_SLICE_NUM = blocksize / SLICE;
  const int window_size = DECODE_WINDOW_SIZE;
  const int windows_num = TOTAL_SLICE_NUM / window_size;

  long slice_offset[4];
  __m256i mxx0, mxx1, mxx2, mxx3, tmp, res;

  // solve the key -> use first kval slices to solve erased first slice
  // for (int solve_key = 1; solve_key >= 0; solve_key--)
  for (int window_id = -1; window_id <= windows_num; window_id++)
  {

    // init maperasure by parity
    if (window_id < windows_num)
    {
      const int slice_id_start = (window_id + 1) * window_size;
      const int slice_id_end = slice_id_start + window_size;

      // slice_id = slice_id_start
      slice_offset[0] = (slice_id_start - coding_data_map[0]) * SLICE;
      slice_offset[1] = slice_id_start * SLICE;
      slice_offset[2] = (slice_id_start - (kval - 1) + coding_data_map[2]) * SLICE;
      slice_offset[3] = (slice_id_start - (kval - 1) + coding_data_map[3]) * 2 * SLICE;

      for (int i = 0; i < 4; i++)
      {
        if (maperasure_ptr[i] != nullptr)
        {
          for (int slice_id = slice_id_start; slice_id < slice_id_end; slice_id++)
          {
            tmp = _mm256_load_si256((__m256i *)parity[i][slice_id * SLICE]);
            _mm256_store_si256((__m256i *)(maperasure[i])[slice_offset[i]], tmp);
            slice_offset[i] += SLICE;
          }
        }
      }
    }

    // substitute
    if (0 <= window_id && window_id <= windows_num)
    {
      const int slice_id_start = window_id * window_size;
      const int slice_id_end = slice_id_start + window_size;

      for (int retension_data_idx = 0; retension_data_idx < retension_data_count; retension_data_idx++)
      {

        int retension_data_chunk_idx = retension_data[retension_data_idx];
        if (retension_data[retension_data_idx + 1] - retension_data_chunk_idx == 1)
        {
          mxx2 = _mm256_load_si256((__m256i *)pad_data[retension_data_chunk_idx][(slice_id_start - 1) * SLICE]);
          mxx3 = _mm256_load_si256((__m256i *)pad_data[retension_data_chunk_idx + 1][(slice_id_start - 1) * SLICE]);
          for (int slice_id = slice_id_start; slice_id < slice_id_end; slice_id++)
          {

            slice_offset[1] = slice_id * SLICE;
            slice_offset[0] = (slice_id + retension_data_chunk_idx - coding_data_map[0]) * SLICE;
            slice_offset[2] = (slice_id + coding_data_map[2] - retension_data_chunk_idx - 1) * SLICE; // for retension_data_chunk_idx + 1
            slice_offset[3] = (slice_id + coding_data_map[3] - retension_data_chunk_idx) * 2 * SLICE; // for retension_data_chunk_idx

            mxx0 = _mm256_load_si256((__m256i *)pad_data[retension_data_chunk_idx][slice_offset[1]]);
            mxx1 = _mm256_load_si256((__m256i *)pad_data[retension_data_chunk_idx + 1][slice_offset[1]]);

            if (maperasure_ptr[0] != nullptr)
            {

              res = _mm256_xor_si256(mxx0, mxx3);

              tmp = _mm256_load_si256((__m256i *)maperasure[0][slice_offset[0]]);
              res = _mm256_xor_si256(res, tmp);
              _mm256_store_si256((__m256i *)maperasure[0][slice_offset[0]], res);
            }
            if (maperasure_ptr[1] != nullptr)
            {

              res = _mm256_xor_si256(mxx1, mxx0);
              tmp = _mm256_load_si256((__m256i *)maperasure[1][slice_offset[1]]);
              res = _mm256_xor_si256(res, tmp);
              _mm256_store_si256((__m256i *)maperasure[1][slice_offset[1]], res);
            }
            if (maperasure_ptr[2] != nullptr)
            {

              res = _mm256_xor_si256(mxx1, mxx2);

              tmp = _mm256_load_si256((__m256i *)maperasure[2][slice_offset[2]]);
              res = _mm256_xor_si256(res, tmp);
              _mm256_store_si256((__m256i *)maperasure[2][slice_offset[2]], res);
            }
            if (maperasure_ptr[3] != nullptr)
            {
              tmp = _mm256_load_si256((__m256i *)maperasure[3][slice_offset[3]]);
              res = _mm256_xor_si256(mxx0, tmp);
              _mm256_store_si256((__m256i *)maperasure[3][slice_offset[3]], res);

              tmp = _mm256_load_si256((__m256i *)maperasure[3][slice_offset[3] - 2 * SLICE]);
              res = _mm256_xor_si256(mxx1, tmp);
              _mm256_store_si256((__m256i *)maperasure[3][slice_offset[3] - 2 * SLICE], res);
            }

            mxx2 = mxx0;
            mxx3 = mxx1;
          }

          retension_data_idx++;
        }
        else
        {
          for (int slice_id = slice_id_start; slice_id < slice_id_end; slice_id++)
          {

            slice_offset[1] = slice_id * SLICE;

            mxx1 = _mm256_load_si256((__m256i *)pad_data[retension_data_chunk_idx][slice_offset[1]]);

            slice_offset[0] = (slice_id + retension_data_chunk_idx - coding_data_map[0]) * SLICE;
            slice_offset[2] = (slice_id + coding_data_map[2] - retension_data_chunk_idx) * SLICE;
            slice_offset[3] = (slice_id + coding_data_map[3] - retension_data_chunk_idx) * 2 * SLICE;

            for (int i = 0; i < 4; i++)
            {
              if (maperasure_ptr[i] != nullptr)
              {
                tmp = _mm256_load_si256((__m256i *)(maperasure[i])[slice_offset[i]]);
                mxx2 = _mm256_xor_si256(mxx1, tmp);
                _mm256_store_si256((__m256i *)(maperasure[i])[slice_offset[i]], mxx2);
              }
            }
          }
        }
      }
    }

    // elimination
    if (window_id > 0)
    {
      const int solve_slice_id_start = (window_id - 1) * window_size;
      const int solve_slice_id_end = solve_slice_id_start + window_size;

      for (int slice_id = solve_slice_id_start; slice_id < solve_slice_id_end; slice_id++)
      {
        slice_offset[1] = slice_id * SLICE;
        // constexpr int two_tone_idx[5] = {0, 2, 1, 0, 2};
        constexpr int two_tone_idx[7] = {0, 3, 2, 1, 0, 3, 2};
        for (int i = 0; i < 4; i++)
        {

          int maperasure_idx = two_tone_idx[i];
          if (parity_ptr[maperasure_idx] != nullptr)
          {
            if (maperasure_idx == 3)
            {
              slice_id++;
            }
            mxx1 = _mm256_load_si256((__m256i *)&pad_data[coding_data_map[maperasure_idx]][slice_offset[1]]);

            slice_offset[0] = (slice_id + coding_data_map[maperasure_idx] - coding_data_map[0]) * SLICE;
            slice_offset[2] = (slice_id + coding_data_map[2] - coding_data_map[maperasure_idx]) * SLICE;
            slice_offset[3] = (slice_id + coding_data_map[3] - coding_data_map[maperasure_idx]) * 2 * SLICE;

            for (int j = 1; j < 4; j++)
            {

              int other_idx = two_tone_idx[i + j];
              if (maperasure_ptr[other_idx] != nullptr)
              {

                tmp = _mm256_load_si256((__m256i *)(maperasure[other_idx])[slice_offset[other_idx]]);
                mxx2 = _mm256_xor_si256(mxx1, tmp);
                _mm256_store_si256((__m256i *)(maperasure[other_idx])[slice_offset[other_idx]], mxx2);
              }
            }

            if (maperasure_idx == 3)
            {
              slice_id--;
            }
          }
        }
      }
    }

    if (window_id == 1)
    {
      // mxx1 = _mm256_setzero_si256();
      // char *parity_key = (char *)aligned_alloc(32, SLICE);
      // for (int i = 0; i < k; i++)
      // {
      //   tmp = _mm256_load_si256((__m256i *)&data[i][0]);
      //   mxx1 = _mm256_xor_si256(mxx1, tmp);
      // }
      // _mm256_store_si256((__m256i *)parity_key, mxx1);

      // redisReply *reply = (redisReply *)redisCommand(
      //     rctx, "GET %b", parity_key, SLICE);
      redisReply *reply = (redisReply *)redisCommand(
          rctx, "GET %b", &coding[1][0], SLICE);
      memcpy(parity_buffer, reply->str, reply->len);

      parity[0].set_extradata((uint8_t *)(parity_buffer));
      // avoid out of bounds of parity[1]
      parity[1].set_extradata((uint8_t *)(parity_buffer));
      parity[2].set_extradata((uint8_t *)(parity_buffer + EXTRA_DATA_SIZE));

      freeReplyObject(reply);
    }
  }

  free(parity_buffer);

  return 0;

#else // ENABLE_DECODE

  return 0;

#endif
}

int ErasureCodeTsx4::decode_one_data_chunk(uint8_t **data, uint8_t *coding, int blocksize, int erasure_idx)
{
  const int TOTAL_SLICE_NUM = blocksize / SLICE;
  const int kval = k;
  const int UNROLL = 4;

  __m256i sum[UNROLL];
  __m256i tmp[UNROLL];

  uint8_t *tmp_ptr = data[erasure_idx];
  data[erasure_idx] = coding;
  coding = tmp_ptr;

  int slice_id = 0;
  int slice_offset[UNROLL];
  // Unrolled loop
  for (; slice_id + UNROLL - 1 < TOTAL_SLICE_NUM; slice_id += UNROLL)
  {
    for (int u = 0; u < UNROLL; ++u)
      slice_offset[u] = (slice_id + u) << SLICE_EXP;

    // Load first data chunk for each unrolled iteration
    for (int u = 0; u < UNROLL; ++u)
      sum[u] = _mm256_load_si256((__m256i *)&data[0][slice_offset[u]]);

    // XOR with remaining data chunks
    for (int data_idx = 1; data_idx < kval; data_idx++)
    {
      for (int u = 0; u < UNROLL; ++u)
      {
        tmp[u] = _mm256_load_si256((__m256i *)&data[data_idx][slice_offset[u]]);
      }
      for (int u = 0; u < UNROLL; ++u)
      {
        sum[u] = _mm256_xor_si256(sum[u], tmp[u]);
      }
    }

    // Store result
    for (int u = 0; u < UNROLL; ++u)
    {
      _mm256_stream_si256((__m256i *)&coding[slice_offset[u]], sum[u]);
    }
  }

  // Handle remaining slices
  for (; slice_id < TOTAL_SLICE_NUM; ++slice_id)
  {
    int offset = slice_id << SLICE_EXP;
    __m256i sum1 = _mm256_load_si256((__m256i *)&data[0][offset]);
    for (int data_idx = 1; data_idx < kval; data_idx++)
    {
      __m256i tmp1 = _mm256_load_si256((__m256i *)&data[data_idx][offset]);
      sum1 = _mm256_xor_si256(sum1, tmp1);
    }
    _mm256_stream_si256((__m256i *)&coding[offset], sum1);
  }

  return 0;
}

int ErasureCodeTsx4::reencode_one_coding_chunk(uint8_t **data, uint8_t *coding, int blocksize, int slice_delta)
{
  if (slice_delta == 0)
  {
    uint8_t *tmp_ptr = data[0];
    data[0] = coding;
    coding = tmp_ptr;
    return decode_one_data_chunk(data, coding, blocksize, 0);
  }
  else if (slice_delta == -1)
  {
    const int kval = k;
    __m256i sum, tmp;
    for (int slice_id = 0; slice_id < kval - 1; slice_id++)
    {
      sum = _mm256_load_si256((__m256i *)data[0]);
      data[0] += SLICE;
      for (int data_idx = 1; data_idx <= slice_id; data_idx++)
      {
        tmp = _mm256_load_si256((__m256i *)data[data_idx]);
        data[data_idx] += SLICE;
        sum = _mm256_xor_si256(sum, tmp);
      }
      _mm256_stream_si256((__m256i *)coding, sum);
      coding += SLICE;
    }

    uint8_t *tmp_ptr = data[0];
    data[0] = coding;
    coding = tmp_ptr;
    return decode_one_data_chunk(data, coding, blocksize - ((kval - 1) << SLICE_EXP), 0);
  }
  else if (slice_delta == 1)
  {
    const int kval = k;
    __m256i sum, tmp;
    for (int slice_id = 0; slice_id < kval - 1; slice_id++)
    {
      sum = _mm256_load_si256((__m256i *)data[kval - 1]);
      data[kval - 1] += SLICE;
      for (int data_idx = kval - 2; data_idx >= kval - 1 - slice_id; data_idx--)
      {
        tmp = _mm256_load_si256((__m256i *)data[data_idx]);
        data[data_idx] += SLICE;
        sum = _mm256_xor_si256(sum, tmp);
      }
      _mm256_stream_si256((__m256i *)coding, sum);
      coding += SLICE;
    }

    uint8_t *tmp_ptr = data[0];
    data[0] = coding;
    coding = tmp_ptr;
    return decode_one_data_chunk(data, coding, blocksize - ((kval - 1) << SLICE_EXP), 0);
  }
  return 0;
}