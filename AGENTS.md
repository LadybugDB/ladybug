Use

make GEN=Ninja python

to compile

For debug builds, add -debug suffix to Makefile targets.

Python is an important surface area to test. If you make significant changes use

make GEN=Ninja pytest-venv 

to test.

Prefer using uv over pip for venv management.
