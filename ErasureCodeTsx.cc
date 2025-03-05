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
#include <unordered_set> // 需要包含哈希表头文件

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

static ostream &_prefix(std::ostream *_dout)
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
  uint8_t *chunks[chunk_count];
  for (unsigned int i = 0; i < chunk_count; ++i)
  {
    chunks[i] = (uint8_t *)(*encoded)[i].c_str();
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

  for (int slice_id = 0; slice_id < TOTAL_SLICE_NUM; slice_id++)
  {
    slice_offset_2 = slice_id * SLICE;
    slice_offset_1 = slice_id * SLICE;
    slice_offset_3 = ((slice_id + 7) * SLICE);
    *(uint64_t *)(chunks[k] + slice_offset_1) = *(uint64_t *)(chunks[0] + slice_offset_2);
    *(uint64_t *)(chunks[k + 1] + slice_offset_2) = *(uint64_t *)(chunks[0] + slice_offset_2);
    *(uint64_t *)(chunks[k + 2] + slice_offset_3) = *(uint64_t *)(chunks[0] + slice_offset_2);
  }

  for (int slice_id = 0; slice_id < TOTAL_SLICE_NUM; slice_id++)
  {
    slice_offset_2 = slice_id * SLICE;
    slice_offset_1 = ((slice_id + 1) * SLICE);
    slice_offset_3 = ((slice_id + 6) * SLICE);
    *(uint64_t *)(chunks[k] + slice_offset_1) ^= *(uint64_t *)(chunks[1] + slice_offset_2);
    *(uint64_t *)(chunks[k + 1] + slice_offset_2) ^= *(uint64_t *)(chunks[1] + slice_offset_2);
    *(uint64_t *)(chunks[k + 2] + slice_offset_3) ^= *(uint64_t *)(chunks[1] + slice_offset_2);
  }

  for (int slice_id = 0; slice_id < TOTAL_SLICE_NUM; slice_id++)
  {
    slice_offset_2 = slice_id * SLICE;
    slice_offset_1 = ((slice_id + 2) * SLICE);
    slice_offset_3 = ((slice_id + 5) * SLICE);
    *(uint64_t *)(chunks[k] + slice_offset_1) ^= *(uint64_t *)(chunks[2] + slice_offset_2);
    *(uint64_t *)(chunks[k + 1] + slice_offset_2) ^= *(uint64_t *)(chunks[2] + slice_offset_2);
    *(uint64_t *)(chunks[k + 2] + slice_offset_3) ^= *(uint64_t *)(chunks[2] + slice_offset_2);
  }

  for (int slice_id = 0; slice_id < TOTAL_SLICE_NUM; slice_id++)
  {
    slice_offset_2 = slice_id * SLICE;
    slice_offset_1 = ((slice_id + 3) * SLICE);
    slice_offset_3 = ((slice_id + 4) * SLICE);
    *(uint64_t *)(chunks[k] + slice_offset_1) ^= *(uint64_t *)(chunks[3] + slice_offset_2);
    *(uint64_t *)(chunks[k + 1] + slice_offset_2) ^= *(uint64_t *)(chunks[3] + slice_offset_2);
    *(uint64_t *)(chunks[k + 2] + slice_offset_3) ^= *(uint64_t *)(chunks[3] + slice_offset_2);
  }

  for (int slice_id = 0; slice_id < TOTAL_SLICE_NUM; slice_id++)
  {
    slice_offset_2 = slice_id * SLICE;
    slice_offset_1 = ((slice_id + 4) * SLICE);
    slice_offset_3 = ((slice_id + 3) * SLICE);
    *(uint64_t *)(chunks[k] + slice_offset_1) ^= *(uint64_t *)(chunks[4] + slice_offset_2);
    *(uint64_t *)(chunks[k + 1] + slice_offset_2) ^= *(uint64_t *)(chunks[4] + slice_offset_2);
    *(uint64_t *)(chunks[k + 2] + slice_offset_3) ^= *(uint64_t *)(chunks[4] + slice_offset_2);
  }

  for (int slice_id = 0; slice_id < TOTAL_SLICE_NUM; slice_id++)
  {
    slice_offset_2 = slice_id * SLICE;
    slice_offset_1 = ((slice_id + 5) * SLICE);
    slice_offset_3 = ((slice_id + 2) * SLICE);
    *(uint64_t *)(chunks[k] + slice_offset_1) ^= *(uint64_t *)(chunks[5] + slice_offset_2);
    *(uint64_t *)(chunks[k + 1] + slice_offset_2) ^= *(uint64_t *)(chunks[5] + slice_offset_2);
    *(uint64_t *)(chunks[k + 2] + slice_offset_3) ^= *(uint64_t *)(chunks[5] + slice_offset_2);
  }

  for (int slice_id = 0; slice_id < TOTAL_SLICE_NUM; slice_id++)
  {
    slice_offset_2 = slice_id * SLICE;
    slice_offset_1 = ((slice_id + 6) * SLICE);
    slice_offset_3 = ((slice_id + 1) * SLICE);
    *(uint64_t *)(chunks[k] + slice_offset_1) ^= *(uint64_t *)(chunks[6] + slice_offset_2);
    *(uint64_t *)(chunks[k + 1] + slice_offset_2) ^= *(uint64_t *)(chunks[6] + slice_offset_2);
    *(uint64_t *)(chunks[k + 2] + slice_offset_3) ^= *(uint64_t *)(chunks[6] + slice_offset_2);
  }

  for (int slice_id = 0; slice_id < TOTAL_SLICE_NUM; slice_id++)
  {
    slice_offset_2 = slice_id * SLICE;
    slice_offset_1 = ((slice_id + 7) * SLICE);
    slice_offset_3 = slice_id * SLICE;
    *(uint64_t *)(chunks[k] + slice_offset_1) ^= *(uint64_t *)(chunks[7] + slice_offset_2);
    *(uint64_t *)(chunks[k + 1] + slice_offset_2) ^= *(uint64_t *)(chunks[7] + slice_offset_2);
    *(uint64_t *)(chunks[k + 2] + slice_offset_3) ^= *(uint64_t *)(chunks[7] + slice_offset_2);
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
  // 初始化哈希
  std::unordered_set<unsigned int> available_chunks; // 新增哈希集合
  int havecodings = 0;
  uint8_t *data[data_chunk_count];
  uint8_t *coding[chunk_count - data_chunk_count];
  for (unsigned int i = 0; i < chunk_count; i++)
  {
    if (chunks.find(i) == chunks.end())
    {
      erasures[erasures_count] = i;
      erasures_count++;
    }
    else
    {
      // 哈希中插入key=i
      available_chunks.insert(i); // 将存在的块插入哈希集合
      if (i >= data_chunk_count)
      {
        havecodings++;
      }
    }
    if (i < data_chunk_count)
      data[i] = (uint8_t *)(*decoded)[i].c_str();
    else
      coding[i - data_chunk_count] = (uint8_t *)(*decoded)[i].c_str();
  }
  erasures[erasures_count] = -1;

  ceph_assert(erasures_count > 0);

  // Calculate k and m
  const int k = data_chunk_count;
  const int m = chunk_count - data_chunk_count;

  // Define constants
  const int SLICE = 8;
  const int TOTAL_SLICE_NUM = (blocksize / SLICE);
  const int PARITY_SIZE = blocksize + (SLICE * 20);

  // Shift XOR decode algorithm
  uint64_t slice_offset_1;
  uint64_t slice_offset_2;
  uint64_t slice_offset_3;

  uint8_t *decode_arr_1 = (uint8_t *)malloc(PARITY_SIZE * sizeof(uint8_t));
  uint8_t *decode_arr_2 = (uint8_t *)malloc(PARITY_SIZE * sizeof(uint8_t));
  uint8_t *decode_arr_3 = (uint8_t *)malloc(PARITY_SIZE * sizeof(uint8_t));
  memcpy(decode_arr_1, coding[0], PARITY_SIZE);
  memcpy(decode_arr_2, coding[1], PARITY_SIZE);
  memcpy(decode_arr_3, coding[2], PARITY_SIZE);

  uint8_t output_chunk_6[blocksize];
  uint8_t output_chunk_7[blocksize];
  uint8_t output_chunk_8[blocksize];

  if (available_chunks.find(0) != available_chunks.end())
  {
    for (int slice_id = 0; slice_id < TOTAL_SLICE_NUM; slice_id++)
    {
      slice_offset_2 = slice_id * SLICE;
      slice_offset_1 = slice_id * SLICE;
      slice_offset_3 = (slice_id + 7) * SLICE;

      if (available_chunks.find(k) != available_chunks.end())
      {
        *(uint64_t *)(decode_arr_1 + slice_offset_1) ^= *(uint64_t *)(data[0] + slice_offset_2);
      }
      if (available_chunks.find(k + 1) != available_chunks.end())
      {
        *(uint64_t *)(decode_arr_2 + slice_offset_2) ^= *(uint64_t *)(data[0] + slice_offset_2);
      }
      if (available_chunks.find(k + 2) != available_chunks.end())
      {
        *(uint64_t *)(decode_arr_3 + slice_offset_3) ^= *(uint64_t *)(data[0] + slice_offset_2);
      }
    }
  }

  if (available_chunks.find(1) != available_chunks.end())
  {
    for (int slice_id = 0; slice_id < TOTAL_SLICE_NUM; slice_id++)
    {
      slice_offset_2 = slice_id * SLICE;
      slice_offset_1 = (slice_id + 1) * SLICE;
      slice_offset_3 = (slice_id + 6) * SLICE;

      if (available_chunks.find(k) != available_chunks.end())
      {
        *(uint64_t *)(decode_arr_1 + slice_offset_1) ^= *(uint64_t *)(data[1] + slice_offset_2);
      }
      if (available_chunks.find(k + 1) != available_chunks.end())
      {
        *(uint64_t *)(decode_arr_2 + slice_offset_2) ^= *(uint64_t *)(data[1] + slice_offset_2);
      }
      if (available_chunks.find(k + 2) != available_chunks.end())
      {
        *(uint64_t *)(decode_arr_3 + slice_offset_3) ^= *(uint64_t *)(data[1] + slice_offset_2);
      }
    }
  }

  if (available_chunks.find(2) != available_chunks.end())
  {
    for (int slice_id = 0; slice_id < TOTAL_SLICE_NUM; slice_id++)
    {
      slice_offset_2 = slice_id * SLICE;
      slice_offset_1 = (slice_id + 2) * SLICE;
      slice_offset_3 = (slice_id + 5) * SLICE;

      if (available_chunks.find(k) != available_chunks.end())
      {
        *(uint64_t *)(decode_arr_1 + slice_offset_1) ^= *(uint64_t *)(data[2] + slice_offset_2);
      }
      if (available_chunks.find(k + 1) != available_chunks.end())
      {
        *(uint64_t *)(decode_arr_2 + slice_offset_2) ^= *(uint64_t *)(data[2] + slice_offset_2);
      }
      if (available_chunks.find(k + 2) != available_chunks.end())
      {
        *(uint64_t *)(decode_arr_3 + slice_offset_3) ^= *(uint64_t *)(data[2] + slice_offset_2);
      }
    }
  }

  if (available_chunks.find(3) != available_chunks.end())
  {
    for (int slice_id = 0; slice_id < TOTAL_SLICE_NUM; slice_id++)
    {
      slice_offset_2 = slice_id * SLICE;
      slice_offset_1 = (slice_id + 3) * SLICE;
      slice_offset_3 = (slice_id + 4) * SLICE;

      if (available_chunks.find(k) != available_chunks.end())
      {
        *(uint64_t *)(decode_arr_1 + slice_offset_1) ^= *(uint64_t *)(data[3] + slice_offset_2);
      }
      if (available_chunks.find(k + 1) != available_chunks.end())
      {
        *(uint64_t *)(decode_arr_2 + slice_offset_2) ^= *(uint64_t *)(data[3] + slice_offset_2);
      }
      if (available_chunks.find(k + 2) != available_chunks.end())
      {
        *(uint64_t *)(decode_arr_3 + slice_offset_3) ^= *(uint64_t *)(data[3] + slice_offset_2);
      }
    }
  }

  if (available_chunks.find(4) != available_chunks.end())
  {
    for (int slice_id = 0; slice_id < TOTAL_SLICE_NUM; slice_id++)
    {
      slice_offset_2 = slice_id * SLICE;
      slice_offset_1 = (slice_id + 4) * SLICE;
      slice_offset_3 = (slice_id + 3) * SLICE;

      if (available_chunks.find(k) != available_chunks.end())
      {
        *(uint64_t *)(decode_arr_1 + slice_offset_1) ^= *(uint64_t *)(data[4] + slice_offset_2);
      }
      if (available_chunks.find(k + 1) != available_chunks.end())
      {
        *(uint64_t *)(decode_arr_2 + slice_offset_2) ^= *(uint64_t *)(data[4] + slice_offset_2);
      }
      if (available_chunks.find(k + 2) != available_chunks.end())
      {
        *(uint64_t *)(decode_arr_3 + slice_offset_3) ^= *(uint64_t *)(data[4] + slice_offset_2);
      }
    }
  }

  if (available_chunks.find(5) != available_chunks.end())
  {
    for (int slice_id = 0; slice_id < TOTAL_SLICE_NUM; slice_id++)
    {
      slice_offset_2 = slice_id * SLICE;
      slice_offset_1 = (slice_id + 5) * SLICE;
      slice_offset_3 = (slice_id + 2) * SLICE;

      if (available_chunks.find(k) != available_chunks.end())
      {
        *(uint64_t *)(decode_arr_1 + slice_offset_1) ^= *(uint64_t *)(data[5] + slice_offset_2);
      }
      if (available_chunks.find(k + 1) != available_chunks.end())
      {
        *(uint64_t *)(decode_arr_2 + slice_offset_2) ^= *(uint64_t *)(data[5] + slice_offset_2);
      }
      if (available_chunks.find(k + 2) != available_chunks.end())
      {
        *(uint64_t *)(decode_arr_3 + slice_offset_3) ^= *(uint64_t *)(data[5] + slice_offset_2);
      }
    }
  }

  if (available_chunks.find(6) != available_chunks.end())
  {
    for (int slice_id = 0; slice_id < TOTAL_SLICE_NUM; slice_id++)
    {
      slice_offset_2 = slice_id * SLICE;
      slice_offset_1 = (slice_id + 6) * SLICE;
      slice_offset_3 = (slice_id + 1) * SLICE;

      if (available_chunks.find(k) != available_chunks.end())
      {
        *(uint64_t *)(decode_arr_1 + slice_offset_1) ^= *(uint64_t *)(data[6] + slice_offset_2);
      }
      if (available_chunks.find(k + 1) != available_chunks.end())
      {
        *(uint64_t *)(decode_arr_2 + slice_offset_2) ^= *(uint64_t *)(data[6] + slice_offset_2);
      }
      if (available_chunks.find(k + 2) != available_chunks.end())
      {
        *(uint64_t *)(decode_arr_3 + slice_offset_3) ^= *(uint64_t *)(data[6] + slice_offset_2);
      }
    }
  }

  if (available_chunks.find(7) != available_chunks.end())
  {
    for (int slice_id = 0; slice_id < TOTAL_SLICE_NUM; slice_id++)
    {
      slice_offset_2 = slice_id * SLICE;
      slice_offset_1 = (slice_id + 7) * SLICE;
      slice_offset_3 = (slice_id * SLICE);

      if (available_chunks.find(k) != available_chunks.end())
      {
        *(uint64_t *)(decode_arr_1 + slice_offset_1) ^= *(uint64_t *)(data[7] + slice_offset_2);
      }
      if (available_chunks.find(k + 1) != available_chunks.end())
      {
        *(uint64_t *)(decode_arr_2 + slice_offset_2) ^= *(uint64_t *)(data[7] + slice_offset_2);
      }
      if (available_chunks.find(k + 2) != available_chunks.end())
      {
        *(uint64_t *)(decode_arr_3 + slice_offset_3) ^= *(uint64_t *)(data[7] + slice_offset_2);
      }
    }
  }

  // paritys_seq: 前半部分是从左往右数，后半部分是从右往左数,记录该校验块在存活校验块中是第几块
  // paritys_seq: 因为是交错解码，校验块顺序从两边往中间数
  int paritys_seq[m];
  for (unsigned int i = 0; i < erasures_count; i++)
  {
    paritys_seq[i] = m + 1; // 该校验块不在后面decode中用到
  }
  for (unsigned int i = 0; i < erasures_count; i++)
  {
    // 丢了几个数据块，我们就用几个校验块
    if (i % 2 == 0)
    {
      // 寻找从左往右数的第i/2个未丢失校验块
      int count = 0;
      for (unsigned int j = 0; j < m; j++)
      {
        if (available_chunks.find(k + j) != available_chunks.end())
        {
          if (count == i / 2)
          {
            paritys_seq[j] = i / 2;
            break;
          }
          count++;
        }
      }
    }
    else
    {
      // 寻找从右往左数的第i/2个未丢失校验块
      int count = 0;
      for (unsigned int j = m - 1; j >= 0; j--)
      {
        if (available_chunks.find(k + j) != available_chunks.end())
        {
          if (count == i / 2)
          {
            paritys_seq[j] = i / 2;
            break;
          }
          count++;
        }
      }
    }
  }

  // 这里专门处理只丢一块数据块的情况
  if (erasures_count == 1)
  {
    for (unsigned int i = 0; i < m; i++)
    {
      if (paritys_seq[i] <= erasures_count)
      {
        if (i == 0)
        {
          (*decoded)[erasures[0]].append((char *)decode_arr_1, blocksize);
        }
        else if (i == 1)
        {
          (*decoded)[erasures[0]].append((char *)decode_arr_2, blocksize);
        }
        else if (i == 2)
        {
          (*decoded)[erasures[0]].append((char *)decode_arr_3, blocksize);
        }
        return 0;
      }
    }
  }

  // solve chunk 6,7,8
  // 拥有的校验块个数肯定大于等于丢失的数据块个数
  // 取相同数量的校验块来解码丢失的数据块
  for (int slice_id = 0; slice_id < TOTAL_SLICE_NUM; slice_id++)
  {
    // 交错解码，左边一个，右边一个
    if (paritys_seq[0] <= erasures_count)
    {
      // Step 1: solve 64 bits in packet 6
      slice_offset_1 = ((slice_id + erasures[paritys_seq[0]]) * SLICE);
      slice_offset_2 = slice_id * SLICE;
      slice_offset_3 = ((slice_id) + k - 1 - erasures[paritys_seq[0]]) * SLICE;

      // directly use leftmost block of parity packet 1 as message of packet 6
      *(uint64_t *)(output_chunk_6 + slice_offset_2) = *(uint64_t *)(decode_arr_1 + slice_offset_1);

      // back substitute packet 6 to parity packet 2 and 3
      if (paritys_seq[1] <= erasures_count)
      {
        *(uint64_t *)(decode_arr_2 + slice_offset_2) ^= *(uint64_t *)(decode_arr_1 + slice_offset_1);
      }
      if (paritys_seq[2] <= erasures_count)
      {
        *(uint64_t *)(decode_arr_3 + slice_offset_3) ^= *(uint64_t *)(decode_arr_1 + slice_offset_1);
      }
    }

    if (paritys_seq[2] <= erasures_count)
    {
      // Step 2: solve 64 bits in packet 8
      slice_offset_1 = ((slice_id + erasures[erasures_count - 1 - paritys_seq[2]]) * SLICE);
      slice_offset_2 = slice_id * SLICE;
      slice_offset_3 = ((slice_id) + k - 1 - erasures[erasures_count - 1 - paritys_seq[2]]) * SLICE;

      // directly use leftmost block of parity packet 3 as message of packet 8
      *(uint64_t *)(output_chunk_8 + slice_offset_2) = *(uint64_t *)(decode_arr_3 + slice_offset_3);

      // back substitute packet 8 to parity 1 and 2
      if (paritys_seq[0] <= erasures_count)
      {
        *(uint64_t *)(decode_arr_1 + slice_offset_1) ^= *(uint64_t *)(decode_arr_3 + slice_offset_3);
      }
      if (paritys_seq[1] <= erasures_count)
      {
        *(uint64_t *)(decode_arr_2 + slice_offset_2) ^= *(uint64_t *)(decode_arr_3 + slice_offset_3);
      }
    }

    if (paritys_seq[1] <= erasures_count)
    {
      // Step 3: solve 64 bits in packet 7
      slice_offset_1 = ((slice_id + erasures[paritys_seq[1]]) * SLICE);
      slice_offset_2 = slice_id * SLICE;
      slice_offset_3 = ((slice_id) + k - 1 - erasures[paritys_seq[1]]) * SLICE;

      // directly use leftmost block of parity packet 2 as message of packet 7
      *(uint64_t *)(output_chunk_7 + slice_offset_2) = *(uint64_t *)(decode_arr_2 + slice_offset_2);

      // back substitute packet 7 to parity packet 1 and 3
      if (paritys_seq[0] <= erasures_count)
      {
        *(uint64_t *)(decode_arr_1 + slice_offset_1) ^= *(uint64_t *)(decode_arr_2 + slice_offset_2);
      }
      if (paritys_seq[2] <= erasures_count)
      {
        *(uint64_t *)(decode_arr_3 + slice_offset_3) ^= *(uint64_t *)(decode_arr_2 + slice_offset_2);
      }
    }
  }

  free(decode_arr_1);
  free(decode_arr_2);
  free(decode_arr_3);

  // Store decoded chunks

  // (*decoded)[k].append((char*)output_chunk_6, blocksize);
  // (*decoded)[k + 1].append((char*)output_chunk_7, blocksize);
  // (*decoded)[k + 2].append((char*)output_chunk_8, blocksize);

  for (unsigned int i = 0; i < m; i++)
  {
    if (paritys_seq[i] <= erasures_count)
    {
      if (i == 0)
      {
        (*decoded)[erasures[0]].append((char *)output_chunk_6, blocksize);
      }
      else if (i == 1)
      {
        (*decoded)[erasures[1]].append((char *)output_chunk_7, blocksize);
      }
      else if (i == 2)
      {
        (*decoded)[erasures[2]].append((char *)output_chunk_8, blocksize);
      }
    }
  }

  return 0;
}
