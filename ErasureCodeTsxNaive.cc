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

#include "ErasureCodeTsxNaive.h"

#define dout_context g_ceph_context
#define dout_subsys ceph_subsys_osd
#undef dout_prefix
#define dout_prefix _prefix(_dout)

// #define Kval 8 // to be standardized
#define Mval 3 // to be standardized
#define SLICE 32
#define SLICE_EXP 5
#define EXTRA_DATA_SIZE 224 // (Kval - 1) * SLICE
#define EXTRA_SLICE_CNT 7
#define PARITY_CHUNKS 3
#define ENABLE_ENCODE 1
#define ENABLE_DECODE 1
#define ENABLE_LOG 0
// !important: window_size >= k - 1 = 7
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
  return *_dout << "ErasureCodeTsx: ";
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

int ErasureCodeTsxNaive::init(ErasureCodeProfile &profile, ostream *ss)
{
  int err = 0;
  err |= parse(profile, ss);
  err |= to_int("k", profile, &k, "8", ss);
  err |= to_int("m", profile, &m, "3", ss);
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

unsigned int ErasureCodeTsxNaive::get_chunk_size(unsigned int object_size) const
{
  return (object_size + k - 1) / k;
}

int ErasureCodeTsxNaive::encode_chunks(const set<int> &want_to_encode,
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

  uint8_t *parity_buffer = (uint8_t *)aligned_alloc(32, 2 * EXTRA_DATA_SIZE);
  ParityChunkPtr parity1(blocksize, chunks[kval], parity_buffer);
  ParityChunkPtr parity2(blocksize, chunks[kval + 1]);
  ParityChunkPtr parity3(blocksize, chunks[kval + 2], parity_buffer + EXTRA_DATA_SIZE);

  __m256i zero = _mm256_setzero_si256(); // Clear 256-bit register
  // init where chunk 0 not overwrite
  for (int i = blocksize; i < blocksize + EXTRA_DATA_SIZE; i += SLICE)
  {
    _mm256_storeu_si256((__m256i *)parity1[i], zero); // Clear parity1
  }
  for (int i = 0; i < EXTRA_DATA_SIZE; i += SLICE)
  {
    _mm256_storeu_si256((__m256i *)parity3[i], zero); // Clear parity3
  }

  uint64_t slice_offset_1;
  uint64_t slice_offset_2;
  uint64_t slice_offset_3;
  __m256i mxx1, mxx2;

  // chunk 0
  for (int slice_id = 0; slice_id < TOTAL_SLICE_NUM; slice_id++)
  {
    slice_offset_1 = slice_id * SLICE;
    slice_offset_2 = slice_id * SLICE;
    slice_offset_3 = (slice_id + kval - 1) * SLICE;

    mxx1 = _mm256_loadu_si256((__m256i *)&chunks[0][slice_offset_2]);
    _mm256_storeu_si256((__m256i *)parity1[slice_offset_1], mxx1);
    _mm256_storeu_si256((__m256i *)parity2[slice_offset_2], mxx1);
    _mm256_storeu_si256((__m256i *)parity3[slice_offset_3], mxx1);
  }

  // chunk 1 ~ (k - 1)
  for (int chunk_idx = 1; chunk_idx < kval; chunk_idx++)
  {
    for (int slice_id = 0; slice_id < TOTAL_SLICE_NUM; slice_id++)
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

  // 输出chunks[0]数据到文件（十六进制编码）
  // encode_log << "chunks[0][i] after encoding at: " << encode_call_count << "\n";
  // for (unsigned i = 0; i < blocksize; ++i)
  // {
  //   encode_log << std::hex << std::setw(2) << std::setfill('0') << (int)chunks[0][i] << " ";
  // }
  // encode_log << "\n";
  // encode_log << std::dec << "blocksize: " << blocksize << std::endl;
  // encode_log << std::dec << "parity1 after encoding at: " << encode_call_count << "\n";
  // for (int i = 0; i < blocksize + EXTRA_DATA_SIZE; ++i)
  // {
  //   encode_log << std::hex
  //              << std::setw(2) << std::setfill('0')
  //              << static_cast<int>(*(parity1[i])) << " ";
  // }
  // encode_log << "\n";

  encode_log.close();

#endif

  return 0;

#else // ENABLE_ENCODE

  return 0;

#endif
}

int ErasureCodeTsx::decode_chunks(const set<int> &want_to_read,
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
  int coding_data_map[Mval] = {0, 0, kval - 1};
  int need_decoded[Mval] = {-1, -1, -1};

  int erasure_data[Mval];
  int erasure_data_count = 0;
  int retension_data[kval];
  int retension_data_count = 0;

  int erasure_coding[Mval];
  int erasure_coding_count = 0;
  int retension_coding[Mval];
  int retension_coding_count = 0;

  uint8_t *data[kval];
  uint8_t *coding[Mval];

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
        erasure_coding[erasure_coding_count] = parity_idx;
        erasure_coding_count++;
      }
      else
      {
        retension_coding[retension_coding_count] = parity_idx;
        retension_coding_count++;
      }
    }
  }

#if ENABLE_DECODE

  // copy to new space to avoid overwrite for retention coding
  for (int retention_coding_idx = 0; retention_coding_idx < retension_coding_count; retention_coding_idx++) {
    int retention_coding_chunk_idx = retension_coding[retention_coding_idx];
    parity_ptr[retention_coding_chunk_idx] = (uint8_t *)aligned_alloc(SLICE, blocksize);
    memcpy(parity_ptr[retention_coding_chunk_idx], coding[retention_coding_chunk_idx], blocksize);
  }

  ParityChunkPtr parity[3]{
      {blocksize, parity_ptr[0]},
      {blocksize, parity_ptr[1]},
      {blocksize, parity_ptr[2]},
  };
  char *parity_buffer = (char *)aligned_alloc(32, 2 * EXTRA_DATA_SIZE);

  const int TOTAL_SLICE_NUM = blocksize / SLICE;

  long slice_offset[3];
  __m256i mxx1, mxx2, tmp;

  // solve the key -> use first kval slices to solve erased first slice
  // for (int retension_data_idx = 0; retension_data_idx < retension_data_count; retension_data_idx++) {
  //   int chunk_idx = retension_data[retension_data_idx];
  //   for (int slice_id = 0; slice_id < kval; slice_id++) {
      
  //   }
  // }

  if (erasure_data_count == 0) {
    goto DECODE_REENCODE;
  }

  for (int window_id = 0; window_id <= windows_num; window_id++) {

    // substitute
    if (window_id < windows_num) {

      const int slice_id_start = window_id * window_size;
      const int slice_id_end = slice_id_start + window_size;

      for (int retension_data_idx = 0; retension_data_idx < retension_data_count; retension_data_idx++) { 

        int retension_data_chunk_idx = retension_data[retension_data_idx];

        for (int slice_id = slice_id_start; slice_id < slice_id_end; slice_id++) {

          slice_offset[1] = slice_id * SLICE;

          mxx1 = _mm256_load_si256((__m256i *)&data[retension_data_chunk_idx][slice_offset[1]]);

          slice_offset[0] = (slice_id + retension_data_chunk_idx) * SLICE;
          slice_offset[2] = (slice_id + kval - 1 - retension_data_chunk_idx) * SLICE;

          for (int retension_coding_idx = 0; retension_coding_idx < erasure_data_count; retension_coding_idx++) {

            int retension_coding_chunk_idx = retension_coding[retension_coding_idx];

            tmp = _mm256_load_si256((__m256i *)(parity[retension_coding_chunk_idx])[slice_offset[retension_coding_chunk_idx]]);
            mxx2 = _mm256_xor_si256(mxx1, tmp);
            _mm256_store_si256((__m256i *)(parity[retension_coding_chunk_idx])[slice_offset[retension_coding_chunk_idx]], mxx2);

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

        // solve the first subsystem
        if (retension_coding[0] == 0) {

          int erasure_data_chunk_idx = erasure_data[0];
          slice_offset[0] = (slice_id + erasure_data_chunk_idx) * SLICE;

          mxx1 = _mm256_load_si256((__m256i *)parity[0][slice_offset[0]]);
          _mm256_store_si256((__m256i *)(data[erasure_data_chunk_idx])[slice_offset[1]], mxx1);

          // substitute
          slice_offset[2] = (slice_id + kval - 1 - erasure_data_chunk_idx) * SLICE;
          for (int retension_coding_idx = 1; retension_coding_idx < erasure_data_count; retension_coding_idx++) { 
            
            int retension_coding_chunk_idx = retension_coding[retension_coding_idx];
            tmp = _mm256_load_si256((__m256i *)(parity[retension_coding_chunk_idx])[slice_offset[retension_coding_chunk_idx]]);
            mxx2 = _mm256_xor_si256(mxx1, tmp);
            _mm256_store_si256((__m256i *)(parity[retension_coding_chunk_idx])[slice_offset[retension_coding_chunk_idx]], mxx2);

          }

        }

        // solve the second subsystem
        for (int retension_coding_idx = erasure_data_count - 1; retension_coding_idx >= erasure_data_count / 2; retension_coding_idx--) {

          int retension_coding_chunk_idx = retension_coding[retension_coding_idx];
          int erasure_data_chunk_idx = erasure_data[retension_coding_idx];

          slice_offset[2] = (slice_id + kval - 1 - erasure_data_chunk_idx) * SLICE;
          slice_offset[0] = (slice_id + erasure_data_chunk_idx) * SLICE;
          
          mxx1 = _mm256_load_si256((__m256i *)parity[retension_coding_chunk_idx][slice_offset[retension_coding_chunk_idx]]);
          _mm256_store_si256((__m256i *)(data[erasure_data_chunk_idx])[slice_offset[1]], mxx1);

          // substitute
          for (int substitute_coding_idx = 0; substitute_coding_idx < erasure_data_count; substitute_coding_idx++) { 
            if (substitute_coding_idx == retension_coding_idx) {
              continue;
            }
            int substitute_coding_chunk_idx = retension_coding[substitute_coding_idx];
            tmp = _mm256_load_si256((__m256i *)(parity[substitute_coding_chunk_idx])[slice_offset[substitute_coding_chunk_idx]]);
            mxx2 = _mm256_xor_si256(mxx1, tmp);
            _mm256_store_si256((__m256i *)(parity[substitute_coding_chunk_idx])[slice_offset[substitute_coding_chunk_idx]], mxx2);

          }

        }

      }
    }

    if (window_id == 1)
    {
      mxx1 = _mm256_setzero_si256();
      char *parity_key = (char *)aligned_alloc(32, SLICE);
      for (int i = 0; i < kval; i++)
      {
        tmp = _mm256_load_si256((__m256i *)&data[i][0]);
        mxx1 = _mm256_xor_si256(mxx1, tmp);
      }
      _mm256_store_si256((__m256i *)parity_key, mxx1);

      redisReply *reply = (redisReply *)redisCommand(
          rctx, "GET %b", parity_key, SLICE);
      memcpy(parity_buffer, reply->str, reply->len);

      parity[0].set_extradata((uint8_t *)(parity_buffer));

      parity[2].set_extradata((uint8_t *)(parity_buffer + EXTRA_DATA_SIZE));

      freeReplyObject(reply);
    }
  }
  
DECODE_REENCODE:  
  // re-encode
  __m256i zero = _mm256_setzero_si256(); // Clear 256-bit register
  if (erasure_coding_count == 0) {
    goto DECODE_END;
  }
  for (int erasure_coding_idx = 0; erasure_coding_idx < erasure_coding_count; erasure_coding_idx++) {
    int erasure_coding_chunk_idx = erasure_coding[erasure_coding_idx];
    parity[erasure_coding_chunk_idx].set_chunk_ptr(coding[erasure_coding_chunk_idx]);
  }
  
  // init erased parities where chunk 0 not overwrite
  if (erasure_coding[0] == 0) {
    for (int i = blocksize; i < blocksize + EXTRA_DATA_SIZE; i += SLICE)
    {
      _mm256_storeu_si256((__m256i *)parity[0][i], zero); // Clear parity1
    }
  }
  if (erasure_coding[erasure_coding_count - 1] == 2) {
    for (int i = 0; i < EXTRA_DATA_SIZE; i += SLICE)
    {
      _mm256_storeu_si256((__m256i *)parity[2][i], zero); // Clear parity3
    }
  }

  // re-encode with chunk 0
  for (int slice_id = 0; slice_id < TOTAL_SLICE_NUM; slice_id++)
  {
    slice_offset[0] = slice_id * SLICE;
    slice_offset[1] = slice_id * SLICE;
    slice_offset[2] = (slice_id + kval - 1) * SLICE;

    mxx1 = _mm256_loadu_si256((__m256i *)&data[0][slice_offset[1]]);
    for (int erasure_coding_idx = 0; erasure_coding_idx < erasure_coding_count; erasure_coding_idx++) {
      int erasure_coding_chunk_idx = erasure_coding[erasure_coding_idx];
      _mm256_storeu_si256((__m256i *)parity[erasure_coding_chunk_idx][slice_offset[erasure_coding_chunk_idx]], mxx1);
    }
    
  }

  // chunk 1 ~ (k - 1)
  for (int chunk_idx = 1; chunk_idx < kval; chunk_idx++)
  {
    for (int slice_id = 0; slice_id < TOTAL_SLICE_NUM; slice_id++)
    {
      slice_offset[1] = slice_id * SLICE;
      slice_offset[0] = (slice_id + chunk_idx) * SLICE;
      slice_offset[2] = (slice_id + (kval - 1 - chunk_idx)) * SLICE;

      mxx1 = _mm256_loadu_si256((__m256i *)&data[chunk_idx][slice_offset[1]]);

      for (int erasure_coding_idx = 0; erasure_coding_idx < erasure_coding_count; erasure_coding_idx++) {
        int erasure_coding_chunk_idx = erasure_coding[erasure_coding_idx];
        tmp = _mm256_loadu_si256((__m256i *)parity[erasure_coding_chunk_idx][slice_offset[erasure_coding_chunk_idx]]);
        mxx2 = _mm256_xor_si256(mxx1, tmp);
        _mm256_storeu_si256((__m256i *)parity[erasure_coding_chunk_idx][slice_offset[erasure_coding_chunk_idx]], mxx2);
      }

    }
  }


DECODE_END:

  free(parity_buffer);

  return 0;

#else // ENABLE_DECODE

  return 0;

#endif
}
