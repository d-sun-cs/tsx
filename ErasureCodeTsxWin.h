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

#ifndef CEPH_ERASURE_CODE_TSXWIN_H
#define CEPH_ERASURE_CODE_TSXWIN_H

#include "include/err.h"
#include "erasure-code/ErasureCode.h"

#include <hiredis/hiredis.h>
#include <hiredis/async.h>

class ErasureCodeTsxWin final : public ceph::ErasureCode
{
public:
  explicit ErasureCodeTsxWin(const std::string &dir)
      : directory(dir), k(8), m(3) {}

  ~ErasureCodeTsxWin() override
  {
    if (rctx)
    {
      // redisAsyncFree(rctx);
      redisFree(rctx);
      rctx = nullptr;
    }
  }

  int init(ceph::ErasureCodeProfile &profile, std::ostream *ss) override;

  unsigned int get_chunk_count() const override
  {
    return k + m;
  }

  unsigned int get_data_chunk_count() const override
  {
    return k;
  }

  unsigned int get_chunk_size(unsigned int object_size) const override;

  int encode_chunks(const std::set<int> &want_to_encode,
                    std::map<int, ceph::buffer::list> *encoded) override;

  int decode_chunks(const std::set<int> &want_to_read,
                    const std::map<int, ceph::buffer::list> &chunks,
                    std::map<int, ceph::buffer::list> *decoded) override;

private:
  std::string directory;
  // int chunk_count;
  // int data_chunk_count;
  int k;
  int m;
  redisContext *rctx;
  // redisAsyncContext *rctx;
};

#endif
