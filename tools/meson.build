libarchive = dependency('libarchive', required: false)
if not libarchive.found()
    # homebrew
    libarchive_lib = cc.find_library('archive', dirs: ['/usr/local/opt/libarchive/lib'], required: false)
    if libarchive_lib.found()
        libarchive = declare_dependency(dependencies: [libarchive_lib], include_directories: include_directories(['/usr/local/opt/libarchive/include']))
    else
        # Apple Silicon Homebrew
        libarchive_lib = cc.find_library('archive', dirs: ['/opt/homebrew/opt/libarchive/lib'], required: false)
        if libarchive_lib.found()
            libarchive = declare_dependency(dependencies: [libarchive_lib], include_directories: include_directories(['/opt/homebrew/opt/libarchive/include']))
        endif
    endif
endif

if libarchive.found()
    fakefsify_src = [
        'fakefsify.c',
        'fakefs.c',
        '../util/fchdir.c',
    ]
    fakefsify = executable('fakefsify', fakefsify_src, dependencies: [libarchive], link_with: [libfakefs], include_directories: [includes])
    custom_target('unfakefsify',
        build_by_default: true,
        command: ['ln', '-sf', 'fakefsify', '@OUTPUT@'],
        output: 'unfakefsify')
endif
