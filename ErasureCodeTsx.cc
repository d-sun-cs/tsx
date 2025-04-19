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

static std::unordered_map<int, std::array<uint8_t*, PARITY_CHUNKS>> parity_extra_data;

class ParityChunkPtr {
public:
  ParityChunkPtr(unsigned blocksize, uint8_t* chunk_ptr, uint8_t* extradata = nullptr)
    : blocksize(blocksize), chunk_ptr(chunk_ptr), external_extradata(extradata != nullptr) {
    if (external_extradata) {
      this->extradata = extradata;
    } else {
      this->extradata = (uint8_t*)malloc(EXTRA_DATA_SIZE);
    }
  }

  // ~ParityChunkPtr() {
  //   if (!external_extradata) {
  //     free(extradata);
  //   }
  // }

  uint8_t* operator[](size_t idx) {
    if (idx >= blocksize) {
      return &extradata[idx - blocksize];
    }
    return &chunk_ptr[idx];
  }

  uint8_t* get_extradata() const {
    return extradata;
  }

private:
  unsigned blocksize;
  uint8_t* chunk_ptr;
  uint8_t* extradata;
  bool external_extradata; // True if extradata is externally managed
};

unsigned int ErasureCodeTsx::get_chunk_size(unsigned int object_size) const
{
  // According to the comment, calculate the chunk size
  return (object_size + data_chunk_count - 1) / data_chunk_count;
}

int ErasureCodeTsx::encode_chunks(const set<int> &want_to_encode,
                                  map<int, bufferlist> *encoded) {
  static int encode_call_count = -1;
  encode_call_count++;

  // Prepare input chunks
  uint8_t* chunks[chunk_count];
  for (unsigned int i = 0; i < chunk_count; ++i) {
    chunks[i] = (uint8_t*)(*encoded)[i].c_str();
  }

  // Calculate k and m
  const int k = data_chunk_count;
  unsigned blocksize = (*encoded)[0].length();

  // Define constants
  const int SLICE = 32;
  const int TOTAL_SLICE_NUM = blocksize / SLICE;

  // Use ParityChunkPtr for parity chunks
  ParityChunkPtr parity_k(blocksize, chunks[k]);
  ParityChunkPtr parity_k1(blocksize, chunks[k + 1]);
  ParityChunkPtr parity_k2(blocksize, chunks[k + 2]);

#if ENABLE_ENCODE

  // Shift XOR encode algorithm
  uint64_t slice_offset_1;
  uint64_t slice_offset_2;
  uint64_t slice_offset_3;
  __m256i mxx1, mxx2;

  // First loop: directly store chunks[0] into parity
  for (int slice_id = 0; slice_id < TOTAL_SLICE_NUM; slice_id++) {
    slice_offset_2 = slice_id * SLICE;
    slice_offset_1 = slice_id * SLICE;
    slice_offset_3 = ((slice_id + 7) * SLICE);
    mxx1 = _mm256_loadu_si256((__m256i*)&chunks[0][slice_offset_2]);
    _mm256_storeu_si256((__m256i*)parity_k[slice_offset_1], mxx1);
    _mm256_storeu_si256((__m256i*)parity_k1[slice_offset_2], mxx1);
    _mm256_storeu_si256((__m256i*)parity_k2[slice_offset_3], mxx1);
  }

  // Apply the chunks[1] loop pattern to all subsequent chunks
  for (int chunk_idx = 1; chunk_idx < 8; chunk_idx++) {
    for (int slice_id = 0; slice_id < TOTAL_SLICE_NUM; slice_id++) {
      slice_offset_2 = slice_id * SLICE;
      slice_offset_1 = ((slice_id + chunk_idx) * SLICE);
      slice_offset_3 = ((slice_id + (7 - chunk_idx)) * SLICE);
      mxx1 = _mm256_loadu_si256((__m256i*)&chunks[chunk_idx][slice_offset_2]);
      if (slice_id == TOTAL_SLICE_NUM - 1) {
        _mm256_storeu_si256((__m256i*)parity_k[slice_offset_1], mxx1);
      } else {
        mxx2 = _mm256_xor_si256(mxx1, *((__m256i*)parity_k[slice_offset_1]));
        _mm256_storeu_si256((__m256i*)parity_k[slice_offset_1], mxx2);
      }
      mxx2 = _mm256_xor_si256(mxx1, *((__m256i*)parity_k1[slice_offset_2]));
      _mm256_storeu_si256((__m256i*)parity_k1[slice_offset_2], mxx2);
      if (slice_id == 0) {
        _mm256_storeu_si256((__m256i*)parity_k2[slice_offset_3], mxx1);
      } else {
        mxx2 = _mm256_xor_si256(mxx1, *((__m256i*)parity_k2[slice_offset_3]));
        _mm256_storeu_si256((__m256i*)parity_k2[slice_offset_3], mxx2);
      }
    }
  }

  // Store extradata pointers into the global hash map
  std::array<uint8_t*, PARITY_CHUNKS> extra_data = {
    parity_k.get_extradata(),
    parity_k1.get_extradata(),
    parity_k2.get_extradata()
  };

  parity_extra_data[encode_call_count] = extra_data;

  return 0;
#else
  return 0;
#endif
}

int ErasureCodeTsx::decode_chunks(const set<int> &want_to_read,
                                  const map<int, bufferlist> &chunks,
                                  map<int, bufferlist> *decoded) {
#if ENABLE_DECODE
  static int decode_call_count = -1;
  decode_call_count++;

  // Prepare input chunks
  unsigned blocksize = (*chunks.begin()).second.length();
  uint8_t *data[data_chunk_count];
  uint8_t *coding[chunk_count - data_chunk_count];
  for (unsigned int i = 0; i < chunk_count; i++) {
    if (i < data_chunk_count)
      data[i] = (uint8_t *)(*decoded)[i].c_str();
    else
      coding[i - data_chunk_count] = (uint8_t *)(*decoded)[i].c_str();
  }

  // Retrieve extradata pointers from the global hash map
  auto extra_data = parity_extra_data[decode_call_count];

  // Use ParityChunkPtr for decoding
  ParityChunkPtr parity_k(blocksize, coding[0], extra_data[0]);
  ParityChunkPtr parity_k1(blocksize, coding[1], extra_data[1]);
  ParityChunkPtr parity_k2(blocksize, coding[2], extra_data[2]);

  // Define constants
  const int SLICE = 32;
  const int TOTAL_SLICE_NUM = (blocksize / SLICE);

  // Shift XOR decode algorithm
  uint64_t slice_offset_1;
  uint64_t slice_offset_2;
  uint64_t slice_offset_3;
  __m256i mxx1, mxx2;

  for (int slice_id = 0; slice_id < TOTAL_SLICE_NUM; slice_id++) {
    slice_offset_2 = slice_id * SLICE;
    slice_offset_1 = slice_id * SLICE;
    slice_offset_3 = (slice_id + 7) * SLICE;
    mxx1 = _mm256_loadu_si256((__m256i *)&data[0][slice_offset_2]);
    mxx2 = _mm256_xor_si256(mxx1, *((__m256i *)parity_k[slice_offset_1]));
    _mm256_storeu_si256((__m256i *)parity_k[slice_offset_1], mxx2);
    mxx2 = _mm256_xor_si256(mxx1, *((__m256i *)parity_k1[slice_offset_2]));
    _mm256_storeu_si256((__m256i *)parity_k1[slice_offset_2], mxx2);
    mxx2 = _mm256_xor_si256(mxx1, *((__m256i *)parity_k2[slice_offset_3]));
    _mm256_storeu_si256((__m256i *)parity_k2[slice_offset_3], mxx2);
  }

  for (int slice_id = 0; slice_id < TOTAL_SLICE_NUM; slice_id++) {
    slice_offset_2 = slice_id * SLICE;
    slice_offset_1 = (slice_id + 1) * SLICE;
    slice_offset_3 = (slice_id + 6) * SLICE;
    mxx1 = _mm256_loadu_si256((__m256i *)&data[1][slice_offset_2]);
    mxx2 = _mm256_xor_si256(mxx1, *((__m256i *)parity_k[slice_offset_1]));
    _mm256_storeu_si256((__m256i *)parity_k[slice_offset_1], mxx2);
    mxx2 = _mm256_xor_si256(mxx1, *((__m256i *)parity_k1[slice_offset_2]));
    _mm256_storeu_si256((__m256i *)parity_k1[slice_offset_2], mxx2);
    mxx2 = _mm256_xor_si256(mxx1, *((__m256i *)parity_k2[slice_offset_3]));
    _mm256_storeu_si256((__m256i *)parity_k2[slice_offset_3], mxx2);
  }

  for (int slice_id = 0; slice_id < TOTAL_SLICE_NUM; slice_id++) {
    slice_offset_2 = slice_id * SLICE;
    slice_offset_1 = (slice_id + 2) * SLICE;
    slice_offset_3 = (slice_id + 5) * SLICE;
    mxx1 = _mm256_loadu_si256((__m256i *)&data[2][slice_offset_2]);
    mxx2 = _mm256_xor_si256(mxx1, *((__m256i *)parity_k[slice_offset_1]));
    _mm256_storeu_si256((__m256i *)parity_k[slice_offset_1], mxx2);
    mxx2 = _mm256_xor_si256(mxx1, *((__m256i *)parity_k1[slice_offset_2]));
    _mm256_storeu_si256((__m256i *)parity_k1[slice_offset_2], mxx2);
    mxx2 = _mm256_xor_si256(mxx1, *((__m256i *)parity_k2[slice_offset_3]));
    _mm256_storeu_si256((__m256i *)parity_k2[slice_offset_3], mxx2);
  }

  for (int slice_id = 0; slice_id < TOTAL_SLICE_NUM; slice_id++) {
    slice_offset_2 = slice_id * SLICE;
    slice_offset_1 = (slice_id + 3) * SLICE;
    slice_offset_3 = (slice_id + 4) * SLICE;
    mxx1 = _mm256_loadu_si256((__m256i *)&data[3][slice_offset_2]);
    mxx2 = _mm256_xor_si256(mxx1, *((__m256i *)parity_k[slice_offset_1]));
    _mm256_storeu_si256((__m256i *)parity_k[slice_offset_1], mxx2);
    mxx2 = _mm256_xor_si256(mxx1, *((__m256i *)parity_k1[slice_offset_2]));
    _mm256_storeu_si256((__m256i *)parity_k1[slice_offset_2], mxx2);
    mxx2 = _mm256_xor_si256(mxx1, *((__m256i *)parity_k2[slice_offset_3]));
    _mm256_storeu_si256((__m256i *)parity_k2[slice_offset_3], mxx2);
  }

  for (int slice_id = 0; slice_id < TOTAL_SLICE_NUM; slice_id++) {
    slice_offset_2 = slice_id * SLICE;
    slice_offset_1 = (slice_id + 4) * SLICE;
    slice_offset_3 = (slice_id + 3) * SLICE;
    mxx1 = _mm256_loadu_si256((__m256i *)&data[4][slice_offset_2]);
    mxx2 = _mm256_xor_si256(mxx1, *((__m256i *)parity_k[slice_offset_1]));
    _mm256_storeu_si256((__m256i *)parity_k[slice_offset_1], mxx2);
    mxx2 = _mm256_xor_si256(mxx1, *((__m256i *)parity_k1[slice_offset_2]));
    _mm256_storeu_si256((__m256i *)parity_k1[slice_offset_2], mxx2);
    mxx2 = _mm256_xor_si256(mxx1, *((__m256i *)parity_k2[slice_offset_3]));
    _mm256_storeu_si256((__m256i *)parity_k2[slice_offset_3], mxx2);
  }

  for (int slice_id = 0; slice_id < TOTAL_SLICE_NUM; slice_id++) {
    slice_offset_1 = ((slice_id + 5) * SLICE);
    slice_offset_2 = slice_id * SLICE;
    slice_offset_3 = ((slice_id + 2) * SLICE);

    mxx1 = _mm256_loadu_si256((__m256i *)parity_k[slice_offset_1]);
    _mm256_storeu_si256((__m256i *)&data[5][slice_offset_2], mxx1);

    mxx2 = _mm256_xor_si256(mxx1, *((__m256i *)parity_k1[slice_offset_2]));
    _mm256_storeu_si256((__m256i *)parity_k1[slice_offset_2], mxx2);
    mxx2 = _mm256_xor_si256(mxx1, *((__m256i *)parity_k2[slice_offset_3]));
    _mm256_storeu_si256((__m256i *)parity_k2[slice_offset_3], mxx2);

    slice_offset_1 = ((slice_id + 7) * SLICE);
    slice_offset_2 = slice_id * SLICE;
    slice_offset_3 = slice_id * SLICE;

    mxx1 = _mm256_loadu_si256((__m256i *)parity_k2[slice_offset_3]);
    _mm256_storeu_si256((__m256i *)&data[7][slice_offset_2], mxx1);

    mxx2 = _mm256_xor_si256(mxx1, *((__m256i *)parity_k[slice_offset_1]));
    _mm256_storeu_si256((__m256i *)parity_k[slice_offset_1], mxx2);
    mxx2 = _mm256_xor_si256(mxx1, *((__m256i *)parity_k1[slice_offset_2]));
    _mm256_storeu_si256((__m256i *)parity_k1[slice_offset_2], mxx2);

    slice_offset_1 = ((slice_id + 6) * SLICE);
    slice_offset_2 = slice_id * SLICE;
    slice_offset_3 = ((slice_id + 1) * SLICE);

    mxx1 = _mm256_loadu_si256((__m256i *)parity_k1[slice_offset_2]);
    _mm256_storeu_si256((__m256i *)&data[6][slice_offset_2], mxx1);

    mxx2 = _mm256_xor_si256(mxx1, *((__m256i *)parity_k[slice_offset_1]));
    _mm256_storeu_si256((__m256i *)parity_k[slice_offset_1], mxx2);
    mxx2 = _mm256_xor_si256(mxx1, *((__m256i *)parity_k2[slice_offset_3]));
    _mm256_storeu_si256((__m256i *)parity_k2[slice_offset_3], mxx2);
  }

  return 0;
#else
  return 0;
#endif
}
