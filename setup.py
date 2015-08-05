#!/usr/bin/env python

import os
try:
    # Use setuptools if available, for install_requires (among other things).
    import setuptools
    from setuptools import setup
except ImportError:
    setuptools = None
    from distutils.core import setup
from distutils.command.build_scripts import build_scripts
from subprocess import call


BASEPATH = os.path.dirname(os.path.abspath(__file__))


class BuildBluepyHelper(build_scripts):
    def run(self):
      cwd = os.getcwd()
      os.chdir(os.path.join(BASEPATH, 'bluepy'))
      cmd = ['make']

      def _make():
        call(cmd)
      self.execute(_make, [])
      os.chdir(cwd)
      build_scripts.run(self)


kwargs = {}

version = '0.9.0'

with open('README.md') as f:
    kwargs['long_description'] = f.read()

setup(
    name='bluepy',
    cmdclass={'build_scripts': BuildBluepyHelper},
    version=version,
    scripts={'bluepy/bluepy-helper'},
    packages=['bluepy'],
    author='Ian Harvey',
    author_email='',
    url='https://github.com/IanHarvey/bluepy',
    license='https://www.gnu.org/licenses/gpl-2.0.txt',
    description='Python interface to Bluetooth LE on Linux',
    classifiers=[],
    **kwargs
)
