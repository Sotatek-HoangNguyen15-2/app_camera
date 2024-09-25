cpp = meson.get_compiler('cpp')
dep_wayland_client = dependency('wayland-client', version: '>= 1.20.0')

dep_wp = dependency('wayland-protocols', version: '>= 1.24')
dir_wp_base = dep_wp.get_pkgconfig_variable('pkgdatadir')

grpcpp_reflection_dep = cpp.find_library('grpc++_reflection')
protoc = find_program('protoc')
grpc_cpp = find_program('grpc_cpp_plugin')

dep_scanner = dependency('wayland-scanner')
prog_scanner = find_program('wayland-scanner')

protoc_gen = generator(protoc, \
                       output : ['@BASENAME@.pb.cc', '@BASENAME@.pb.h'],
                       arguments : ['--proto_path=@CURRENT_SOURCE_DIR@/protocol',
                         '--cpp_out=@BUILD_DIR@',
                         '@INPUT@'])

generated_protoc_sources = protoc_gen.process('protocol/agl_shell.proto')

grpc_gen = generator(protoc, \
                     output : ['@BASENAME@.grpc.pb.cc', '@BASENAME@.grpc.pb.h'],
                     arguments : ['--proto_path=@CURRENT_SOURCE_DIR@/protocol',
                       '--grpc_out=@BUILD_DIR@',
                       '--plugin=protoc-gen-grpc=' + grpc_cpp.path(),
                       '@INPUT@'])

generated_grpc_sources = grpc_gen.process('protocol/agl_shell.proto')

grpc_deps = [
    dependency('protobuf'),
    dependency('grpc'),
    dependency('grpc++'),
    grpcpp_reflection_dep,
]


protocols = [
        [ 'xdg-shell', 'stable' ],
]

foreach proto: protocols
        proto_name = proto[0]
        if proto[1] == 'internal'
                base_file = proto_name
                xml_path = '@0@.xml'.format(proto_name)
        elif proto[1] == 'stable'
                base_file = proto_name
                xml_path = '@0@/stable/@1@/@1@.xml'.format(dir_wp_base, base_file)
        else
                base_file = '@0@-unstable-@1@'.format(proto_name, proto[1])
                xml_path = '@0@/unstable/@1@/@2@.xml'.format(dir_wp_base, proto_name, base_file)
        endif

        foreach output_type: [ 'client-header', 'server-header', 'private-code' ]
                if output_type == 'client-header'
                        output_file = '@0@-client-protocol.h'.format(base_file)
                elif output_type == 'server-header'
                        output_file = '@0@-server-protocol.h'.format(base_file)
                else
                        output_file = '@0@-protocol.c'.format(base_file)
                        if dep_scanner.version().version_compare('< 1.14.91')
                                output_type = 'code'
                        endif
                endif

                var_name = output_file.underscorify()
                target = custom_target(
                        '@0@ @1@'.format(base_file, output_type),
                        command: [ prog_scanner, output_type, '@INPUT@', '@OUTPUT@' ],
                        input: xml_path,
                        output: output_file,
                )

                set_variable(var_name, target)
        endforeach
endforeach

depnames_gstreamer = [
        'gstreamer-1.0', 'gstreamer-plugins-bad-1.0', 'gstreamer-wayland-1.0',
        'gstreamer-video-1.0', 'gstreamer-plugins-base-1.0',
]

deps_gstreamer = []
foreach depname : depnames_gstreamer
        dep = dependency(depname, required: false)
        if not dep.found()
                error('Required @0@ which was not found. '.format(depname) + user_hint)
        endif
        deps_gstreamer += dep
endforeach


camera_gstreamer_dep = [
    dep_wayland_client,
    deps_gstreamer,
    grpc_deps
]

camera_gstreamer_src_headers = [
  xdg_shell_client_protocol_h,
  'utils.h',
  'AglShellGrpcClient.h',
]

camera_gstreamer_src = [
  xdg_shell_protocol_c,
  'utils.cpp',
  'AglShellGrpcClient.cpp',
  'main.cpp',
  generated_protoc_sources,
  generated_grpc_sources
]

install_data('still-image.jpg', install_dir: get_option('datadir') / 'applications/data')

executable('camera-gstreamer', camera_gstreamer_src, camera_gstreamer_src_headers,
            dependencies : camera_gstreamer_dep,
            install: true)
