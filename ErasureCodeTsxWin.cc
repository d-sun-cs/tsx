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
#include <immintrin.h>

#include "common/debug.h"
#include "include/stringify.h"
#include "erasure-code/ErasureCodePlugin.h"

#include "ErasureCodeTsxWin.h"

#define dout_context g_ceph_context
#define dout_subsys ceph_subsys_osd
#undef dout_prefix
#define dout_prefix _prefix(_dout)

// #define Kval 8 // to be standardized
#define Mval 3 // to be standardized
#define SLICE 32
#define SLICE_EXP 5
#define EXTRA_DATA_SIZE 224 // (Kval - 1) * SLICE
// #define EXTRA_SLICE_CNT 7
#define PARITY_CHUNKS 3
#define ENABLE_ENCODE 1
#define ENABLE_DECODE 1
#define ENABLE_LOG 0
#define ENCODE_WINDOW_SIZE 16
#define DECODE_WINDOW_SIZE 16

#if ENABLE_LOG
#include <fstream>
#include <chrono>
#endif

using namespace std;
using namespace ceph;

static ostream &_prefix(std::ostream *_dout)
{
  return *_dout << "ErasureCodeTsxWin: ";
}

class ParityChunkPtr
{
public:
  int blocksize;
  uint8_t *chunk_ptr;
  uint8_t *extradata;

  ParityChunkPtr(int blocksize, uint8_t *chunk_ptr, uint8_t *extradata = nullptr)
      : blocksize(blocksize), chunk_ptr(chunk_ptr), extradata(extradata), zeros(nullptr)
  {

    // if (chunk_ptr == nullptr)
    // {
    zeros = (uint8_t *)aligned_alloc(SLICE, SLICE);
    _mm256_store_si256((__m256i *)zeros, _mm256_setzero_si256());
    // }
  }

  ~ParityChunkPtr()
  {
    // if (zeros != nullptr)
    // {
    free(zeros);
    // }
  }

  uint8_t *operator[](int idx)
  {
    if (idx < 0 || idx >= blocksize + EXTRA_DATA_SIZE || chunk_ptr == nullptr)
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

  void set_chunk_ptr(uint8_t *chunk_ptr)
  {
    this->chunk_ptr = chunk_ptr;
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
    unimportant = (uint8_t *)aligned_alloc(SLICE, SLICE);
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

int ErasureCodeTsxWin::init(ErasureCodeProfile &profile, ostream *ss)
{
  int err = 0;
  err |= parse(profile, ss);
  err |= to_int("k", profile, &k, "8", ss);
  err |= to_int("m", profile, &m, "3", ss);
  if (err)
    return err;

  rctx = redisConnect("127.0.0.1", 6379);
  if (rctx == NULL || rctx->err)
  {
    dout(0) << "Error Redis: " << rctx->errstr << dendl;
    return 1;
  }

  return ErasureCode::init(profile, ss);
}

unsigned int ErasureCodeTsxWin::get_chunk_size(unsigned int object_size) const
{
  return (object_size + k - 1) / k;
}

int ErasureCodeTsxWin::encode_chunks(const set<int> &want_to_encode,
                                     map<int, bufferlist> *encoded)
{
  const int kval = k; // member -> local var
  uint8_t *chunks[kval + Mval];
  for (int i = 0; i < kval + Mval; ++i)
  {
    chunks[i] = (uint8_t *)(*encoded)[i].c_str();
  }

  int blocksize = (*encoded)[0].length();

#if ENABLE_ENCODE

#if ENABLE_LOG
  std::ofstream encode_log("tsx-encode.log", std::ios::app);
  static int encode_call_count = -1;
  encode_call_count++;
  static double total_encode_time_ms = 0;
  auto encode_start = std::chrono::high_resolution_clock::now();
#endif

  const int TOTAL_SLICE_NUM = blocksize / SLICE;
  const int window_size = ENCODE_WINDOW_SIZE;
  const int windows_num = TOTAL_SLICE_NUM / window_size;

  uint8_t *parity_buffer = (uint8_t *)aligned_alloc(32, 2 * EXTRA_DATA_SIZE);
  ParityChunkPtr parity1(blocksize, chunks[kval], parity_buffer);
  PadChunkPtr parity2(blocksize, chunks[kval + 1]);
  ParityChunkPtr parity3(blocksize, chunks[kval + 2], parity_buffer + EXTRA_DATA_SIZE);

  PadChunkPtr data0(blocksize, chunks[0]);

  uint64_t slice_offset_1;
  uint64_t slice_offset_2;
  uint64_t slice_offset_3;
  __m256i mxx1, mxx2;

  for (int window_id = -1; window_id <= windows_num; window_id++)
  {
    // chunk 0
    // window size >= 2 * (k - 1)
    const int slice_id_start = window_id * window_size;
    const int slice_id_end = slice_id_start + window_size;
    for (int slice_id = slice_id_start; slice_id < slice_id_end; slice_id++)
    {
      slice_offset_1 = slice_id * SLICE;
      slice_offset_2 = slice_id * SLICE;
      slice_offset_3 = (slice_id + kval - 1) * SLICE;

      mxx1 = _mm256_loadu_si256((__m256i *)data0[slice_offset_2]);
      _mm256_storeu_si256((__m256i *)parity1[slice_offset_1], mxx1);
      _mm256_storeu_si256((__m256i *)parity2[slice_offset_2], mxx1);
      _mm256_storeu_si256((__m256i *)parity3[slice_offset_3], mxx1);
    }

    // chunk 1 ~ (k - 1)
    if (window_id > 0)
    {
      const int slice_id_start = (window_id - 1) * window_size;
      const int slice_id_end = slice_id_start + window_size;
      for (int chunk_idx = 1; chunk_idx < kval; chunk_idx++)
      {
        for (int slice_id = slice_id_start; slice_id < slice_id_end; slice_id++)
        {
          slice_offset_2 = slice_id * SLICE;
          slice_offset_1 = (slice_id + chunk_idx) * SLICE;
          slice_offset_3 = (slice_id + (kval - 1 - chunk_idx)) * SLICE;

          mxx1 = _mm256_loadu_si256((__m256i *)&chunks[chunk_idx][slice_offset_2]);

          mxx2 = _mm256_loadu_si256((__m256i *)parity1[slice_offset_1]);
          mxx2 = _mm256_xor_si256(mxx1, mxx2);
          _mm256_storeu_si256((__m256i *)parity1[slice_offset_1], mxx2);

          mxx2 = _mm256_loadu_si256((__m256i *)parity2[slice_offset_2]);
          mxx2 = _mm256_xor_si256(mxx1, mxx2);
          _mm256_storeu_si256((__m256i *)parity2[slice_offset_2], mxx2);

          mxx2 = _mm256_loadu_si256((__m256i *)parity3[slice_offset_3]);
          mxx2 = _mm256_xor_si256(mxx1, mxx2);
          _mm256_storeu_si256((__m256i *)parity3[slice_offset_3], mxx2);
        }
      }
    }
  }

  // take the xors of first slice as key
  redisReply *reply = (redisReply *)redisCommand(
      rctx, "SET %b %b", parity2[0], SLICE, parity_buffer, 2 * EXTRA_DATA_SIZE);
  freeReplyObject(reply);

#if ENABLE_LOG
  auto encode_end = std::chrono::high_resolution_clock::now();
  double encode_duration = std::chrono::duration_cast<std::chrono::microseconds>(encode_end - encode_start).count() / 1000.0;
  total_encode_time_ms += encode_duration;

  encode_log << "encode_call_count: " << encode_call_count
             << ", blocksize: " << blocksize
             << ", total_time_ms: " << total_encode_time_ms << "\n";

  encode_log.close();

#endif

  return 0;

#else // ENABLE_ENCODE == 0

  return 0;

#endif
}

int ErasureCodeTsxWin::decode_chunks(const set<int> &want_to_read,
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
  int retension_data[kval];
  int retension_data_count = 0;
  int retension_coding_count = 0;

  uint8_t *data[kval];
  uint8_t *coding[Mval];

  int coding_data_map[Mval] = {0, 0, kval - 1};
  int need_decoded[Mval] = {-1, -1, -1}; // distinguish erased data or coding
  uint8_t *maperasure_ptr[Mval] = {nullptr, nullptr, nullptr};
  uint8_t *parity_ptr[Mval] = {nullptr, nullptr, nullptr}; // used to decode erased data

  for (int i = 0; i < kval + Mval; i++)
  {
    if (i < kval)
    {
      data[i] = (uint8_t *)(*decoded)[i].c_str();
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
          need_decoded[parity_idx] = (coding_data_map[parity_idx] = erasure_data[retension_coding_count]);
          maperasure_ptr[parity_idx] = data[coding_data_map[parity_idx]];
          parity_ptr[parity_idx] = coding[parity_idx];
        }
        retension_coding_count++;
      }
    }
  }

#if ENABLE_DECODE

  PadChunkPtr maperasure[3] = {
      PadChunkPtr(blocksize, maperasure_ptr[0]),
      PadChunkPtr(blocksize, maperasure_ptr[1]),
      PadChunkPtr(blocksize, maperasure_ptr[2])};
  ParityChunkPtr parity[3]{
      {blocksize, parity_ptr[0]},
      {blocksize, parity_ptr[1]},
      {blocksize, parity_ptr[2]},
  };

  char *parity_buffer = (char *)aligned_alloc(32, 2 * EXTRA_DATA_SIZE);
  // ParityChunkPtr parity[3]{
  //     {blocksize, parity_ptr[0], (uint8_t *)(parity_buffer)},
  //     {blocksize, parity_ptr[1]},
  //     {blocksize, parity_ptr[2], (uint8_t *)(parity_buffer + EXTRA_DATA_SIZE)},
  // };

  const int TOTAL_SLICE_NUM = blocksize / SLICE;
  const int window_size = DECODE_WINDOW_SIZE;
  const int windows_num = TOTAL_SLICE_NUM / window_size;

  long slice_offset[3];
  __m256i mxx1, mxx2, tmp;

  // if (erasure_data_count == 0)
  // {
  //   goto DECODE_REENCODE;
  // }

  // init maperasure
  // slice_id = 0
  // slice_offset[0] = (0 - coding_data_map[0]) * SLICE;
  // slice_offset[1] = 0 * SLICE;
  // slice_offset[2] = (0 - (kval - 1) + coding_data_map[2]) * SLICE;

  // for (int i = 0; i < 3; i++)
  // {
  //   if (maperasure_ptr[i] == nullptr)
  //   {
  //     continue;
  //   }
  //   for (int slice_id = 0; slice_id < TOTAL_SLICE_NUM; slice_id++)
  //   {
  //     tmp = _mm256_load_si256((__m256i *)parity[i][slice_id * SLICE]);
  //     _mm256_store_si256((__m256i *)(maperasure[i])[slice_offset[i]], tmp);
  //     slice_offset[i] += SLICE;
  //   }
  // }

  // solve the key -> use first kval slices to solve erased first slice
  // for (int solve_key = 1; solve_key >= 0; solve_key--)
  for (int window_id = -1; window_id <= windows_num; window_id++)
  {

    // substitute
    // const int slice_id_start = solve_key == 1 ? 0 : kval;
    // const int slice_id_end = solve_key == 1 ? kval : TOTAL_SLICE_NUM;

    // init maperasure by parity
    if (window_id < windows_num)
    {
      const int slice_id_start = (window_id + 1) * window_size;
      const int slice_id_end = slice_id_start + window_size;

      // slice_id = slice_id_start
      slice_offset[0] = (slice_id_start - coding_data_map[0]) * SLICE;
      slice_offset[1] = slice_id_start * SLICE;
      slice_offset[2] = (slice_id_start - (kval - 1) + coding_data_map[2]) * SLICE;
      for (int i = 0; i < 3; i++)
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
    if (0 <= window_id && window_id < windows_num)
    {
      const int slice_id_start = window_id * window_size;
      const int slice_id_end = slice_id_start + window_size;

      for (int retension_data_idx = 0; retension_data_idx < retension_data_count; retension_data_idx++)
      {

        int retension_data_chunk_idx = retension_data[retension_data_idx];

        for (int slice_id = slice_id_start; slice_id < slice_id_end; slice_id++)
        {

          slice_offset[1] = slice_id * SLICE;

          mxx1 = _mm256_load_si256((__m256i *)&data[retension_data_chunk_idx][slice_offset[1]]);

          slice_offset[0] = (slice_id + retension_data_chunk_idx - coding_data_map[0]) * SLICE;
          slice_offset[2] = (slice_id + coding_data_map[2] - retension_data_chunk_idx) * SLICE;

          for (int i = 0; i < 3; i++)
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

    // elimination
    if (window_id > 0)
    {
      const int solve_slice_id_start = (window_id - 1) * window_size;
      const int solve_slice_id_end = solve_slice_id_start + window_size;

      for (int slice_id = solve_slice_id_start; slice_id < solve_slice_id_end; slice_id++)
      {
        slice_offset[1] = slice_id * SLICE;
        constexpr int two_tone_idx[5] = {0, 2, 1, 0, 2};
        for (int i = 0; i < 3; i++)
        {

          int maperasure_idx = two_tone_idx[i];
          if (parity_ptr[maperasure_idx] != nullptr)
          {

            mxx1 = _mm256_load_si256((__m256i *)&data[coding_data_map[maperasure_idx]][slice_offset[1]]);

            slice_offset[0] = (slice_id + coding_data_map[maperasure_idx] - coding_data_map[0]) * SLICE;
            slice_offset[2] = (slice_id + coding_data_map[2] - coding_data_map[maperasure_idx]) * SLICE;

            for (int j = 1; j < 3; j++)
            {

              int other_idx = two_tone_idx[i + j];
              if (maperasure_ptr[other_idx] != nullptr)
              {

                tmp = _mm256_load_si256((__m256i *)(maperasure[other_idx])[slice_offset[other_idx]]);
                mxx2 = _mm256_xor_si256(mxx1, tmp);
                _mm256_store_si256((__m256i *)(maperasure[other_idx])[slice_offset[other_idx]], mxx2);
              }
            }
          }
        }
      }
    }

    if (window_id == 1)
    {
      mxx1 = _mm256_setzero_si256();
      char *parity_key = (char *)aligned_alloc(32, SLICE);
      for (int i = 0; i < k; i++)
      {
        tmp = _mm256_load_si256((__m256i *)&data[i][0]);
        mxx1 = _mm256_xor_si256(mxx1, tmp);
      }
      _mm256_store_si256((__m256i *)parity_key, mxx1);

      redisReply *reply = (redisReply *)redisCommand(
          rctx, "GET %b", parity_key, SLICE);
      memcpy(parity_buffer, reply->str, reply->len);

      parity[0].set_extradata((uint8_t *)(parity_buffer));
      // avoid out of bounds of parity[1]
      parity[1].set_extradata((uint8_t *)(parity_buffer));
      parity[2].set_extradata((uint8_t *)(parity_buffer + EXTRA_DATA_SIZE));

      freeReplyObject(reply);
    }

    // if (solve_key == 1)
    // {
    //   mxx1 = _mm256_load_si256((__m256i *)&data[0][0]);
    //   char *parity_key = (char *)aligned_alloc(32, SLICE);
    //   for (int i = 1; i < kval; i++)
    //   {
    //     tmp = _mm256_load_si256((__m256i *)&data[i][0]);
    //     mxx1 = _mm256_xor_si256(mxx1, tmp);
    //   }
    //   _mm256_store_si256((__m256i *)parity_key, mxx1);

    //   redisReply *reply = (redisReply *)redisCommand(
    //       rctx, "GET %b", parity_key, SLICE);
    //   memcpy(parity_buffer, reply->str, reply->len);

    //   parity[0].set_extradata((uint8_t *)(parity_buffer));

    //   parity[2].set_extradata((uint8_t *)(parity_buffer + EXTRA_DATA_SIZE));

    //   freeReplyObject(reply);

    //   // init maperasure extra part
    //   slice_offset[0] = (TOTAL_SLICE_NUM - coding_data_map[0]) * SLICE;
    //   slice_offset[2] = (TOTAL_SLICE_NUM - (kval - 1) + coding_data_map[2]) * SLICE;
    //   for (int i = 0; i < 3; i += 2)
    //   {
    //     if (maperasure_ptr[i] == nullptr)
    //     {
    //       continue;
    //     }
    //     for (int slice_id = TOTAL_SLICE_NUM; slice_id < TOTAL_SLICE_NUM + kval - 1; slice_id++)
    //     {
    //       tmp = _mm256_load_si256((__m256i *)parity[i][slice_id * SLICE]);
    //       _mm256_store_si256((__m256i *)maperasure[i][slice_offset[i]], tmp);
    //       slice_offset[i] += SLICE;
    //     }
    //   }
    // }

  }

  free(parity_buffer);

  return 0;

#else // ENABLE_DECODE

  return 0;

#endif
}
