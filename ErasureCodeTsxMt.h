// SPDX-License-Identifier: LGPL-2.1-or-later
#ifndef CEPH_ERASURE_CODE_TSX_MT_H
#define CEPH_ERASURE_CODE_TSX_MT_H

#include <semaphore>

#include "erasure-code/ErasureCode.h"

class ErasureCodeTsxMt final : public ceph::ErasureCode {
public:
  int init(ceph::ErasureCodeProfile& profile, std::ostream* ss) override;
  unsigned int get_chunk_count() const override { return 11; }
  unsigned int get_data_chunk_count() const override { return 8; }
  unsigned int get_chunk_size(unsigned int object_size) const override;

  int _minimum_to_decode(const std::set<int>& want, const std::set<int>& available,
                         std::set<int>* minimum) override;
  int encode_chunks(const std::set<int>& want,
                    std::map<int, ceph::bufferlist>* encoded) override;
  int decode_chunks(const std::set<int>& want,
                    const std::map<int, ceph::bufferlist>& chunks,
                    std::map<int, ceph::bufferlist>* decoded) override;

private:
  static constexpr int max_threads = 64;
  int threads = 4;
  int redis_port = 6379;
  std::counting_semaphore<max_threads> slots{0};

  template<typename Work>
  void for_output_ranges(size_t symbols, Work&& work);
  int write_parities(size_t symbols, const std::set<int>& want,
                     std::map<int, ceph::bufferlist>& chunks);
};

#endif
