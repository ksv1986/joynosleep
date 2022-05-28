project('joynosleep-c', 'c',
  version : '0.1',
  default_options : ['warning_level=3'])

dep = dependency('libsystemd')
exe = executable('joynosleep', 'joynosleep.c',
  dependencies: dep, install : true)

test('basic', exe)
