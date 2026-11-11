// SPDX-License-Identifier: LGPL-2.1-or-later
#ifndef CEPH_ERASURE_CODE_PLUGIN_TSX_LRC_H
#define CEPH_ERASURE_CODE_PLUGIN_TSX_LRC_H
#include "erasure-code/ErasureCodePlugin.h"

class ErasureCodePluginTsxLrc : public ceph::ErasureCodePlugin {
public:
  int factory(const std::string& directory, ceph::ErasureCodeProfile& profile,
              ceph::ErasureCodeInterfaceRef* erasure_code,
              std::ostream* ss) override;
};
#endif
