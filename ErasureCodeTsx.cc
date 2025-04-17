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
#include <algorithm>
#include <fstream>
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

using namespace std;
using namespace ceph;

static ostream& _prefix(std::ostream* _dout)
{
  return *_dout << "ErasureCodeTsx: ";
}

static std::unordered_map<int, std::array<std::array<uint8_t, EXTRA_DATA_SIZE>, PARITY_CHUNKS>> parity_extra_data;

unsigned int ErasureCodeTsx::get_chunk_size(unsigned int object_size) const
{
  // According to the comment, calculate the chunk size
  return (object_size + data_chunk_count - 1) / data_chunk_count;
}

int ErasureCodeTsx::encode_chunks(const set<int> &want_to_encode,
                                  map<int, bufferlist> *encoded)
{

  static int encode_call_count = -1;
  encode_call_count++;

  // Prepare input chunks
  uint8_t *chunks[chunk_count];
  for (unsigned int i = 0; i < chunk_count; ++i)
  {
    chunks[i] = (uint8_t *)(*encoded)[i].c_str();
  }

  // Calculate k and m
  const int k = data_chunk_count;

  // Define constants
  const int SLICE = 32;
  const int TOTAL_SLICE_NUM = (*encoded)[0].length() / SLICE;
  const int PARITY_SIZE = (*encoded)[0].length() + EXTRA_DATA_SIZE;



  // Allocate temporary arrays for parity chunks using malloc
  uint8_t *temp_chunks_k = (uint8_t *)malloc(PARITY_SIZE * sizeof(uint8_t));
  uint8_t *temp_chunks_k1 = (uint8_t *)malloc(PARITY_SIZE * sizeof(uint8_t));
  uint8_t *temp_chunks_k2 = (uint8_t *)malloc(PARITY_SIZE * sizeof(uint8_t));
  // memset(temp_chunks_k, 0, PARITY_SIZE);
  // memset(temp_chunks_k1, 0, PARITY_SIZE);
  // memset(temp_chunks_k2, 0, PARITY_SIZE);

#if ENABLE_ENCODE

  // Shift XOR encode algorithm
  uint64_t slice_offset_1;
  uint64_t slice_offset_2;
  uint64_t slice_offset_3;
  __m256i mxx1, mxx2;

  for (int slice_id = 0; slice_id < TOTAL_SLICE_NUM; slice_id++)
  {
    slice_offset_2 = slice_id * SLICE;
    slice_offset_1 = slice_id * SLICE;
    slice_offset_3 = ((slice_id + 7) * SLICE);
    mxx1 = _mm256_loadu_si256((__m256i *)(chunks[0] + slice_offset_2));
    _mm256_storeu_si256((__m256i *)(temp_chunks_k + slice_offset_1), mxx1);
    _mm256_storeu_si256((__m256i *)(temp_chunks_k1 + slice_offset_2), mxx1);
    _mm256_storeu_si256((__m256i *)(temp_chunks_k2 + slice_offset_3), mxx1);
  }

  for (int slice_id = 0; slice_id < TOTAL_SLICE_NUM; slice_id++)
  {
    slice_offset_2 = slice_id * SLICE;
    slice_offset_1 = ((slice_id + 1) * SLICE);
    slice_offset_3 = ((slice_id + 6) * SLICE);
    mxx1 = _mm256_loadu_si256((__m256i *)(chunks[1] + slice_offset_2));
    mxx2 = _mm256_xor_si256(mxx1, *((__m256i *)(temp_chunks_k + slice_offset_1)));
    _mm256_storeu_si256((__m256i *)(temp_chunks_k + slice_offset_1), mxx2);
    mxx2 = _mm256_xor_si256(mxx1, *((__m256i *)(temp_chunks_k1 + slice_offset_2)));
    _mm256_storeu_si256((__m256i *)(temp_chunks_k1 + slice_offset_2), mxx2);
    mxx2 = _mm256_xor_si256(mxx1, *((__m256i *)(temp_chunks_k2 + slice_offset_3)));
    _mm256_storeu_si256((__m256i *)(temp_chunks_k2 + slice_offset_3), mxx2);
  }

  for (int slice_id = 0; slice_id < TOTAL_SLICE_NUM; slice_id++)
  {
    slice_offset_2 = slice_id * SLICE;
    slice_offset_1 = ((slice_id + 2) * SLICE);
    slice_offset_3 = ((slice_id + 5) * SLICE);
    mxx1 = _mm256_loadu_si256((__m256i *)(chunks[2] + slice_offset_2));
    mxx2 = _mm256_xor_si256(mxx1, *((__m256i *)(temp_chunks_k + slice_offset_1)));
    _mm256_storeu_si256((__m256i *)(temp_chunks_k + slice_offset_1), mxx2);
    mxx2 = _mm256_xor_si256(mxx1, *((__m256i *)(temp_chunks_k1 + slice_offset_2)));
    _mm256_storeu_si256((__m256i *)(temp_chunks_k1 + slice_offset_2), mxx2);
    mxx2 = _mm256_xor_si256(mxx1, *((__m256i *)(temp_chunks_k2 + slice_offset_3)));
    _mm256_storeu_si256((__m256i *)(temp_chunks_k2 + slice_offset_3), mxx2);
  }

  for (int slice_id = 0; slice_id < TOTAL_SLICE_NUM; slice_id++)
  {
    slice_offset_2 = slice_id * SLICE;
    slice_offset_1 = ((slice_id + 3) * SLICE);
    slice_offset_3 = ((slice_id + 4) * SLICE);
    mxx1 = _mm256_loadu_si256((__m256i *)(chunks[3] + slice_offset_2));
    mxx2 = _mm256_xor_si256(mxx1, *((__m256i *)(temp_chunks_k + slice_offset_1)));
    _mm256_storeu_si256((__m256i *)(temp_chunks_k + slice_offset_1), mxx2);
    mxx2 = _mm256_xor_si256(mxx1, *((__m256i *)(temp_chunks_k1 + slice_offset_2)));
    _mm256_storeu_si256((__m256i *)(temp_chunks_k1 + slice_offset_2), mxx2);
    mxx2 = _mm256_xor_si256(mxx1, *((__m256i *)(temp_chunks_k2 + slice_offset_3)));
    _mm256_storeu_si256((__m256i *)(temp_chunks_k2 + slice_offset_3), mxx2);
  }

  for (int slice_id = 0; slice_id < TOTAL_SLICE_NUM; slice_id++)
  {
    slice_offset_2 = slice_id * SLICE;
    slice_offset_1 = ((slice_id + 4) * SLICE);
    slice_offset_3 = ((slice_id + 3) * SLICE);
    mxx1 = _mm256_loadu_si256((__m256i *)(chunks[4] + slice_offset_2));
    mxx2 = _mm256_xor_si256(mxx1, *((__m256i *)(temp_chunks_k + slice_offset_1)));
    _mm256_storeu_si256((__m256i *)(temp_chunks_k + slice_offset_1), mxx2);
    mxx2 = _mm256_xor_si256(mxx1, *((__m256i *)(temp_chunks_k1 + slice_offset_2)));
    _mm256_storeu_si256((__m256i *)(temp_chunks_k1 + slice_offset_2), mxx2);
    mxx2 = _mm256_xor_si256(mxx1, *((__m256i *)(temp_chunks_k2 + slice_offset_3)));
    _mm256_storeu_si256((__m256i *)(temp_chunks_k2 + slice_offset_3), mxx2);
  }

  for (int slice_id = 0; slice_id < TOTAL_SLICE_NUM; slice_id++)
  {
    slice_offset_2 = slice_id * SLICE;
    slice_offset_1 = ((slice_id + 5) * SLICE);
    slice_offset_3 = ((slice_id + 2) * SLICE);
    mxx1 = _mm256_loadu_si256((__m256i *)(chunks[5] + slice_offset_2));
    mxx2 = _mm256_xor_si256(mxx1, *((__m256i *)(temp_chunks_k + slice_offset_1)));
    _mm256_storeu_si256((__m256i *)(temp_chunks_k + slice_offset_1), mxx2);
    mxx2 = _mm256_xor_si256(mxx1, *((__m256i *)(temp_chunks_k1 + slice_offset_2)));
    _mm256_storeu_si256((__m256i *)(temp_chunks_k1 + slice_offset_2), mxx2);
    mxx2 = _mm256_xor_si256(mxx1, *((__m256i *)(temp_chunks_k2 + slice_offset_3)));
    _mm256_storeu_si256((__m256i *)(temp_chunks_k2 + slice_offset_3), mxx2);
  }

  for (int slice_id = 0; slice_id < TOTAL_SLICE_NUM; slice_id++)
  {
    slice_offset_2 = slice_id * SLICE;
    slice_offset_1 = ((slice_id + 6) * SLICE);
    slice_offset_3 = ((slice_id + 1) * SLICE);
    mxx1 = _mm256_loadu_si256((__m256i *)(chunks[6] + slice_offset_2));
    mxx2 = _mm256_xor_si256(mxx1, *((__m256i *)(temp_chunks_k + slice_offset_1)));
    _mm256_storeu_si256((__m256i *)(temp_chunks_k + slice_offset_1), mxx2);
    mxx2 = _mm256_xor_si256(mxx1, *((__m256i *)(temp_chunks_k1 + slice_offset_2)));
    _mm256_storeu_si256((__m256i *)(temp_chunks_k1 + slice_offset_2), mxx2);
    mxx2 = _mm256_xor_si256(mxx1, *((__m256i *)(temp_chunks_k2 + slice_offset_3)));
    _mm256_storeu_si256((__m256i *)(temp_chunks_k2 + slice_offset_3), mxx2);
  }

  for (int slice_id = 0; slice_id < TOTAL_SLICE_NUM; slice_id++)
  {
    slice_offset_2 = slice_id * SLICE;
    slice_offset_1 = ((slice_id + 7) * SLICE);
    slice_offset_3 = slice_id * SLICE;
    mxx1 = _mm256_loadu_si256((__m256i *)(chunks[7] + slice_offset_2));
    mxx2 = _mm256_xor_si256(mxx1, *((__m256i *)(temp_chunks_k + slice_offset_1)));
    _mm256_storeu_si256((__m256i *)(temp_chunks_k + slice_offset_1), mxx2);
    mxx2 = _mm256_xor_si256(mxx1, *((__m256i *)(temp_chunks_k1 + slice_offset_2)));
    _mm256_storeu_si256((__m256i *)(temp_chunks_k1 + slice_offset_2), mxx2);
    mxx2 = _mm256_xor_si256(mxx1, *((__m256i *)(temp_chunks_k2 + slice_offset_3)));
    _mm256_storeu_si256((__m256i *)(temp_chunks_k2 + slice_offset_3), mxx2);
  }

  // Copy the encoded data back to the original chunks and store extra data
  memcpy(chunks[k], temp_chunks_k, (*encoded)[0].length());
  memcpy(chunks[k + 1], temp_chunks_k1, (*encoded)[0].length());
  memcpy(chunks[k + 2], temp_chunks_k2, (*encoded)[0].length());

  std::array<std::array<uint8_t, EXTRA_DATA_SIZE>, PARITY_CHUNKS> extra_data;
  memcpy(extra_data[0].data(), temp_chunks_k + (*encoded)[0].length(), EXTRA_DATA_SIZE);
  memcpy(extra_data[1].data(), temp_chunks_k1 + (*encoded)[0].length(), EXTRA_DATA_SIZE);
  memcpy(extra_data[2].data(), temp_chunks_k2 + (*encoded)[0].length(), EXTRA_DATA_SIZE);

  parity_extra_data[encode_call_count] = extra_data;

  free(temp_chunks_k);
  free(temp_chunks_k1);
  free(temp_chunks_k2);

  return 0;
#else
  return 0;
#endif
}

int ErasureCodeTsx::decode_chunks(const set<int> &want_to_read,
                                  const map<int, bufferlist> &chunks,
                                  map<int, bufferlist> *decoded)
{
#if ENABLE_DECODE
  static int decode_call_count = -1;
  decode_call_count++;

  // Prepare input chunks
  unsigned blocksize = (*chunks.begin()).second.length();
  uint8_t *data[data_chunk_count];
  uint8_t *coding[chunk_count - data_chunk_count];
  for (unsigned int i = 0; i < chunk_count; i++)
  {
    if (i < data_chunk_count)
      data[i] = (uint8_t *)(*decoded)[i].c_str();
    else
      coding[i - data_chunk_count] = (uint8_t *)(*decoded)[i].c_str();
  }

  // Define constants
  const int SLICE = 32;
  const int TOTAL_SLICE_NUM = (blocksize / SLICE);
  const int PARITY_SIZE = blocksize + EXTRA_DATA_SIZE;

  // Shift XOR decode algorithm
  uint64_t slice_offset_1;
  uint64_t slice_offset_2;
  uint64_t slice_offset_3;
  __m256i mxx1, mxx2;

  uint8_t *decode_arr_1 = (uint8_t *)malloc(PARITY_SIZE * sizeof(uint8_t));
  uint8_t *decode_arr_2 = (uint8_t *)malloc(PARITY_SIZE * sizeof(uint8_t));
  uint8_t *decode_arr_3 = (uint8_t *)malloc(PARITY_SIZE * sizeof(uint8_t));
  memcpy(decode_arr_1, coding[0], blocksize);
  memcpy(decode_arr_2, coding[1], blocksize);
  memcpy(decode_arr_3, coding[2], blocksize);

  // Append extra data from the global hash map
  auto extra_data = parity_extra_data[decode_call_count];
  memcpy(decode_arr_1 + blocksize, extra_data[0].data(), EXTRA_DATA_SIZE);
  memcpy(decode_arr_2 + blocksize, extra_data[1].data(), EXTRA_DATA_SIZE);
  memcpy(decode_arr_3 + blocksize, extra_data[2].data(), EXTRA_DATA_SIZE);

  for (int slice_id = 0; slice_id < TOTAL_SLICE_NUM; slice_id++)
  {
    slice_offset_2 = slice_id * SLICE;
    slice_offset_1 = slice_id * SLICE;
    slice_offset_3 = (slice_id + 7) * SLICE;
    mxx1 = _mm256_loadu_si256((__m256i *)(data[0] + slice_offset_2));
    mxx2 = _mm256_xor_si256(mxx1, *((__m256i *)(decode_arr_1 + slice_offset_1)));
    _mm256_storeu_si256((__m256i *)(decode_arr_1 + slice_offset_1), mxx2);
    mxx2 = _mm256_xor_si256(mxx1, *((__m256i *)(decode_arr_2 + slice_offset_2)));
    _mm256_storeu_si256((__m256i *)(decode_arr_2 + slice_offset_2), mxx2);
    mxx2 = _mm256_xor_si256(mxx1, *((__m256i *)(decode_arr_3 + slice_offset_3)));
    _mm256_storeu_si256((__m256i *)(decode_arr_3 + slice_offset_3), mxx2);
  }

  for (int slice_id = 0; slice_id < TOTAL_SLICE_NUM; slice_id++)
  {
    slice_offset_2 = slice_id * SLICE;
    slice_offset_1 = (slice_id + 1) * SLICE;
    slice_offset_3 = (slice_id + 6) * SLICE;
    mxx1 = _mm256_loadu_si256((__m256i *)(data[1] + slice_offset_2));
    mxx2 = _mm256_xor_si256(mxx1, *((__m256i *)(decode_arr_1 + slice_offset_1)));
    _mm256_storeu_si256((__m256i *)(decode_arr_1 + slice_offset_1), mxx2);
    mxx2 = _mm256_xor_si256(mxx1, *((__m256i *)(decode_arr_2 + slice_offset_2)));
    _mm256_storeu_si256((__m256i *)(decode_arr_2 + slice_offset_2), mxx2);
    mxx2 = _mm256_xor_si256(mxx1, *((__m256i *)(decode_arr_3 + slice_offset_3)));
    _mm256_storeu_si256((__m256i *)(decode_arr_3 + slice_offset_3), mxx2);
  }

  for (int slice_id = 0; slice_id < TOTAL_SLICE_NUM; slice_id++)
  {
    slice_offset_2 = slice_id * SLICE;
    slice_offset_1 = (slice_id + 2) * SLICE;
    slice_offset_3 = (slice_id + 5) * SLICE;
    mxx1 = _mm256_loadu_si256((__m256i *)(data[2] + slice_offset_2));
    mxx2 = _mm256_xor_si256(mxx1, *((__m256i *)(decode_arr_1 + slice_offset_1)));
    _mm256_storeu_si256((__m256i *)(decode_arr_1 + slice_offset_1), mxx2);
    mxx2 = _mm256_xor_si256(mxx1, *((__m256i *)(decode_arr_2 + slice_offset_2)));
    _mm256_storeu_si256((__m256i *)(decode_arr_2 + slice_offset_2), mxx2);
    mxx2 = _mm256_xor_si256(mxx1, *((__m256i *)(decode_arr_3 + slice_offset_3)));
    _mm256_storeu_si256((__m256i *)(decode_arr_3 + slice_offset_3), mxx2);
  }

  for (int slice_id = 0; slice_id < TOTAL_SLICE_NUM; slice_id++)
  {
    slice_offset_2 = slice_id * SLICE;
    slice_offset_1 = (slice_id + 3) * SLICE;
    slice_offset_3 = (slice_id + 4) * SLICE;
    mxx1 = _mm256_loadu_si256((__m256i *)(data[3] + slice_offset_2));
    mxx2 = _mm256_xor_si256(mxx1, *((__m256i *)(decode_arr_1 + slice_offset_1)));
    _mm256_storeu_si256((__m256i *)(decode_arr_1 + slice_offset_1), mxx2);
    mxx2 = _mm256_xor_si256(mxx1, *((__m256i *)(decode_arr_2 + slice_offset_2)));
    _mm256_storeu_si256((__m256i *)(decode_arr_2 + slice_offset_2), mxx2);
    mxx2 = _mm256_xor_si256(mxx1, *((__m256i *)(decode_arr_3 + slice_offset_3)));
    _mm256_storeu_si256((__m256i *)(decode_arr_3 + slice_offset_3), mxx2);
  }

  for (int slice_id = 0; slice_id < TOTAL_SLICE_NUM; slice_id++)
  {
    slice_offset_2 = slice_id * SLICE;
    slice_offset_1 = (slice_id + 4) * SLICE;
    slice_offset_3 = (slice_id + 3) * SLICE;
    mxx1 = _mm256_loadu_si256((__m256i *)(data[4] + slice_offset_2));
    mxx2 = _mm256_xor_si256(mxx1, *((__m256i *)(decode_arr_1 + slice_offset_1)));
    _mm256_storeu_si256((__m256i *)(decode_arr_1 + slice_offset_1), mxx2);
    mxx2 = _mm256_xor_si256(mxx1, *((__m256i *)(decode_arr_2 + slice_offset_2)));
    _mm256_storeu_si256((__m256i *)(decode_arr_2 + slice_offset_2), mxx2);
    mxx2 = _mm256_xor_si256(mxx1, *((__m256i *)(decode_arr_3 + slice_offset_3)));
    _mm256_storeu_si256((__m256i *)(decode_arr_3 + slice_offset_3), mxx2);
  }

  // Solve chunk 6, 7, 8 in 256-bitwise
  for (int slice_id = 0; slice_id < TOTAL_SLICE_NUM; slice_id++)
  {
    // Step 1: solve 256 bits in packet 6 
    slice_offset_1 = ((slice_id + 5) * SLICE);
    slice_offset_2 = slice_id * SLICE;
    slice_offset_3 = ((slice_id + 2) * SLICE);

    mxx1 = _mm256_loadu_si256((__m256i *) &decode_arr_1[slice_offset_1]);
    _mm256_storeu_si256((__m256i *) &data[5][slice_offset_2], mxx1);

    mxx2 = _mm256_xor_si256(mxx1, *((__m256i *) &decode_arr_2[slice_offset_2]));
    _mm256_storeu_si256((__m256i *) &decode_arr_2[slice_offset_2], mxx2);
    mxx2 = _mm256_xor_si256(mxx1, *((__m256i *) &decode_arr_3[slice_offset_3]));
    _mm256_storeu_si256((__m256i *) &decode_arr_3[slice_offset_3], mxx2);

    // Step 2: solve 256 bits in packet 8
    slice_offset_1 = ((slice_id + 7) * SLICE);
    slice_offset_2 = slice_id * SLICE;
    slice_offset_3 = slice_id * SLICE;

    mxx1 = _mm256_loadu_si256((__m256i *) &decode_arr_3[slice_offset_3]);
    _mm256_storeu_si256((__m256i *) &data[7][slice_offset_2], mxx1);

    mxx2 = _mm256_xor_si256(mxx1, *((__m256i *) &decode_arr_1[slice_offset_1]));
    _mm256_storeu_si256((__m256i *) &decode_arr_1[slice_offset_1], mxx2);
    mxx2 = _mm256_xor_si256(mxx1, *((__m256i *) &decode_arr_2[slice_offset_2]));
    _mm256_storeu_si256((__m256i *) &decode_arr_2[slice_offset_2], mxx2);

    // Step 3: solve 256 bits in packet 7
    slice_offset_1 = ((slice_id + 6) * SLICE);
    slice_offset_2 = slice_id * SLICE;
    slice_offset_3 = ((slice_id + 1) * SLICE);

    mxx1 = _mm256_loadu_si256((__m256i *) &decode_arr_2[slice_offset_2]);
    _mm256_storeu_si256((__m256i *) &data[6][slice_offset_2], mxx1);

    mxx2 = _mm256_xor_si256(mxx1, *((__m256i *) &decode_arr_1[slice_offset_1]));
    _mm256_storeu_si256((__m256i *) &decode_arr_1[slice_offset_1], mxx2);
    mxx2 = _mm256_xor_si256(mxx1, *((__m256i *) &decode_arr_3[slice_offset_3]));
    _mm256_storeu_si256((__m256i *) &decode_arr_3[slice_offset_3], mxx2);
  }

  free(decode_arr_1);
  free(decode_arr_2);
  free(decode_arr_3);

  return 0;
#else
  return 0;
#endif
}
