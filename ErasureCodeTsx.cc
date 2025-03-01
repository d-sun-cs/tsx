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
  // 根据注释，计算chunk大小
  return (object_size + data_chunk_count - 1) / data_chunk_count;
}

int ErasureCodeTsx::encode_chunks(const set<int> &want_to_encode,
				  map<int, bufferlist> *encoded)
{
  // 调试输出
  dout(10) << "encode_chunks: want_to_encode size = " << want_to_encode.size() << dendl;

  // 简单实现，不修改encoded
  return 0;
}

int ErasureCodeTsx::decode_chunks(const set<int> &want_to_read,
				  const map<int, bufferlist> &chunks,
				  map<int, bufferlist> *decoded)
{
  // 调试输出
  dout(10) << "decode_chunks: want_to_read size = " << want_to_read.size() << dendl;

  // 简单实现，直接将chunks中的数据复制到decoded中
  for (const auto& pair : chunks) {
    dout(20) << "decode_chunks: copying chunk " << pair.first << dendl;
    (*decoded)[pair.first] = pair.second;
  }
  return 0;
}

