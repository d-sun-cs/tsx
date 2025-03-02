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

#include "common/debug.h"
#include "include/stringify.h"
#include "erasure-code/ErasureCodePlugin.h"

#include "ErasureCodeTsx.h"

#define dout_context g_ceph_context
#define dout_subsys ceph_subsys_osd
#undef dout_prefix
#define dout_prefix _prefix(_dout)

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
  // Debug output
  dout(10) << "encode_chunks: want_to_encode size = " << want_to_encode.size() << dendl;

  // Prepare input chunks
  uint8_t* chunks[chunk_count];
  for (unsigned int i = 0; i < chunk_count; ++i) {
    chunks[i] = (uint8_t*)(*encoded)[i].c_str();
  }

  // Calculate k and m
  const int k = data_chunk_count;

  // Define constants
  const int SLICE = 8;
  const int TOTAL_SLICE_NUM = (get_chunk_size((*encoded)[0].length()) / SLICE);

  // Shift XOR encode algorithm
  uint64_t slice_offset_1;
  uint64_t slice_offset_2;
  uint64_t slice_offset_3;

  for (int slice_id = 0; slice_id < TOTAL_SLICE_NUM; slice_id++) {
    slice_offset_2 = slice_id * SLICE;
    slice_offset_1 = slice_id * SLICE;
    slice_offset_3 = ((slice_id + 7) * SLICE);
    *(uint64_t*)(chunks[k] + slice_offset_1) = *(uint64_t*)(chunks[0] + slice_offset_2);
    *(uint64_t*)(chunks[k + 1] + slice_offset_2) = *(uint64_t*)(chunks[0] + slice_offset_2);
    *(uint64_t*)(chunks[k + 2] + slice_offset_3) = *(uint64_t*)(chunks[0] + slice_offset_2);
  }

  for (int slice_id = 0; slice_id < TOTAL_SLICE_NUM; slice_id++) {
    slice_offset_2 = slice_id * SLICE;
    slice_offset_1 = ((slice_id + 1) * SLICE);
    slice_offset_3 = ((slice_id + 6) * SLICE);
    *(uint64_t*)(chunks[k] + slice_offset_1) ^= *(uint64_t*)(chunks[1] + slice_offset_2);
    *(uint64_t*)(chunks[k + 1] + slice_offset_2) ^= *(uint64_t*)(chunks[1] + slice_offset_2);
    *(uint64_t*)(chunks[k + 2] + slice_offset_3) ^= *(uint64_t*)(chunks[1] + slice_offset_2);
  }

  for (int slice_id = 0; slice_id < TOTAL_SLICE_NUM; slice_id++) {
    slice_offset_2 = slice_id * SLICE;
    slice_offset_1 = ((slice_id + 2) * SLICE);
    slice_offset_3 = ((slice_id + 5) * SLICE);
    *(uint64_t*)(chunks[k] + slice_offset_1) ^= *(uint64_t*)(chunks[2] + slice_offset_2);
    *(uint64_t*)(chunks[k + 1] + slice_offset_2) ^= *(uint64_t*)(chunks[2] + slice_offset_2);
    *(uint64_t*)(chunks[k + 2] + slice_offset_3) ^= *(uint64_t*)(chunks[2] + slice_offset_2);
  }

  for (int slice_id = 0; slice_id < TOTAL_SLICE_NUM; slice_id++) {
    slice_offset_2 = slice_id * SLICE;
    slice_offset_1 = ((slice_id + 3) * SLICE);
    slice_offset_3 = ((slice_id + 4) * SLICE);
    *(uint64_t*)(chunks[k] + slice_offset_1) ^= *(uint64_t*)(chunks[3] + slice_offset_2);
    *(uint64_t*)(chunks[k + 1] + slice_offset_2) ^= *(uint64_t*)(chunks[3] + slice_offset_2);
    *(uint64_t*)(chunks[k + 2] + slice_offset_3) ^= *(uint64_t*)(chunks[3] + slice_offset_2);
  }

  for (int slice_id = 0; slice_id < TOTAL_SLICE_NUM; slice_id++) {
    slice_offset_2 = slice_id * SLICE;
    slice_offset_1 = ((slice_id + 4) * SLICE);
    slice_offset_3 = ((slice_id + 3) * SLICE);
    *(uint64_t*)(chunks[k] + slice_offset_1) ^= *(uint64_t*)(chunks[4] + slice_offset_2);
    *(uint64_t*)(chunks[k + 1] + slice_offset_2) ^= *(uint64_t*)(chunks[4] + slice_offset_2);
    *(uint64_t*)(chunks[k + 2] + slice_offset_3) ^= *(uint64_t*)(chunks[4] + slice_offset_2);
  }

  for (int slice_id = 0; slice_id < TOTAL_SLICE_NUM; slice_id++) {
    slice_offset_2 = slice_id * SLICE;
    slice_offset_1 = ((slice_id + 5) * SLICE);
    slice_offset_3 = ((slice_id + 2) * SLICE);
    *(uint64_t*)(chunks[k] + slice_offset_1) ^= *(uint64_t*)(chunks[5] + slice_offset_2);
    *(uint64_t*)(chunks[k + 1] + slice_offset_2) ^= *(uint64_t*)(chunks[5] + slice_offset_2);
    *(uint64_t*)(chunks[k + 2] + slice_offset_3) ^= *(uint64_t*)(chunks[5] + slice_offset_2);
  }

  for (int slice_id = 0; slice_id < TOTAL_SLICE_NUM; slice_id++) {
    slice_offset_2 = slice_id * SLICE;
    slice_offset_1 = ((slice_id + 6) * SLICE);
    slice_offset_3 = ((slice_id + 1) * SLICE);
    *(uint64_t*)(chunks[k] + slice_offset_1) ^= *(uint64_t*)(chunks[6] + slice_offset_2);
    *(uint64_t*)(chunks[k + 1] + slice_offset_2) ^= *(uint64_t*)(chunks[6] + slice_offset_2);
    *(uint64_t*)(chunks[k + 2] + slice_offset_3) ^= *(uint64_t*)(chunks[6] + slice_offset_2);
  }

  for (int slice_id = 0; slice_id < TOTAL_SLICE_NUM; slice_id++) {
    slice_offset_2 = slice_id * SLICE;
    slice_offset_1 = ((slice_id + 7) * SLICE);
    slice_offset_3 = slice_id * SLICE;
    *(uint64_t*)(chunks[k] + slice_offset_1) ^= *(uint64_t*)(chunks[7] + slice_offset_2);
    *(uint64_t*)(chunks[k + 1] + slice_offset_2) ^= *(uint64_t*)(chunks[7] + slice_offset_2);
    *(uint64_t*)(chunks[k + 2] + slice_offset_3) ^= *(uint64_t*)(chunks[7] + slice_offset_2);
  }

  return 0;
}

int ErasureCodeTsx::decode_chunks(const set<int> &want_to_read,
				  const map<int, bufferlist> &chunks,
				  map<int, bufferlist> *decoded)
{
  // Debug output
  dout(10) << "decode_chunks: want_to_read size = " << want_to_read.size() << dendl;

  // Prepare input chunks
  unsigned blocksize = (*chunks.begin()).second.length();
  int erasures[chunk_count + 1];
  int erasures_count = 0;
  uint8_t* data[data_chunk_count];
  uint8_t* coding[chunk_count - data_chunk_count];
  for (unsigned int i = 0; i < chunk_count; i++) {
    if (chunks.find(i) == chunks.end()) {
      erasures[erasures_count] = i;
      erasures_count++;
    }
    if (i < data_chunk_count)
      data[i] = (uint8_t*)(*decoded)[i].c_str();
    else
      coding[i - data_chunk_count] = (uint8_t*)(*decoded)[i].c_str();
  }
  erasures[erasures_count] = -1;

  ceph_assert(erasures_count > 0);

  // Calculate k and m
  const int k = data_chunk_count;

  // Define constants
  const int SLICE = 8;
  const int TOTAL_SLICE_NUM = (blocksize / SLICE);
  const int PARITY_SIZE = blocksize + (SLICE * 20);

  // Shift XOR decode algorithm
  uint64_t slice_offset_1;
  uint64_t slice_offset_2;
  uint64_t slice_offset_3;

  uint8_t* decode_arr_1 = (uint8_t*) malloc(PARITY_SIZE * sizeof(uint8_t));
  uint8_t* decode_arr_2 = (uint8_t*) malloc(PARITY_SIZE * sizeof(uint8_t));
  uint8_t* decode_arr_3 = (uint8_t*) malloc(PARITY_SIZE * sizeof(uint8_t));
  memcpy(decode_arr_1, coding[0], PARITY_SIZE);
  memcpy(decode_arr_2, coding[1], PARITY_SIZE);
  memcpy(decode_arr_3, coding[2], PARITY_SIZE);

  uint8_t output_chunk_6[blocksize];
  uint8_t output_chunk_7[blocksize];
  uint8_t output_chunk_8[blocksize];

  for (int slice_id = 0; slice_id < TOTAL_SLICE_NUM; slice_id++) {
    slice_offset_2 = slice_id * SLICE;
    slice_offset_1 = slice_id * SLICE;
    slice_offset_3 = (slice_id + 7) * SLICE;

    *(uint64_t*)(decode_arr_1 + slice_offset_1) ^= *(uint64_t*)(data[0] + slice_offset_2);
    *(uint64_t*)(decode_arr_2 + slice_offset_2) ^= *(uint64_t*)(data[0] + slice_offset_2);
    *(uint64_t*)(decode_arr_3 + slice_offset_3) ^= *(uint64_t*)(data[0] + slice_offset_2);
  }

  for (int slice_id = 0; slice_id < TOTAL_SLICE_NUM; slice_id++) {
    slice_offset_2 = slice_id * SLICE;
    slice_offset_1 = (slice_id + 1) * SLICE;
    slice_offset_3 = (slice_id + 6) * SLICE;

    *(uint64_t*)(decode_arr_1 + slice_offset_1) ^= *(uint64_t*)(data[1] + slice_offset_2);
    *(uint64_t*)(decode_arr_2 + slice_offset_2) ^= *(uint64_t*)(data[1] + slice_offset_2);
    *(uint64_t*)(decode_arr_3 + slice_offset_3) ^= *(uint64_t*)(data[1] + slice_offset_2);
  }

  for (int slice_id = 0; slice_id < TOTAL_SLICE_NUM; slice_id++) {
    slice_offset_2 = slice_id * SLICE;
    slice_offset_1 = (slice_id + 2) * SLICE;
    slice_offset_3 = (slice_id + 5) * SLICE;

    *(uint64_t*)(decode_arr_1 + slice_offset_1) ^= *(uint64_t*)(data[2] + slice_offset_2);
    *(uint64_t*)(decode_arr_2 + slice_offset_2) ^= *(uint64_t*)(data[2] + slice_offset_2);
    *(uint64_t*)(decode_arr_3 + slice_offset_3) ^= *(uint64_t*)(data[2] + slice_offset_2);
  }

  for (int slice_id = 0; slice_id < TOTAL_SLICE_NUM; slice_id++) {
    slice_offset_2 = slice_id * SLICE;
    slice_offset_1 = (slice_id + 3) * SLICE;
    slice_offset_3 = (slice_id + 4) * SLICE;

    *(uint64_t*)(decode_arr_1 + slice_offset_1) ^= *(uint64_t*)(data[3] + slice_offset_2);
    *(uint64_t*)(decode_arr_2 + slice_offset_2) ^= *(uint64_t*)(data[3] + slice_offset_2);
    *(uint64_t*)(decode_arr_3 + slice_offset_3) ^= *(uint64_t*)(data[3] + slice_offset_2);
  }

  for (int slice_id = 0; slice_id < TOTAL_SLICE_NUM; slice_id++) {
    slice_offset_2 = slice_id * SLICE;
    slice_offset_1 = (slice_id + 4) * SLICE;
    slice_offset_3 = (slice_id + 3) * SLICE;

    *(uint64_t*)(decode_arr_1 + slice_offset_1) ^= *(uint64_t*)(data[4] + slice_offset_2);
    *(uint64_t*)(decode_arr_2 + slice_offset_2) ^= *(uint64_t*)(data[4] + slice_offset_2);
    *(uint64_t*)(decode_arr_3 + slice_offset_3) ^= *(uint64_t*)(data[4] + slice_offset_2);
  }

  for (int slice_id = 0; slice_id < TOTAL_SLICE_NUM; slice_id++) {
    slice_offset_1 = ((slice_id + 5) * SLICE);
    slice_offset_2 = slice_id * SLICE;
    slice_offset_3 = ((slice_id) + 2) * SLICE;

    *(uint64_t*)(output_chunk_6 + slice_offset_2) = *(uint64_t*)(decode_arr_1 + slice_offset_1);
    *(uint64_t*)(decode_arr_2 + slice_offset_2) ^= *(uint64_t*)(decode_arr_1 + slice_offset_1);
    *(uint64_t*)(decode_arr_3 + slice_offset_3) ^= *(uint64_t*)(decode_arr_1 + slice_offset_1);
  }

  for (int slice_id = 0; slice_id < TOTAL_SLICE_NUM; slice_id++) {
    slice_offset_1 = ((slice_id + 7) * SLICE);
    slice_offset_2 = slice_id * SLICE;
    slice_offset_3 = slice_id * SLICE;

    *(uint64_t*)(output_chunk_8 + slice_offset_2) = *(uint64_t*)(decode_arr_3 + slice_offset_3);
    *(uint64_t*)(decode_arr_1 + slice_offset_1) ^= *(uint64_t*)(decode_arr_3 + slice_offset_3);
    *(uint64_t*)(decode_arr_2 + slice_offset_2) ^= *(uint64_t*)(decode_arr_3 + slice_offset_3);
  }

  for (int slice_id = 0; slice_id < TOTAL_SLICE_NUM; slice_id++) {
    slice_offset_1 = ((slice_id + 6) * SLICE);
    slice_offset_2 = slice_id * SLICE;
    slice_offset_3 = ((slice_id + 1) * SLICE);

    *(uint64_t*)(output_chunk_7 + slice_offset_2) = *(uint64_t*)(decode_arr_2 + slice_offset_2);
    *(uint64_t*)(decode_arr_1 + slice_offset_1) ^= *(uint64_t*)(decode_arr_2 + slice_offset_2);
    *(uint64_t*)(decode_arr_3 + slice_offset_3) ^= *(uint64_t*)(decode_arr_2 + slice_offset_2);
  }

  free(decode_arr_1);
  free(decode_arr_2);
  free(decode_arr_3);

  // Store decoded chunks
  (*decoded)[k].append((char*)output_chunk_6, blocksize);
  (*decoded)[k + 1].append((char*)output_chunk_7, blocksize);
  (*decoded)[k + 2].append((char*)output_chunk_8, blocksize);

  return 0;
}

