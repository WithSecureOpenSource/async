import os
import fsenv

DIRECTORIES = [
    'src',
    'test',
    'components/async' ]

TARGET_DEFINES = {
    'freebsd_amd64': ['HAVE_EXECINFO'],
    'linux32': ['_FILE_OFFSET_BITS=64', 'HAVE_EXECINFO'],
    'linux64': ['HAVE_EXECINFO'],
    'linux_arm64': ['HAVE_EXECINFO'],
    'openbsd_amd64': [],
    'darwin': ['HAVE_EXECINFO']
}

TARGET_CPPPATH = {
    'freebsd_amd64': [],
    'linux32': [],
    'linux64': [],
    'linux_arm64': [],
    'openbsd_amd64': ['/usr/local/include'],
    'darwin': []
}

TARGET_LIBPATH = {
    'freebsd_amd64': [],
    'linux32': [],
    'linux64': [],
    'linux_arm64': [],
    'openbsd_amd64': ['/usr/local/lib'],
    'darwin': []
}

TARGET_LIBS = {
    'freebsd_amd64': ['execinfo'],
    'linux32': ['rt'],
    'linux64': ['rt'],
    'linux_arm64': ['rt'],
    'openbsd_amd64': ['iconv'],
    'darwin': ['iconv'],
}

TARGET_FLAGS = {
    'freebsd_amd64': '',
    'linux32': '-m32 ',
    'linux64': '',
    'linux_arm64': '',
    'openbsd_amd64': '',
    'darwin': ''
}

def construct():
    ccflags = (
        ' -g -O2 -Wall -Wextra -Werror '
        '-Wno-null-pointer-arithmetic '
        '-Wno-sign-compare '
        '-Wno-unknown-warning-option '
        '-Wno-unused-label '
        '-Wno-unused-parameter '
    )
    prefix = ARGUMENTS.get('prefix', '/usr/local')
    for target_arch in fsenv.target_architectures():
        arch_env = Environment(
            NAME='async',
            ARCH=target_arch,
            PREFIX=prefix,
            PKG_CONFIG_LIBS=['fsdyn', 'encjson', 'fstrace', 'unixkit'],
            CCFLAGS=TARGET_FLAGS[target_arch] + ccflags,
            CPPDEFINES=TARGET_DEFINES[target_arch],
            CPPPATH=TARGET_CPPPATH[target_arch],
            LINKFLAGS=TARGET_FLAGS[target_arch],
            TARGET_LIBPATH=TARGET_LIBPATH[target_arch],
            TARGET_LIBS=TARGET_LIBS[target_arch],
            tools=['default', 'textfile', 'fscomp', 'scons_compilation_db'])
        fsenv.consider_environment_variables(arch_env)
        if target_arch == "darwin":
            env.AppendENVPath("PATH", "/opt/local/bin")
        build_dir = os.path.join(
            fsenv.STAGE,
            target_arch,
            ARGUMENTS.get('builddir', 'build'))
        arch_env.CompilationDB(
            os.path.join(build_dir, "compile_commands.json"))
        for directory in DIRECTORIES:
            env = arch_env.Clone()
            env.SetCompilationDB(arch_env.GetCompilationDB())
            SConscript(dirs=directory,
                       exports=['env'],
                       duplicate=False,
                       variant_dir=os.path.join(build_dir, directory))
        Clean('.', build_dir)

if __name__ == 'SCons.Script':
    construct()
