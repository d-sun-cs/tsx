// SPDX-License-Identifier: LGPL-2.1-or-later
#ifndef CEPH_ERASURE_CODE_TSX_LRC_H
#define CEPH_ERASURE_CODE_TSX_LRC_H

#include "erasure-code/ErasureCode.h"

// Fixed 8+2+2 layout. FastTT supports inline or Redis-backed tails for
// experiments. Scaffold mode keeps the equal-length ISA comparison.
class ErasureCodeTsxLrc final : public ceph::ErasureCode {
public:
  explicit ErasureCodeTsxLrc(const std::string& directory)
    : directory(directory) {}

  int init(ceph::ErasureCodeProfile& profile, std::ostream* ss) override;
  unsigned int get_chunk_count() const override { return 12; }
  unsigned int get_data_chunk_count() const override { return 8; }
  unsigned int get_chunk_size(unsigned int object_size) const override;

  int _minimum_to_decode(const std::set<int>& want,
                        const std::set<int>& available,
                        std::set<int>* minimum) override;
  int encode_chunks(const std::set<int>& want,
                    std::map<int, ceph::bufferlist>* encoded) override;
  int decode_chunks(const std::set<int>& want,
                    const std::map<int, ceph::bufferlist>& chunks,
                    std::map<int, ceph::bufferlist>* decoded) override;

private:
  // Same roles as ErasureCodeLrc::Layer: physical IDs are remapped to
  // contiguous [data..., coding...] IDs before calling each child plugin.
  struct Layer {
    std::vector<int> data;
    std::vector<int> coding;
    std::vector<int> chunks;
    ceph::ErasureCodeInterfaceRef erasure_code;
  };
  struct RepairStep {
    unsigned int layer;
    std::set<int> helpers;
    std::set<int> targets;
    std::vector<int> equations;
  };
  struct RepairPlan {
    std::set<int> inputs;
    std::vector<RepairStep> steps;
  };

  std::string directory;
  std::vector<Layer> layers;
  bool fasttt = false;
  int redis_port = 0;
  int layers_init(std::ostream* ss);
  int make_repair_plan(const std::set<int>& want,
                       const std::set<int>& available,
                       RepairPlan* plan) const;
  bool valid_ids(const std::set<int>& ids) const;
};
#endif
