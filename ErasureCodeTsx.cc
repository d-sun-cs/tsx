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
#include <unordered_map>
#include <immintrin.h>
#include "common/debug.h"
#include "include/stringify.h"
#include "erasure-code/ErasureCodePlugin.h"

#include "ErasureCodeTsx.h"

#define dout_context g_ceph_context
#define dout_subsys ceph_subsys_osd
#undef dout_prefix
#define dout_prefix _prefix(_dout)

#define EXTRA_DATA_SIZE 256
#define PARITY_CHUNKS 3
#define ENABLE_ENCODE 1
#define ENABLE_DECODE 1
#define ENABLE_LOG 0
// !important: window_size >= k - 1
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

static std::unordered_map<int, std::array<uint8_t *, PARITY_CHUNKS>> parity_extra_data;

class ParityChunkPtr
{
public:
  ParityChunkPtr(unsigned blocksize, uint8_t *chunk_ptr, uint8_t *extradata = nullptr)
      : blocksize(blocksize), chunk_ptr(chunk_ptr), external_extradata(extradata != nullptr)
  {
    if (external_extradata)
    {
      this->extradata = extradata;
    }
    else
    {
      this->extradata = (uint8_t *)aligned_alloc(32, EXTRA_DATA_SIZE);
    }
  }

  uint8_t *operator[](size_t idx)
  {
    if (idx >= blocksize)
    {
      return &extradata[idx - blocksize];
    }
    return &chunk_ptr[idx];
  }

  uint8_t *get_extradata() const
  {
    return extradata;
  }

private:
  unsigned blocksize;
  uint8_t *chunk_ptr;
  uint8_t *extradata;
  bool external_extradata; // True if extradata is externally managed
};

int ErasureCodeTsx::init(ErasureCodeProfile &profile, ostream *ss)
{
  int err = 0;
  err |= parse(profile, ss);
  if (err)
    return err;

  this->rctx = redisConnect("127.0.0.1", 6379);
  if (this->rctx == NULL || this->rctx->err)
  {
    dout(0) << "Error Redis: " << this->rctx->errstr << dendl;
    return 1;
  }
  // redisReply *reply = (redisReply*)redisCommand(this->rc, "SET foo bar");
  // dout(20) << "Reply: " << reply->str << dendl; // >>> Reply: OK
  // freeReplyObject(reply);
  // reply = (redisReply*)redisCommand(this->rc, "GET foo");
  // dout(20) << "Reply: " << reply->str << dendl; // >>> Reply: bar
  // freeReplyObject(reply);
  // redisFree(this->rc);

  return ErasureCode::init(profile, ss);
}

unsigned int ErasureCodeTsx::get_chunk_size(unsigned int object_size) const
{
  return (object_size + data_chunk_count - 1) / data_chunk_count;
}

int ErasureCodeTsx::encode_chunks(const set<int> &want_to_encode,
                                  map<int, bufferlist> *encoded)
{

  uint8_t *chunks[chunk_count];
  for (unsigned int i = 0; i < chunk_count; ++i)
  {
    chunks[i] = (uint8_t *)(*encoded)[i].c_str();
  }

  const int k = data_chunk_count;
  unsigned blocksize = (*encoded)[0].length();

#if ENABLE_ENCODE

  static int encode_call_count = -1;
  encode_call_count++;

#if ENABLE_LOG
  std::ofstream encode_log("tsx-encode.log", std::ios::app);
  // static double total_encode_time_ms = 0;
  // auto encode_start = std::chrono::high_resolution_clock::now();
#endif

  const int SLICE = 32;
  const int TOTAL_SLICE_NUM = blocksize / SLICE;
  const int window_size = ENCODE_WINDOW_SIZE;
  const int windows_num = TOTAL_SLICE_NUM / window_size;

  // ParityChunkPtr parity1(blocksize, chunks[k]);
  // ParityChunkPtr parity2(blocksize, chunks[k + 1]);
  // ParityChunkPtr parity3(blocksize, chunks[k + 2]);

  ParityChunkPtr parity_k(blocksize, chunks[k]);
  ParityChunkPtr parity_k1(blocksize, chunks[k + 1]);
  ParityChunkPtr parity_k2(blocksize, chunks[k + 2]);

  uint64_t slice_offset_1;
  uint64_t slice_offset_2;
  uint64_t slice_offset_3;
  __m256i mxx1, mxx2, mxx3, mxx4, res1, res3, tmp;

  // for (int window_id = 0; window_id < windows_num; window_id++)
  // {
  //   const int slice_id_start = window_id * window_size;
  //   const int slice_id_end = slice_id_start + window_size;

  //   for (int slice_id = slice_id_start; slice_id < slice_id_end; slice_id++)
  //   {
  //     slice_offset_1 = slice_id * SLICE;
  //     slice_offset_2 = slice_id * SLICE;
  //     slice_offset_3 = (slice_id + 6) * SLICE; // 7 - 1

  //     // Load two data chunks into registers and XOR them
  //     mxx1 = _mm256_load_si256((__m256i *)&chunks[0][slice_offset_2]);
  //     mxx2 = _mm256_load_si256((__m256i *)&chunks[1][slice_offset_2]);
  //     res1 = _mm256_xor_si256(mxx1, mxx2);

  //     // Store the result into parity chunks
  //     _mm256_store_si256((__m256i *)parity2[slice_offset_2], res1);
  //     if (slice_id == slice_id_start)
  //     {
  //       res1 = mxx1;
  //       res3 = mxx2;
  //     }
  //     else
  //     {
  //       res1 = _mm256_xor_si256(mxx1, mxx4);
  //       res3 = _mm256_xor_si256(mxx2, mxx3);
  //     }
  //     if (window_id != 0)
  //     {
  //       if ((slice_id % window_size) < k - 1)
  //       {
  //         tmp = _mm256_load_si256((__m256i *)parity1[slice_offset_1]);
  //         res1 = _mm256_xor_si256(res1, tmp);
  //       }
  //       if (slice_id == slice_id_start)
  //       {
  //         tmp = _mm256_load_si256((__m256i *)parity3[slice_offset_3]);
  //         res3 = _mm256_xor_si256(res3, tmp);
  //       }
  //     }
  //     _mm256_store_si256((__m256i *)parity1[slice_offset_1], res1);
  //     _mm256_store_si256((__m256i *)parity3[slice_offset_3], res3);
  //     if (slice_id != slice_id_end - 1)
  //     {
  //       mxx3 = mxx1;
  //       mxx4 = mxx2;
  //     }
  //     else
  //     {
  //       // [!] Assert window_size >= k - 1, or overwriting occurs
  //       _mm256_store_si256((__m256i *)parity1[slice_offset_1 + SLICE], mxx2);
  //       _mm256_store_si256((__m256i *)parity3[slice_offset_3 + SLICE], mxx1);
  //     }
  //   }

  //   for (int chunk_idx = 2; chunk_idx < 8; chunk_idx += 2)
  //   {
  //     for (int slice_id = slice_id_start; slice_id < slice_id_end; slice_id++)
  //     {
  //       slice_offset_2 = slice_id * SLICE;
  //       slice_offset_1 = (slice_id + chunk_idx) * SLICE;
  //       slice_offset_3 = (slice_id + (6 - chunk_idx)) * SLICE; // 7 - 1

  //       // Load two data chunks into registers and XOR them
  //       mxx1 = _mm256_load_si256((__m256i *)&chunks[chunk_idx][slice_offset_2]);
  //       mxx2 = _mm256_load_si256((__m256i *)&chunks[chunk_idx + 1][slice_offset_2]);
  //       res1 = _mm256_xor_si256(mxx1, mxx2);

  //       // Store the result into parity chunks
  //       tmp = _mm256_load_si256((__m256i *)parity2[slice_offset_2]);
  //       res1 = _mm256_xor_si256(res1, tmp);
  //       _mm256_store_si256((__m256i *)parity2[slice_offset_2], res1);

  //       if (slice_id == slice_id_start)
  //       {
  //         res1 = mxx1;
  //         res3 = mxx2;
  //       }
  //       else
  //       {
  //         res1 = _mm256_xor_si256(mxx1, mxx4);
  //         res3 = _mm256_xor_si256(mxx2, mxx3);
  //       }

  //       if (slice_id > 1)
  //       {
  //         tmp = _mm256_load_si256((__m256i *)parity3[slice_offset_3]);
  //         res3 = _mm256_xor_si256(res3, tmp);
  //       }
  //       _mm256_store_si256((__m256i *)parity3[slice_offset_3], res3);

  //       if (slice_id != slice_id_end - 1)
  //       {
  //         tmp = _mm256_load_si256((__m256i *)parity1[slice_offset_1]);
  //         res1 = _mm256_xor_si256(res1, tmp);
  //         _mm256_store_si256((__m256i *)parity1[slice_offset_1], res1);
  //         mxx3 = mxx1;
  //         mxx4 = mxx2;
  //       }
  //       else
  //       {
  //         _mm256_store_si256((__m256i *)parity1[slice_offset_1], res1);
  //         // the slice of shift end
  //         _mm256_store_si256((__m256i *)parity1[slice_offset_1 + SLICE], mxx2);
  //         tmp = _mm256_load_si256((__m256i *)parity3[slice_offset_3 + SLICE]);
  //         res3 = _mm256_xor_si256(mxx1, tmp);
  //         _mm256_store_si256((__m256i *)parity3[slice_offset_3 + SLICE], res3);
  //       }
  //     }
  //   }
  // }

  for (int window_id = 0; window_id < windows_num; window_id++)
  {
    const int slice_id_start = window_id * window_size;
    const int slice_id_end = slice_id_start + window_size;

    for (int slice_id = slice_id_start; slice_id < slice_id_end; slice_id++)
    {
      slice_offset_2 = slice_id * SLICE;
      slice_offset_1 = slice_id * SLICE;
      slice_offset_3 = ((slice_id + 7) * SLICE);
      mxx1 = _mm256_load_si256((__m256i *)&chunks[0][slice_offset_2]);

      if (slice_id >= window_size && slice_id - slice_id_start < k - 1)
      { // avoid overwrite
        mxx2 = _mm256_xor_si256(mxx1, *((__m256i *)parity_k[slice_offset_1]));
        _mm256_store_si256((__m256i *)parity_k[slice_offset_1], mxx2);
      }
      else
      {
        _mm256_store_si256((__m256i *)parity_k[slice_offset_1], mxx1);
      }
      // _mm256_store_si256((__m256i*)parity_k[slice_offset_1], mxx1);

      _mm256_store_si256((__m256i *)parity_k1[slice_offset_2], mxx1);
      _mm256_store_si256((__m256i *)parity_k2[slice_offset_3], mxx1);
    }

    for (int chunk_idx = 1; chunk_idx < 8; chunk_idx++)
    {
      for (int slice_id = slice_id_start; slice_id < slice_id_end; slice_id++)
      {
        slice_offset_2 = slice_id * SLICE;
        slice_offset_1 = ((slice_id + chunk_idx) * SLICE);
        slice_offset_3 = ((slice_id + (7 - chunk_idx)) * SLICE);
        mxx1 = _mm256_load_si256((__m256i *)&chunks[chunk_idx][slice_offset_2]);

        if (slice_id == slice_id_end - 1)
        {
          _mm256_store_si256((__m256i *)parity_k[slice_offset_1], mxx1);
        }
        else
        {
          mxx2 = _mm256_xor_si256(mxx1, *((__m256i *)parity_k[slice_offset_1]));
          _mm256_store_si256((__m256i *)parity_k[slice_offset_1], mxx2);
        }

        mxx2 = _mm256_xor_si256(mxx1, *((__m256i *)parity_k1[slice_offset_2]));
        _mm256_store_si256((__m256i *)parity_k1[slice_offset_2], mxx2);

        if (slice_id == 0)
        {
          _mm256_store_si256((__m256i *)parity_k2[slice_offset_3], mxx1);
        }
        else
        {
          mxx2 = _mm256_xor_si256(mxx1, *((__m256i *)parity_k2[slice_offset_3]));
          _mm256_store_si256((__m256i *)parity_k2[slice_offset_3], mxx2);
        }
      }
    }
  }

  std::array<uint8_t *, PARITY_CHUNKS> extra_data = {
      parity_k.get_extradata(),
      parity_k1.get_extradata(),
      parity_k2.get_extradata()};

  parity_extra_data[encode_call_count] = extra_data;

  redisReply *reply = (redisReply *)redisCommand(
      rctx, "SET %s %b", encode_call_count, extra_data, 256);
  freeReplyObject(reply);

#if ENABLE_LOG
  // auto encode_end = std::chrono::high_resolution_clock::now();
  // double encode_duration = std::chrono::duration_cast<std::chrono::microseconds>(encode_end - encode_start).count() / 1000.0;
  // total_encode_time_ms += encode_duration;

  // encode_log << "encode_call_count: " << encode_call_count
  //            << ", blocksize: " << blocksize
  //            << ", total_time_ms: " << total_encode_time_ms << "\n";

  // 输出chunks[0]数据到文件（十六进制编码）
  encode_log << "chunks[0] encode_call_count: " << encode_call_count << "\n";
  for (unsigned i = 0; i < blocksize; ++i)
  {
    encode_log << std::hex << std::setw(2) << std::setfill('0') << (int)chunks[0][i] << " ";
  }
  encode_log << "\n";
  // 输出parity_k2数据到文件（十六进制编码）
  encode_log << "parity_k2 encode_call_count: " << encode_call_count << "\n";
  for (unsigned i = 0; i < blocksize + EXTRA_DATA_SIZE; ++i)
  {
    encode_log << std::hex << std::setw(2) << std::setfill('0') << (int)*(parity_k2[i]) << " ";
  }
  encode_log << "\n";
  encode_log.close();

#endif

  return 0;

#else

  return 0;

#endif
}

int ErasureCodeTsx::decode_chunks(const set<int> &want_to_read,
                                  const map<int, bufferlist> &chunks,
                                  map<int, bufferlist> *decoded)
{

  unsigned blocksize = (*chunks.begin()).second.length();
  int erasure_data[chunk_count - data_chunk_count];
  int erasure_data_count = 0;
  int retension_data[data_chunk_count - 1];
  int retension_data_count = 0;
  // int retension_coding[3] = {0, 1, 2};
  // int retension_coding[chunk_count - data_chunk_count];
  int retension_coding_count = 0;
  bool retension_coding[chunk_count - data_chunk_count];
  uint8_t *data[data_chunk_count];
  uint8_t *coding[chunk_count - data_chunk_count];
  for (unsigned int i = 0; i < chunk_count; i++)
  {

    if (i < data_chunk_count)
    {
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
      data[i] = (uint8_t *)(*decoded)[i].c_str();
    }
    else
    {
      if (chunks.find(i) != chunks.end())
      {
        // retension_coding[retension_coding_count] = i - data_chunk_count;
        retension_coding[i - data_chunk_count] = true;
        erasure_data[i - data_chunk_count] = erasure_data[retension_coding_count];
        retension_coding_count++;
      }
      else
      {
        retension_coding[i - data_chunk_count] = false;
      }
      coding[i - data_chunk_count] = (uint8_t *)(*decoded)[i].c_str();
    }
  }

#if ENABLE_DECODE

  static int decode_call_count = -1;
  decode_call_count++;

#if ENABLE_LOG
  std::ofstream decode_log("tsx-decode.log", std::ios::app);
  // static double total_decode_time_ms = 0;
  // auto decode_start = std::chrono::high_resolution_clock::now();
#endif

  auto extra_data = parity_extra_data[decode_call_count];

  ParityChunkPtr parity[3]{
      {blocksize, coding[0], extra_data[0]},
      {blocksize, coding[1], extra_data[1]},
      {blocksize, coding[2], extra_data[2]}};

  const int SLICE = 32;
  const int TOTAL_SLICE_NUM = blocksize / SLICE;
  const int window_size = DECODE_WINDOW_SIZE;
  const int windows_num = TOTAL_SLICE_NUM / window_size;

  uint64_t slice_offset[3];
  __m256i mxx1, mxx2, tmp;

#if ENABLE_LOG

  // 输出original parity[2]数据到文件（十六进制编码）
  decode_log << "original parity[2] decode_call_count: " << decode_call_count << "\n";
  for (unsigned i = 0; i < blocksize + EXTRA_DATA_SIZE; ++i)
  {
    decode_log << std::hex << std::setw(2) << std::setfill('0') << (int)*(parity[2][i]) << " ";
  }
  decode_log << "\n";

#endif

  for (int window_id = 0; window_id <= windows_num; window_id++)
  {
    if (window_id < windows_num)
    {
      const int slice_id_start = window_id * window_size;
      const int slice_id_end = slice_id_start + window_size;
      for (int retension_data_idx = 0; retension_data_idx < retension_data_count; retension_data_idx++)
      {
        int retension_data_chunk_idx = retension_data[retension_data_idx];
        for (int slice_id = slice_id_start; slice_id < slice_id_end; slice_id++)
        {
          slice_offset[1] = slice_id * SLICE;
          slice_offset[0] = (slice_id + retension_data_chunk_idx) * SLICE;
          slice_offset[2] = (slice_id + 7 - retension_data_chunk_idx) * SLICE;

          mxx1 = _mm256_load_si256((__m256i *)&data[retension_data_chunk_idx][slice_offset[1]]);
          tmp = _mm256_load_si256((__m256i *)parity[0][slice_offset[0]]);
          mxx2 = _mm256_xor_si256(mxx1, tmp);
          _mm256_store_si256((__m256i *)parity[0][slice_offset[0]], mxx2);
          tmp = _mm256_load_si256((__m256i *)parity[1][slice_offset[1]]);
          mxx2 = _mm256_xor_si256(mxx1, tmp);
          _mm256_store_si256((__m256i *)parity[1][slice_offset[1]], mxx2);
          tmp = _mm256_load_si256((__m256i *)parity[2][slice_offset[2]]);
          mxx2 = _mm256_xor_si256(mxx1, tmp);
          _mm256_store_si256((__m256i *)parity[2][slice_offset[2]], mxx2);
        }
      }
    }

    if (window_id > 0)
    {
      const int solve_slice_id_start = (window_id - 1) * window_size;
      const int solve_slice_id_end = solve_slice_id_start + window_size;

      for (int slice_id = solve_slice_id_start; slice_id < solve_slice_id_end; slice_id++)
      {

        if (retension_coding[0])
        {
          int erasure_data_chunk_idx = erasure_data[0];
          slice_offset[1] = slice_id * SLICE;
          slice_offset[0] = (slice_id + erasure_data_chunk_idx) * SLICE;
          slice_offset[2] = (slice_id + 7 - erasure_data_chunk_idx) * SLICE;

          mxx1 = _mm256_load_si256((__m256i *)parity[0][slice_offset[0]]);
          _mm256_store_si256((__m256i *)&data[erasure_data_chunk_idx][slice_offset[1]], mxx1);
          mxx2 = _mm256_xor_si256(mxx1, *((__m256i *)parity[1][slice_offset[1]]));
          _mm256_store_si256((__m256i *)parity[1][slice_offset[1]], mxx2);
          mxx2 = _mm256_xor_si256(mxx1, *((__m256i *)parity[2][slice_offset[2]]));
          _mm256_store_si256((__m256i *)parity[2][slice_offset[2]], mxx2);
        }
        if (retension_coding[2])
        {
          int erasure_data_chunk_idx = erasure_data[2];
          slice_offset[1] = slice_id * SLICE;
          slice_offset[0] = (slice_id + erasure_data_chunk_idx) * SLICE;
          slice_offset[2] = (slice_id + 7 - erasure_data_chunk_idx) * SLICE;

          mxx1 = _mm256_load_si256((__m256i *)parity[2][slice_offset[2]]);
          _mm256_store_si256((__m256i *)&data[erasure_data_chunk_idx][slice_offset[1]], mxx1);
          mxx2 = _mm256_xor_si256(mxx1, *((__m256i *)parity[0][slice_offset[0]]));
          _mm256_store_si256((__m256i *)parity[0][slice_offset[0]], mxx2);
          mxx2 = _mm256_xor_si256(mxx1, *((__m256i *)parity[1][slice_offset[1]]));
          _mm256_store_si256((__m256i *)parity[1][slice_offset[1]], mxx2);
        }
        if (retension_coding[1])
        {
          int erasure_data_chunk_idx = erasure_data[1];
          slice_offset[1] = slice_id * SLICE;
          slice_offset[0] = (slice_id + erasure_data_chunk_idx) * SLICE;
          slice_offset[2] = (slice_id + 7 - erasure_data_chunk_idx) * SLICE;

          mxx1 = _mm256_load_si256((__m256i *)parity[1][slice_offset[1]]);
          _mm256_store_si256((__m256i *)&data[erasure_data_chunk_idx][slice_offset[1]], mxx1);
          mxx2 = _mm256_xor_si256(mxx1, *((__m256i *)parity[0][slice_offset[0]]));
          _mm256_store_si256((__m256i *)parity[0][slice_offset[0]], mxx2);
          mxx2 = _mm256_xor_si256(mxx1, *((__m256i *)parity[2][slice_offset[2]]));
          _mm256_store_si256((__m256i *)parity[2][slice_offset[2]], mxx2);
        }
      }
    }
  }

#if ENABLE_LOG
  // auto decode_end = std::chrono::high_resolution_clock::now();
  // double decode_duration = std::chrono::duration_cast<std::chrono::microseconds>(decode_end - decode_start).count() / 1000.0;
  // total_decode_time_ms += decode_duration;

  // decode_log << "decode_call_count: " << decode_call_count
  //            << ", blocksize: " << blocksize
  //            << ", total_time_ms: " << total_decode_time_ms << "\n";

  // 输出data[0]数据到文件（十六进制编码）
  decode_log << "data[0] decode_call_count: " << decode_call_count << "\n";
  for (unsigned i = 0; i < blocksize; ++i)
  {
    decode_log << std::hex << std::setw(2) << std::setfill('0') << (int)data[0][i] << " ";
  }
  decode_log << "\n";
  // 输出processed parity[2]数据到文件（十六进制编码）
  decode_log << "processed parity[2] decode_call_count: " << decode_call_count << "\n";
  for (unsigned i = 0; i < blocksize + EXTRA_DATA_SIZE; ++i)
  {
    decode_log << std::hex << std::setw(2) << std::setfill('0') << (int)*(parity[2][i]) << " ";
  }
  decode_log << "\n";
  decode_log.close();

#endif

  return 0;

#else

  return 0;

#endif
}
