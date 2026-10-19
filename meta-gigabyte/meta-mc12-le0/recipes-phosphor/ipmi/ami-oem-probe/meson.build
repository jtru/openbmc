project(
  'ami-oem-probe',
  'cpp',
  version: '1.0',
  # libipmid's headers are C++23 (sdbusplus/asio/connection.hpp uses
  # std::move_only_function), so this has to match phosphor-ipmi-host.
  default_options: [
    'cpp_std=c++23',
    'warning_level=3',
  ],
)

# Registering an OEM IANA handler needs libipmid; lg2 comes from
# phosphor-logging and is header-only. sdbusplus is needed for the D-Bus call
# that tells smbios-mdr to re-read the table this provider publishes.
ipmid_dep = dependency('libipmid')
phosphor_logging_dep = dependency('phosphor-logging')
sdbusplus_dep = dependency('sdbusplus')

shared_module(
  'ami-oem-probe',
  'main.cpp',
  implicit_include_directories: false,
  dependencies: [
    ipmid_dep,
    phosphor_logging_dep,
    sdbusplus_dep,
  ],
  install: true,
  install_dir: get_option('libdir') / 'ipmid-providers',
)
