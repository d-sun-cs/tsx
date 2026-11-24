// SPDX-License-Identifier: LGPL-2.1-or-later
#include "ceph_ver.h"
#include "ErasureCodeTsxMt.h"
#include "erasure-code/ErasureCodePlugin.h"

namespace {
class ErasureCodePluginTsxMt final : public ceph::ErasureCodePlugin {
  int factory(const std::string&, ceph::ErasureCodeProfile& profile,
              ceph::ErasureCodeInterfaceRef* erasure_code, std::ostream* ss) override
  {
    auto code = std::make_unique<ErasureCodeTsxMt>();
    int r = code->init(profile, ss);
    if (r) {
      return r;
    }
    *erasure_code = ceph::ErasureCodeInterfaceRef(code.release());
    return 0;
  }
};
}

const char* __erasure_code_version() { return CEPH_GIT_NICE_VER; }

int __erasure_code_init(char* plugin_name, char*)
{
  return ceph::ErasureCodePluginRegistry::instance().add(
    plugin_name, new ErasureCodePluginTsxMt());
}
