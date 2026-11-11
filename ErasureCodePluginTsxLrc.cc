// SPDX-License-Identifier: LGPL-2.1-or-later
#include "ceph_ver.h"
#include "ErasureCodePluginTsxLrc.h"
#include "ErasureCodeTsxLrc.h"

int ErasureCodePluginTsxLrc::factory(const std::string& directory,
                                   ceph::ErasureCodeProfile& profile,
                                   ceph::ErasureCodeInterfaceRef* erasure_code,
                                   std::ostream* ss)
{
  auto* code = new ErasureCodeTsxLrc(directory);
  int r = code->init(profile, ss);
  if (r) {
    delete code;
    return r;
  }
  *erasure_code = ceph::ErasureCodeInterfaceRef(code);
  return 0;
}

const char* __erasure_code_version() { return CEPH_GIT_NICE_VER; }

int __erasure_code_init(char* plugin_name, char* directory)
{
  return ceph::ErasureCodePluginRegistry::instance().add(
    plugin_name, new ErasureCodePluginTsxLrc());
}
