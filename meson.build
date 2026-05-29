project('wh-wall', 'c',
  version: '0.1.0',
  default_options: ['c_std=c11', 'warning_level=2']
)

gtk4_dep    = dependency('gtk4')
curl_dep    = dependency('libcurl')
json_dep    = dependency('json-glib-1.0')
threads_dep = dependency('threads')

executable('wh-wall',
  'src/main.c',
  'src/api.c',
  'src/api.h',
  'src/config.c',
  'src/config.h',
  'src/log.h',
  'src/window.c',
  'src/window.h',
  'src/wallpaper.c',
  'src/wallpaper.h',
  dependencies: [gtk4_dep, curl_dep, json_dep, threads_dep],
  install: true
)
