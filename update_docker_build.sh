#!/bin/bash

# 替换../../script/build-with-container.py中的--pull=always为--pull=true
sed -i 's/--pull=always/--pull=true/g' ../../script/build-with-container.py