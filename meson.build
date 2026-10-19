project('ddc-controls', 'cpp',
  version : '0.1.0',
  license : 'GPL-3.0-or-later',
  license_files : ['LICENSE'],
  meson_version : '>= 1.1.0',
  default_options : ['cpp_std=c++17', 'warning_level=2', 'buildtype=release'])

cxx = meson.get_compiler('cpp')

libddcci = dependency('libddcci', version : '>= 0.4.0')
if get_option('static_libddcci')
  # -lddcci would pick libddcci.so when both exist; name the archive.
  libddcci = declare_dependency(
    dependencies : cxx.find_library('ddcci', static : true,
      dirs : [libddcci.get_variable(pkgconfig : 'libdir')]),
    include_directories : include_directories(
      libddcci.get_variable(pkgconfig : 'includedir'), is_system : true))
endif

deps = [
  dependency('gtkmm-3.0'),
  dependency('giomm-2.4'),
  libddcci,
  dependency('threads'),
]

executable('ddc-controls',
  'src/main.cpp',
  'src/util.cpp',
  'src/device.cpp',
  'src/config.cpp',
  'src/worker.cpp',
  'src/session.cpp',
  'src/commands.cpp',
  'src/waybar.cpp',
  'src/gui.cpp',
  dependencies : deps,
  cpp_args : ['-DDDC_CONTROLS_VERSION="' + meson.project_version() + '"'],
  install : true)

install_data('data/ddc-controls.desktop',
  install_dir : get_option('datadir') / 'applications')
install_data('data/waybar-module.jsonc', 'data/waybar-style.css',
  install_dir : get_option('datadir') / 'ddc-controls')
install_data('LICENSE',
  install_dir : get_option('datadir') / 'licenses' / 'ddc-controls')
install_data('README.md',
  install_dir : get_option('datadir') / 'doc' / 'ddc-controls')
