import shutil
import subprocess
import multiprocessing
import os
import sys

from setuptools import setup, Extension
from setuptools.command.build_ext import build_ext
from setuptools.command.build_py import build_py as _build_py

base_dir = os.path.dirname(__file__)


def _get_lbug_version():
    cmake_file = os.path.join(base_dir, 'real_ladybug-source', 'CMakeLists.txt')
    with open(cmake_file) as f:
        for line in f:
            if line.startswith('project(Lbug VERSION'):
                raw_version = line.split(' ')[2].strip()
                version_nums = raw_version.split('.')
                if len(version_nums) <= 3:
                    return raw_version
                else:
                    dev_suffix = version_nums[3]
                    version = '.'.join(version_nums[:3])
                    version += ".dev%s" % dev_suffix
                    return version

lbug_version = _get_lbug_version()
print("The version of this build is %s" % lbug_version)


class CMakeExtension(Extension):
    def __init__(self, name: str, sourcedir: str = "") -> None:
        super().__init__(name, sources=[])
        self.sourcedir = os.path.abspath(sourcedir)


class CMakeBuild(build_ext):
    def build_extension(self, ext: CMakeExtension) -> None:
        self.announce("Building native extension...")
        # Pass the platform architecture for arm64 to cmake for
        # cross-compilation.
        env_vars = os.environ.copy()
        python_version = '.'.join(
            (str(sys.version_info.major),  str(sys.version_info.minor)))
        self.announce("Python version is %s" % python_version)
        env_vars['PYBIND11_PYTHON_VERSION'] = python_version
        env_vars['PYTHON_EXECUTABLE'] = sys.executable

        if sys.platform == 'darwin':
            archflags = os.getenv("ARCHFLAGS", "")

            if len(archflags) > 0:
                self.announce("The ARCHFLAGS is set to '%s'." %
                              archflags)
                if archflags == "-arch arm64":
                    env_vars['CMAKE_OSX_ARCHITECTURES'] = "arm64"
                elif archflags == "-arch x86_64":
                    env_vars['CMAKE_OSX_ARCHITECTURES'] = "x86_64"
                else:
                    self.announce(
                        "The ARCHFLAGS is not valid and will be ignored.")
            else:
                self.announce("The ARCHFLAGS is not set.")

            deploy_target = os.getenv("MACOSX_DEPLOYMENT_TARGET", "")
            if len(deploy_target) > 0:
                self.announce("The deployment target is set to '%s'." %
                              deploy_target)
                env_vars['CMAKE_OSX_DEPLOYMENT_TARGET'] = deploy_target

        build_dir = os.path.join(ext.sourcedir, 'real_ladybug-source')

        # Clean the build directory.
        subprocess.run(['make', 'clean'], check=True, cwd=build_dir)

        try:
            num_cores = int(os.environ['NUM_THREADS'])
            self.announce("Using %d cores for building the native extension." % num_cores)
        except:
            self.announce("NUM_THREADS is not set. Using all available cores.")
            num_cores = multiprocessing.cpu_count()

        # Build the native extension.
        full_cmd = ['make', 'python', 'NUM_THREADS=%d' % num_cores]
        subprocess.run(full_cmd, cwd=build_dir, check=True, env=env_vars)
        self.announce("Done building native extension.")
        self.announce("Copying native extension...")
        dst = os.path.join(ext.sourcedir, ext.name)
        shutil.rmtree(dst, ignore_errors=True)
        shutil.copytree(os.path.join(build_dir, 'tools', 'python_api', 'build',
                                     ext.name), dst)
        # Copy to build directory (for wheel packaging) - THIS IS CRITICAL
        self.announce("Copying native extension to build directory...")
        build_dst = os.path.join(self.build_lib, ext.name)
        shutil.rmtree(build_dst, ignore_errors=True)
        shutil.copytree(os.path.join(build_dir, 'tools', 'python_api', 'build',
                                     ext.name), build_dst)

        self.announce("Done copying native extension.")


class BuildExtFirst(_build_py):
    # Override the build_py command to build the extension first.
    def run(self):
        self.run_command("build_ext")
        return super().run()


setup(name='real_ladybug',
      version=lbug_version,
      install_requires=[],
      ext_modules=[CMakeExtension(
          name="real_ladybug", sourcedir=base_dir)],
      description='An in-process property graph database management system built for query speed and scalability.',
      license='MIT',
      long_description=open(os.path.join(base_dir, "README.md"), 'r').read(),
      long_description_content_type="text/markdown",
      packages=["real_ladybug"],
      zip_safe=True,
      include_package_data=True,
      cmdclass={
          'build_py': BuildExtFirst,
          'build_ext': CMakeBuild,
      }
      )
