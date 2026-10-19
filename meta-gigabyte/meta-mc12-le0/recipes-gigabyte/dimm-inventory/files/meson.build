project(
  'dimm-inventory',
  'cpp',
  default_options: [
    # sdbusplus/asio/connection.hpp uses std::move_only_function, so C++23 is
    # required here (entity-manager in this tree is c++23 as well).
    'cpp_std=c++23',
    'warning_level=3',
  ],
  version: '1.0',
)

sdbusplus_dep = dependency('sdbusplus')
boost_dep = dependency('boost')
systemd_dep = dependency('systemd')

# Same form entity-manager uses in this tree; the older pkgconfig
# 'systemdsystemunitdir' variable is not available on this systemd.
systemd_system_unit_dir = systemd_dep.get_variable(
  'systemd_system_unit_dir',
  pkgconfig_define: ['prefix', get_option('prefix')],
)

executable(
  'dimm-inventory',
  sources: [
    'main.cpp',
    'smbios.cpp',
    'spd.cpp',
  ],
  dependencies: [
    sdbusplus_dep,
    boost_dep,
  ],
  install: true,
)

install_data(
  'dimm-inventory.service',
  install_dir: systemd_system_unit_dir,
)
