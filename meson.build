project('gst-plugin-dav1ddec', 'c',
  version : '1.0.0',
  license : 'LGPL-2.1-or-later',
  meson_version : '>= 0.53',
  default_options : [
    'warning_level=1',
    'buildtype=debugoptimized',
    'c_std=gnu11',
  ])

gst_req = '>= 1.18.0'
dav1d_req = '>= 1.0.0'

gst_dep = dependency('gstreamer-1.0', version : gst_req)
gstbase_dep = dependency('gstreamer-base-1.0', version : gst_req)
gstvideo_dep = dependency('gstreamer-video-1.0', version : gst_req)
dav1d_dep = dependency('dav1d', version : dav1d_req)

# Everything platform dependent is taken from GLib and GStreamer on purpose,
# so that no per-OS compatibility code is needed. In particular the plugin
# uses GLib threading and g_get_num_processors() rather than pthreads and
# sysconf(), GST_ROUND_UP_N() rather than any allocation helper, and
# GstAllocationParams rather than posix_memalign().

plugins_install_dir = gst_dep.get_variable('pluginsdir',
  default_value : get_option('libdir') / 'gstreamer-1.0')

cdata = configuration_data()
cdata.set_quoted('PACKAGE', meson.project_name())
cdata.set_quoted('PACKAGE_VERSION', meson.project_version())
cdata.set_quoted('GST_LICENSE', 'LGPL')
cdata.set_quoted('GST_PACKAGE_NAME', 'GStreamer dav1d plugin')
cdata.set_quoted('GST_PACKAGE_ORIGIN', 'https://gstreamer.freedesktop.org')
configure_file(output : 'config.h', configuration : cdata)

gstdav1d_sources = [
  'src/gstdav1ddec.c',
  'src/plugin.c',
]

shared_library('gstdav1d',
  gstdav1d_sources,
  c_args : ['-DHAVE_CONFIG_H'],
  include_directories : include_directories('.', 'src'),
  dependencies : [gst_dep, gstbase_dep, gstvideo_dep, dav1d_dep],
  install : true,
  install_dir : plugins_install_dir,
)
