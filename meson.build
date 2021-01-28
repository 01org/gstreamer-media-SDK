# This project use C source code
project('GstMfx', 'c',
	version: '1.4.0.11',
	meson_version: '>= 0.35.0',
	default_options: ['warning_level=1',
			  'buildtype=debugoptimized'])

mfx_version = meson.project_version()
version_arr = mfx_version.split('.')
mfx_version_major = version_arr[0]
mfx_version_minor = version_arr[1]
mfx_version_micro = version_arr[2]
soversion = 0
libversion = '@0@.@1@.0'.format(soversion, mfx_version_minor.to_int() * 100 + mfx_version_micro.to_int())

cdata = configuration_data()
cdata.set ('GstMfx_PACKAGE', 'gstmfx')
cdata.set ('GstMfx_PACKAGE_NAME', 'GStreamer Media SDK')
cdata.set ('GstMfx_VERSION', '@0@'.format(mfx_version))
cdata.set ('GstMfx_PACKAGE_TARNAME', 'gstreamer-msdk')

configure_file (input: 'version.h.in',
		output: 'version.h',
		configuration: cdata)

mfx_sources = []
mfx_c_args = []
mfx_deps = []

mfx_c_args += ['-std=gnu99', '-fPIE', '-fPIC']

# Check the Media SDK
mfx_home = get_option ('MFX_HOME')

mfx_inc = include_directories(['.', 'gst/mfx', 'gst-libs/mfx', 'parsers', '@0@/include'.format(mfx_home)])

compiler = meson.get_compiler('c')
libmfx = compiler.find_library('mfx', dirs: '@0@/lib/lin_x64/'.format(mfx_home))
mfx_deps += [libmfx]

libstdcc = compiler.find_library('stdc++')
mfx_deps += [libstdcc]

libdl = compiler.find_library('dl')
mfx_deps += [libdl]

# Check base dependencies

glib_deps = [dependency('glib-2.0'), dependency('gobject-2.0'), dependency('gio-2.0'), dependency('gmodule-2.0')]
mfx_deps += glib_deps

gst_req = '>= 1.6.0'

gst_dep = dependency('gstreamer-1.0', version: gst_req,
  fallback: ['gstreamer', 'gst_dep'])
gstvideo_dep = dependency('gstreamer-video-1.0', version: gst_req,
    fallback: ['gst-plugins-base', 'video_dep'])
gstallocators_dep = dependency('gstreamer-allocators-1.0', version: gst_req,
    fallback: ['gst-plugins-base', 'allocators_dep'])
gstcodecparsers_dep = dependency('gstreamer-codecparsers-1.0', version: gst_req,
    fallback: ['gst-plugins-bad', 'gstcodecparsers_dep'], required: false)
gstpbutils_dep = dependency('gstreamer-pbutils-1.0', version: gst_req,
	fallback: ['gst-plugins-base', 'pbutils_dep'], required: false)

mfx_deps += [gst_dep, gstvideo_dep, gstallocators_dep]

with_codecparsers = false
if gstcodecparsers_dep.found()
	mfx_deps += [gstcodecparsers_dep]
	with_codecparsers = true
endif

with_pbutils = false
if gstpbutils_dep.found()
	mfx_deps += [gstpbutils_dep]
	with_pbutils = true
endif

driver_deps = [dependency ('libva'), dependency ('libdrm'), dependency ('libdrm_intel'),
		dependency ('libva-drm'), dependency('libudev')]

mfx_deps += driver_deps

# Parse common user options
with_mss = get_option('WITH_MSS_2016')
if with_mss
	mfx_c_args += ['-DWITH_MSS_2016']
endif

mfx_decoder = get_option('MFX_DECODER')
if mfx_decoder
	mfx_c_args += ['-DMFX_DECODER']

	if dependency('gst-plugins-bad', version: '>1.18').found()
		mfx_c_args += ['-DUSE_GST_PLUGINS_BAD_1_18']
	endif
endif

mfx_sink = get_option('MFX_SINK')
with_wayland = false
with_x11 = false
if mfx_sink
	mfx_c_args += ['-DMFX_SINK']
	if get_option ('WITH_WAYLAND') != 'no'
		need_wayland = get_option ('WITH_WAYLAND') == 'yes'

		wayland_client_dep = dependency('wayland-client',
						required: need_wayland)
		if wayland_client_dep.found()
			with_wayland = true
			mfx_deps += wayland_client_dep
			if dependency('weston', version: '>=3.0.0', required: false).found()
				mfx_c_args += ['-DUSE_WESTON_4_0']
			endif
		endif

		wayland_cursor_dep = dependency('wayland-cursor',
						required: false)
		if wayland_cursor_dep.found()
			mfx_deps += wayland_cursor_dep
		endif
	endif
	if get_option ('WITH_X11') != 'no'
		need_x11 = get_option ('WITH_X11') == 'yes'
		x11_dep = dependency('x11', required: need_x11)
		if x11_dep.found()
			with_x11 = true
			mfx_deps += [x11_dep]
		endif

		x11_optional_deps = [
			['xcb-dri3', '-DHAVE_XCBDRI3'],
			['xcb-present', '-DHAVE_XCBPRESENT'],
			['xkb-common', '-DHAVE_XKBLIB'],
			['xrandr', '-DHAVE_XRANDR'],
			['xrender', '-DHAVE_XRENDER'],
			['x11-xcb', '-DHAVE_X11XCB'],
		]

		foreach d: x11_optional_deps
			dep = dependency(d.get(0), required: false)
			if dep.found()
				mfx_deps += [dep]
				mfx_c_args += [d.get(1)]
			endif
		endforeach
	endif
else
	foreach opt: ['WITH_WAYLAND', 'WITH_X11', 'USE_EGL_RENDERER']
		if get_option (opt) == 'yes'
			error('@0@ required but MFX_SINK is false'.format(opt))
		endif
	endforeach
endif

use_egl_renderer = false
if with_wayland
	mfx_c_args += ['-DWITH_WAYLAND']
	if get_option ('USE_WAYLAND_RENDERER')  != 'no'
		mfx_c_args += ['-DUSE_WAYLAND']
	endif

	if get_option ('USE_EGL_RENDERER') != 'no'
		need_egl = get_option ('USE_EGL_RENDERER') == 'yes'
		egl_dep = dependency('egl', required: need_egl)
		egl_wayland_dep = dependency('wayland-egl', required: need_egl)
		if egl_dep.found() and egl_wayland_dep.found()
			use_egl_renderer = true
			mfx_deps += [egl_dep, egl_wayland_dep]
			mfx_c_args += ['-DUSE_EGL']
		endif
	endif
elif get_option ('USE_WAYLAND_RENDERER') == 'yes'
		error('USE_WAYLAND_RENDERER required but WITH_WAYLAND is false')
endif

if with_x11
	mfx_c_args += ['-DWITH_X11']
	if get_option ('USE_DRI3_RENDERER')  != 'no'
		mfx_c_args += ['-DUSE_DRI3']
	endif
	if use_egl_renderer == false
		need_egl = get_option ('USE_EGL_RENDERER') == 'yes'
		egl_dep = dependency('egl', required: need_egl)
		if egl_dep.found()
			use_egl_renderer = true
			egl_deps = [egl_dep]
			mfx_deps += egl_deps
			mfx_c_args += ['-DUSE_EGL']
		endif
	endif
elif get_option ('USE_DRI3_RENDERER') == 'yes'
		error('USE_DRI3_RENDERER required but WITH_WAYLAND is false')
endif



mfx_encoder = get_option('MFX_ENCODER')

subdir('gst-libs')
subdir('gst')
subdir('parsers')

gstvideo = shared_library('gstmfx',
  mfx_sources,
  c_args: mfx_c_args,
  include_directories: mfx_inc,
  version: libversion,
  soversion: soversion,
  install: true,
  install_dir: 'lib/gstreamer-1.0',
  dependencies: mfx_deps,
)
