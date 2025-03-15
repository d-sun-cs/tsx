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

#include "common/debug.h"
#include "include/stringify.h"
#include "erasure-code/ErasureCodePlugin.h"

#include "ErasureCodeTsx.h"

#define dout_context g_ceph_context
#define dout_subsys ceph_subsys_osd
#undef dout_prefix
#define dout_prefix _prefix(_dout)

#define EXTRA_DATA_SIZE 64
#define PARITY_CHUNKS 3
#define ENABLE_LOGGING 1

using namespace std;
using namespace ceph;

static ostream& _prefix(std::ostream* _dout)
{
  return *_dout << "ErasureCodeTsx: ";
}

unsigned int ErasureCodeTsx::get_chunk_size(unsigned int object_size) const
{
  // According to the comment, calculate the chunk size
  return (object_size + data_chunk_count - 1) / data_chunk_count;
}

int ErasureCodeTsx::encode_chunks(const set<int> &want_to_encode,
                                  map<int, bufferlist> *encoded)
{

#if ENABLE_LOGGING
  ofstream encode_log("mylog-encode.txt", ios_base::app);
#endif

  // Prepare input chunks
  uint8_t *chunks[chunk_count];
  for (unsigned int i = 0; i < chunk_count; ++i)
  {
    chunks[i] = (uint8_t *)(*encoded)[i].c_str();
  }

  // Calculate k and m
  const int k = data_chunk_count;

  // Define constants
  const int SLICE = 8;
  const unsigned int blocksize = (*encoded)[0].length();
  const int TOTAL_SLICE_NUM = blocksize / SLICE;
  const int PARITY_SIZE = blocksize + EXTRA_DATA_SIZE;

#if ENABLE_LOGGING
  encode_log << "Block size: " << blocksize << std::endl;
#endif

  // Allocate temporary arrays for parity chunks
  uint8_t *temp_chunks_k = new uint8_t[PARITY_SIZE]();
  uint8_t *temp_chunks_k1 = new uint8_t[PARITY_SIZE]();
  uint8_t *temp_chunks_k2 = new uint8_t[PARITY_SIZE]();

  // Shift XOR encode algorithm
  uint64_t slice_offset_1;
  uint64_t slice_offset_2;
  uint64_t slice_offset_3;

  for (int slice_id = 0; slice_id < TOTAL_SLICE_NUM; slice_id++)
  {
    slice_offset_2 = slice_id * SLICE;
    slice_offset_1 = slice_id * SLICE;
    slice_offset_3 = ((slice_id + 7) * SLICE);
    *(uint64_t *)(temp_chunks_k + slice_offset_1) = *(uint64_t *)(chunks[0] + slice_offset_2);
    *(uint64_t *)(temp_chunks_k1 + slice_offset_2) = *(uint64_t *)(chunks[0] + slice_offset_2);
    *(uint64_t *)(temp_chunks_k2 + slice_offset_3) = *(uint64_t *)(chunks[0] + slice_offset_2);
  }

  for (int slice_id = 0; slice_id < TOTAL_SLICE_NUM; slice_id++)
  {
    slice_offset_2 = slice_id * SLICE;
    slice_offset_1 = ((slice_id + 1) * SLICE);
    slice_offset_3 = ((slice_id + 6) * SLICE);
    *(uint64_t *)(temp_chunks_k + slice_offset_1) ^= *(uint64_t *)(chunks[1] + slice_offset_2);
    *(uint64_t *)(temp_chunks_k1 + slice_offset_2) ^= *(uint64_t *)(chunks[1] + slice_offset_2);
    *(uint64_t *)(temp_chunks_k2 + slice_offset_3) ^= *(uint64_t *)(chunks[1] + slice_offset_2);
  }

  for (int slice_id = 0; slice_id < TOTAL_SLICE_NUM; slice_id++)
  {
    slice_offset_2 = slice_id * SLICE;
    slice_offset_1 = ((slice_id + 2) * SLICE);
    slice_offset_3 = ((slice_id + 5) * SLICE);
    *(uint64_t *)(temp_chunks_k + slice_offset_1) ^= *(uint64_t *)(chunks[2] + slice_offset_2);
    *(uint64_t *)(temp_chunks_k1 + slice_offset_2) ^= *(uint64_t *)(chunks[2] + slice_offset_2);
    *(uint64_t *)(temp_chunks_k2 + slice_offset_3) ^= *(uint64_t *)(chunks[2] + slice_offset_2);
  }

  for (int slice_id = 0; slice_id < TOTAL_SLICE_NUM; slice_id++)
  {
    slice_offset_2 = slice_id * SLICE;
    slice_offset_1 = ((slice_id + 3) * SLICE);
    slice_offset_3 = ((slice_id + 4) * SLICE);
    *(uint64_t *)(temp_chunks_k + slice_offset_1) ^= *(uint64_t *)(chunks[3] + slice_offset_2);
    *(uint64_t *)(temp_chunks_k1 + slice_offset_2) ^= *(uint64_t *)(chunks[3] + slice_offset_2);
    *(uint64_t *)(temp_chunks_k2 + slice_offset_3) ^= *(uint64_t *)(chunks[3] + slice_offset_2);
  }

  for (int slice_id = 0; slice_id < TOTAL_SLICE_NUM; slice_id++)
  {
    slice_offset_2 = slice_id * SLICE;
    slice_offset_1 = ((slice_id + 4) * SLICE);
    slice_offset_3 = ((slice_id + 3) * SLICE);
    *(uint64_t *)(temp_chunks_k + slice_offset_1) ^= *(uint64_t *)(chunks[4] + slice_offset_2);
    *(uint64_t *)(temp_chunks_k1 + slice_offset_2) ^= *(uint64_t *)(chunks[4] + slice_offset_2);
    *(uint64_t *)(temp_chunks_k2 + slice_offset_3) ^= *(uint64_t *)(chunks[4] + slice_offset_2);
  }

  for (int slice_id = 0; slice_id < TOTAL_SLICE_NUM; slice_id++)
  {
    slice_offset_2 = slice_id * SLICE;
    slice_offset_1 = ((slice_id + 5) * SLICE);
    slice_offset_3 = ((slice_id + 2) * SLICE);
    *(uint64_t *)(temp_chunks_k + slice_offset_1) ^= *(uint64_t *)(chunks[5] + slice_offset_2);
    *(uint64_t *)(temp_chunks_k1 + slice_offset_2) ^= *(uint64_t *)(chunks[5] + slice_offset_2);
    *(uint64_t *)(temp_chunks_k2 + slice_offset_3) ^= *(uint64_t *)(chunks[5] + slice_offset_2);
  }

  for (int slice_id = 0; slice_id < TOTAL_SLICE_NUM; slice_id++)
  {
    slice_offset_2 = slice_id * SLICE;
    slice_offset_1 = ((slice_id + 6) * SLICE);
    slice_offset_3 = ((slice_id + 1) * SLICE);
    *(uint64_t *)(temp_chunks_k + slice_offset_1) ^= *(uint64_t *)(chunks[6] + slice_offset_2);
    *(uint64_t *)(temp_chunks_k1 + slice_offset_2) ^= *(uint64_t *)(chunks[6] + slice_offset_2);
    *(uint64_t *)(temp_chunks_k2 + slice_offset_3) ^= *(uint64_t *)(chunks[6] + slice_offset_2);
  }

  for (int slice_id = 0; slice_id < TOTAL_SLICE_NUM; slice_id++)
  {
    slice_offset_2 = slice_id * SLICE;
    slice_offset_1 = ((slice_id + 7) * SLICE);
    slice_offset_3 = slice_id * SLICE;
    *(uint64_t *)(temp_chunks_k + slice_offset_1) ^= *(uint64_t *)(chunks[7] + slice_offset_2);
    *(uint64_t *)(temp_chunks_k1 + slice_offset_2) ^= *(uint64_t *)(chunks[7] + slice_offset_2);
    *(uint64_t *)(temp_chunks_k2 + slice_offset_3) ^= *(uint64_t *)(chunks[7] + slice_offset_2);
  }

  // Copy the encoded data back to the original chunks and store extra data
  memcpy(chunks[k],     temp_chunks_k,  blocksize);
  memcpy(chunks[k + 1], temp_chunks_k1, blocksize);
  memcpy(chunks[k + 2], temp_chunks_k2, blocksize);

  // Store extra data in the last chunk
  memcpy(chunks[k + PARITY_CHUNKS],                       temp_chunks_k  + blocksize, EXTRA_DATA_SIZE);
  memcpy(chunks[k + PARITY_CHUNKS] + EXTRA_DATA_SIZE,     temp_chunks_k1 + blocksize, EXTRA_DATA_SIZE);
  memcpy(chunks[k + PARITY_CHUNKS] + 2 * EXTRA_DATA_SIZE, temp_chunks_k2 + blocksize, EXTRA_DATA_SIZE);

  // Log the extra data for debugging
#if ENABLE_LOGGING
  encode_log << "Extra data for encode" << std::endl;
  for (int i = 0; i < PARITY_CHUNKS; ++i) {
    encode_log << "  Chunk " << i << ": ";
    for (int j = 0; j < EXTRA_DATA_SIZE; ++j) {
      encode_log << std::hex << (int)chunks[k + PARITY_CHUNKS][i * EXTRA_DATA_SIZE + j] << " ";
    }
    encode_log << std::endl;
  }

  // Log the first few bytes of the parity chunks
  encode_log << "First few bytes of parity chunks for encode" << std::endl;
  for (int i = 0; i < PARITY_CHUNKS; ++i) {
    encode_log << "  Parity Chunk " << i << ": ";
    for (int j = 0; j < 8; ++j) { // Log first 8 bytes
      encode_log << std::hex << (int)chunks[k + i][j] << " ";
    }
    encode_log << std::endl;
  }
  encode_log.close();
#endif

  delete[] temp_chunks_k;
  delete[] temp_chunks_k1;
  delete[] temp_chunks_k2;

  return 0;
}

int ErasureCodeTsx::decode_chunks(const set<int> &want_to_read,
                                  const map<int, bufferlist> &chunks,
                                  map<int, bufferlist> *decoded)
{

#if ENABLE_LOGGING
  ofstream decode_log("mylog-decode.txt", ios_base::app);
#endif

  // Prepare input chunks
  unsigned blocksize = (*chunks.begin()).second.length();
  int erasures[chunk_count];
  int erasures_count = 0;
  uint8_t *data[data_chunk_count];
  uint8_t *coding[chunk_count - data_chunk_count];
  for (unsigned int i = 0; i < chunk_count; i++)
  {
    if (chunks.find(i) == chunks.end()) {
      erasures[erasures_count] = i;
      erasures_count++;
    }
    if (i < data_chunk_count)
      data[i] = (uint8_t *)(*decoded)[i].c_str();
    else
      coding[i - data_chunk_count] = (uint8_t *)(*decoded)[i].c_str();
  }
  erasures[erasures_count] = -1;
  ceph_assert(erasures_count > 0);

  // Define constants
  const int SLICE = 8;
  const int TOTAL_SLICE_NUM = blocksize / SLICE;
  const int PARITY_SIZE = blocksize + EXTRA_DATA_SIZE;

#if ENABLE_LOGGING
  decode_log << "Block size: " << blocksize << std::endl;
  for (int i = 0; i < erasures_count; ++i) {
    decode_log << "Erasure " << i << ": " << erasures[i] << std::endl;
  }
#endif

  // Shift XOR decode algorithm
  uint64_t slice_offset_1;
  uint64_t slice_offset_2;
  uint64_t slice_offset_3;

  uint8_t *decode_arr_1 = (uint8_t *)malloc(PARITY_SIZE * sizeof(uint8_t));
  uint8_t *decode_arr_2 = (uint8_t *)malloc(PARITY_SIZE * sizeof(uint8_t));
  uint8_t *decode_arr_3 = (uint8_t *)malloc(PARITY_SIZE * sizeof(uint8_t));
  memcpy(decode_arr_1, coding[0], blocksize);
  memcpy(decode_arr_2, coding[1], blocksize);
  memcpy(decode_arr_3, coding[2], blocksize);

  // Append extra data from the last chunk
  memcpy(decode_arr_1 + blocksize, coding[PARITY_CHUNKS],                       EXTRA_DATA_SIZE);
  memcpy(decode_arr_2 + blocksize, coding[PARITY_CHUNKS] + EXTRA_DATA_SIZE,     EXTRA_DATA_SIZE);
  memcpy(decode_arr_3 + blocksize, coding[PARITY_CHUNKS] + 2 * EXTRA_DATA_SIZE, EXTRA_DATA_SIZE);

  // Log the extra data for debugging
#if ENABLE_LOGGING
  decode_log << "Extra data for decode call" << std::endl;
  for (int i = 0; i < PARITY_CHUNKS; ++i) {
    decode_log << "  Chunk " << i << ": ";
    for (int j = 0; j < EXTRA_DATA_SIZE; ++j) {
      decode_log << std::hex << (int)coding[PARITY_CHUNKS][i * EXTRA_DATA_SIZE + j] << " ";
    }
    decode_log << std::endl;
  }

  // Log the first few bytes of the parity chunks
  decode_log << "First few bytes of parity chunks for decode call" << std::endl;
  for (int i = 0; i < PARITY_CHUNKS; ++i) {
    decode_log << "  Parity Chunk " << i << ": ";
    for (int j = 0; j < 8; ++j) { // Log first 8 bytes
      decode_log << std::hex << (int)coding[i][j] << " ";
    }
    decode_log << std::endl;
  }
  decode_log.close();
#endif

  for (int slice_id = 0; slice_id < TOTAL_SLICE_NUM; slice_id++)
  {
    slice_offset_2 = slice_id * SLICE;
    slice_offset_1 = slice_id * SLICE;
    slice_offset_3 = (slice_id + 7) * SLICE;

    *(uint64_t *)(decode_arr_1 + slice_offset_1) ^= *(uint64_t *)(data[0] + slice_offset_2);
    *(uint64_t *)(decode_arr_2 + slice_offset_2) ^= *(uint64_t *)(data[0] + slice_offset_2);
    *(uint64_t *)(decode_arr_3 + slice_offset_3) ^= *(uint64_t *)(data[0] + slice_offset_2);
  }

  for (int slice_id = 0; slice_id < TOTAL_SLICE_NUM; slice_id++)
  {
    slice_offset_2 = slice_id * SLICE;
    slice_offset_1 = (slice_id + 1) * SLICE;
    slice_offset_3 = (slice_id + 6) * SLICE;

    *(uint64_t *)(decode_arr_1 + slice_offset_1) ^= *(uint64_t *)(data[1] + slice_offset_2);
    *(uint64_t *)(decode_arr_2 + slice_offset_2) ^= *(uint64_t *)(data[1] + slice_offset_2);
    *(uint64_t *)(decode_arr_3 + slice_offset_3) ^= *(uint64_t *)(data[1] + slice_offset_2);
  }

  for (int slice_id = 0; slice_id < TOTAL_SLICE_NUM; slice_id++)
  {
    slice_offset_2 = slice_id * SLICE;
    slice_offset_1 = (slice_id + 2) * SLICE;
    slice_offset_3 = (slice_id + 5) * SLICE;

    *(uint64_t *)(decode_arr_1 + slice_offset_1) ^= *(uint64_t *)(data[2] + slice_offset_2);
    *(uint64_t *)(decode_arr_2 + slice_offset_2) ^= *(uint64_t *)(data[2] + slice_offset_2);
    *(uint64_t *)(decode_arr_3 + slice_offset_3) ^= *(uint64_t *)(data[2] + slice_offset_2);
  }

  for (int slice_id = 0; slice_id < TOTAL_SLICE_NUM; slice_id++)
  {
    slice_offset_2 = slice_id * SLICE;
    slice_offset_1 = (slice_id + 3) * SLICE;
    slice_offset_3 = (slice_id + 4) * SLICE;

    *(uint64_t *)(decode_arr_1 + slice_offset_1) ^= *(uint64_t *)(data[3] + slice_offset_2);
    *(uint64_t *)(decode_arr_2 + slice_offset_2) ^= *(uint64_t *)(data[3] + slice_offset_2);
    *(uint64_t *)(decode_arr_3 + slice_offset_3) ^= *(uint64_t *)(data[3] + slice_offset_2);
  }

  for (int slice_id = 0; slice_id < TOTAL_SLICE_NUM; slice_id++)
  {
    slice_offset_2 = slice_id * SLICE;
    slice_offset_1 = (slice_id + 4) * SLICE;
    slice_offset_3 = (slice_id + 3) * SLICE;

    *(uint64_t *)(decode_arr_1 + slice_offset_1) ^= *(uint64_t *)(data[4] + slice_offset_2);
    *(uint64_t *)(decode_arr_2 + slice_offset_2) ^= *(uint64_t *)(data[4] + slice_offset_2);
    *(uint64_t *)(decode_arr_3 + slice_offset_3) ^= *(uint64_t *)(data[4] + slice_offset_2);
  }

  for (int slice_id = 0; slice_id < TOTAL_SLICE_NUM; slice_id++)
  {
    slice_offset_1 = ((slice_id + 5) * SLICE);
    slice_offset_2 = slice_id * SLICE;
    slice_offset_3 = ((slice_id) + 2) * SLICE;

    *(uint64_t *)(data[5] + slice_offset_2) = *(uint64_t *)(decode_arr_1 + slice_offset_1);
    *(uint64_t *)(decode_arr_2 + slice_offset_2) ^= *(uint64_t *)(decode_arr_1 + slice_offset_1);
    *(uint64_t *)(decode_arr_3 + slice_offset_3) ^= *(uint64_t *)(decode_arr_1 + slice_offset_1);

    slice_offset_1 = ((slice_id + 7) * SLICE);
    slice_offset_2 = slice_id * SLICE;
    slice_offset_3 = slice_id * SLICE;

    *(uint64_t *)(data[7] + slice_offset_2) = *(uint64_t *)(decode_arr_3 + slice_offset_3);
    *(uint64_t *)(decode_arr_1 + slice_offset_1) ^= *(uint64_t *)(decode_arr_3 + slice_offset_3);
    *(uint64_t *)(decode_arr_2 + slice_offset_2) ^= *(uint64_t *)(decode_arr_3 + slice_offset_3);

    slice_offset_1 = ((slice_id + 6) * SLICE);
    slice_offset_2 = slice_id * SLICE;
    slice_offset_3 = ((slice_id + 1) * SLICE);

    *(uint64_t *)(data[6] + slice_offset_2) = *(uint64_t *)(decode_arr_2 + slice_offset_2);
    *(uint64_t *)(decode_arr_1 + slice_offset_1) ^= *(uint64_t *)(decode_arr_2 + slice_offset_2);
    *(uint64_t *)(decode_arr_3 + slice_offset_3) ^= *(uint64_t *)(decode_arr_2 + slice_offset_2);
  }

  free(decode_arr_1);
  free(decode_arr_2);
  free(decode_arr_3);

  return 0;
}
