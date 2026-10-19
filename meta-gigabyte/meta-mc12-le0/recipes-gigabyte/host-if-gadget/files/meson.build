project(
  'host-if-gadget',
  'c',
  version: '1.0',
  default_options: [
    'warning_level=3',
  ],
)

# Same form entity-manager uses in this tree; the older pkgconf
# 'systemdsystemunitdir' variable is not available on this systemd.
systemd_dep = dependency('systemd')
systemd_system_unit_dir = systemd_dep.get_variable(
  'systemd_system_unit_dir',
  pkgconfig_define: ['prefix', get_option('prefix')],
)

executable(
  'host-if-listen',
  'host-if-listen.c',
  install: true,
)

# The gadget itself is created through configfs by a shell script.
install_data(
  'gadget-setup.sh',
  install_dir: get_option('bindir'),
  rename: 'host-if-gadget-setup',
  install_mode: 'rwxr-xr-x',
)

install_data(
  'host-if-gadget.service',
  'host-if-listen.service',
  install_dir: systemd_system_unit_dir,
)
