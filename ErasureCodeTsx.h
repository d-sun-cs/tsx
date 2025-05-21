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

#ifndef CEPH_ERASURE_CODE_TSX_H
#define CEPH_ERASURE_CODE_TSX_H

#include "include/err.h"
#include "erasure-code/ErasureCode.h"

#include <hiredis/hiredis.h>

class ErasureCodeTsx final : public ceph::ErasureCode {
public:
  explicit ErasureCodeTsx(const std::string &dir)
    : directory(dir), chunk_count(11), data_chunk_count(8) {}

  ~ErasureCodeTsx() override {
    if (rctx) {
      redisFree(rctx);
      rctx = nullptr;
    }
  }

  int init(ceph::ErasureCodeProfile &profile, std::ostream *ss) override;

  unsigned int get_chunk_count() const override {
    return chunk_count;
  }

  unsigned int get_data_chunk_count() const override {
    return data_chunk_count;
  }

  unsigned int get_chunk_size(unsigned int object_size) const override;

  int encode_chunks(const std::set<int> &want_to_encode,
		    std::map<int, ceph::buffer::list> *encoded) override;

  int decode_chunks(const std::set<int> &want_to_read,
		    const std::map<int, ceph::buffer::list> &chunks,
		    std::map<int, ceph::buffer::list> *decoded) override;


private:

  std::string directory;
  unsigned int chunk_count;
  unsigned int data_chunk_count;
  redisContext *rctx;
};

#endif
