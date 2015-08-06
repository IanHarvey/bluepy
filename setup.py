"""Python setup script for bluepy"""

from setuptools.command.install import install
from setuptools.command.develop import develop
from setuptools import setup
from subprocess import Popen, PIPE
import shlex
import sys


def pre_install():
    """Do the custom compiling of the bluepy-helper executable from the makefile"""
    cmd = shlex.split("make -C ./bluepy")
    proc = Popen(cmd, stdout=PIPE, stderr=PIPE, shell=False)
    proc.wait()
    if proc.returncode != 0:
        print("Bluez build failed. Exiting install. Make output:")
        print(proc.stdout.read())
        sys.exit(1)

def post_install():
    """Post installation tasks"""
    pass

def setup_command(command_subclass):
    """Decorator for customizing setuptools.command subclasses"""
    orig_run = command_subclass.run
    def custom_run(self):

        pre_install()        
        orig_run(self)
        post_install()

    command_subclass.run = custom_run
    return command_subclass

@setup_command
class BluepyInstall(install):
    pass

@setup_command
class BluepyDevelop(develop):
    pass

setup (
    name='bluepy',
    version='0.9.0',
    description='Python module for interfacing with BLE devices through Bluez',
    author='Ian Harvey',
    url='https://github.com/IanHarvey/bluepy',
    classifiers=[
        'Programming Language :: Python :: 2.7'
    ],
    packages=['bluepy'],
    package_data={
        'bluepy': ['bluepy-helper', '*.json']
    },
    cmdclass={'install': BluepyInstall, 'develop': BluepyDevelop},
    entry_points={
        'console_scripts': [
            'sensortag=bluepy.sensortag:main',
        ]
    }
)



