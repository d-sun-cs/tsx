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

#include "ErasureCodeCerasure.h"

#define dout_context g_ceph_context
#define dout_subsys ceph_subsys_osd
#undef dout_prefix
#define dout_prefix _prefix(_dout)

#define Kval 8 // to be standardized
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
  return *_dout << "ErasureCodeCerasure: ";
}

int ErasureCodeCerasure::init(ErasureCodeProfile &profile, ostream *ss)
{
  int err = 0;
  err |= parse(profile, ss);
  // err |= to_int("k", profile, &k, "8", ss);
  err |= to_int("k", profile, &k, "8", ss);
  err |= to_int("m", profile, &p, "3", ss);
  // chunk_count = k + m;
  if (err)
    return err;

  // rctx = redisConnect("127.0.0.1", 6379);
  // // rctx = redisAsyncConnect("127.0.0.1", 6379);
  // if (rctx == NULL || rctx->err)
  // {
  //   dout(0) << "Error Redis: " << rctx->errstr << dendl;
  //   return 1;
  // }

  return ErasureCode::init(profile, ss);
}

unsigned int ErasureCodeCerasure::get_chunk_size(unsigned int object_size) const
{
  // return (object_size + k - 1) / k;
  return (object_size + k - 1) / k;
}



#define MMAX 255

int ErasureCodeCerasure::encode_chunks(const set<int> &want_to_encode,
                                  map<int, bufferlist> *encoded)
{

  // int kval = k;
  uint8_t *chunks[k + p];
  for (int i = 0; i < k + p; ++i)
  {
    chunks[i] = (uint8_t *)(*encoded)[i].c_str();
  }

  int w = 8;
  int alignlen = (*encoded)[0].length();
  int packetsize = alignlen/w;
  int i, j, m, a;
  int alignlen;
  // Fragment buffer pointers
  uint8_t *frag_ptrs[MMAX];
  uint8_t *copy_ptrs[MMAX];
  uint8_t *tmp[MMAX];
  unsigned int *encode_matrix;

  int *encode_bitmatrix;
  int *slp;
  int **schedule;
  m = k + p;

  int alldata = alignlen * (k);
  int decodedata = alignlen * (k);
  for (i = 0; i < m; i++)
  {
    frag_ptrs[i] = (uint8_t *)(*encoded)[i].c_str();
  }

  encode_matrix = gen_best_matrix_from_any_element_lowtime(k, p, w);
  schedule = bitmatrix_to_smart_schedule(k, p, w, encode_bitmatrix);
  slp_encode_best_finally(k, p, w, schedule, frag_ptrs, &frag_ptrs[k], alignlen, packetsize);
  return 0;
}

int ErasureCodeCerasure::decode_chunks(const set<int> &want_to_read,
                                  const map<int, bufferlist> &chunks,
                                  map<int, bufferlist> *decoded)
{

  int w = 8;
  int m = k + p;
  int alignlen = (*encoded)[0].length();
  int packetsize = alignlen / w;
  uint8_t **ptrs;
  int *decode_bitmatrix;
  int **de_operations;
  int *eraseds;
  int *survives;
  int *recover;
  int *de_recover;
  int erro_num = 0;

  eraseds = (int *)malloc(sizeof(int) * (m));
  survives = (int *)malloc(sizeof(int) * (m));
  recover = (int *)malloc(sizeof(int) * (m));
  de_recover = (int *)malloc(sizeof(int) * (m));
  int erro_num = 0;
  int survive_num = 0;

  for (int i = 0; i < kval + Mval; i++)
  {
    if (chunks.find(i) == chunks.end())
    {
      eraseds[erro_num] = i;
      erro_num++;
    }
    else
    {
      survives[survive_num] = i;
      de_recover[survive_num] = i;
      survive_num++;
    }
  }
  eraseds[erro_num] = -1;

  decode_bitmatrix = generate_decoding_bitmatrix(k, p, w, encode_bitmatrix, eraseds, de_recover);
  ptrs = set_up_ptrs_for_opertaions(k, erro_num, eraseds, frag_ptrs, &frag_ptrs[k], de_recover);
  slp_encode_best_finally(k, erro_num, w, schedule, ptrs, &ptrs[k], alignlen, packetsize);

  return 0;

}
