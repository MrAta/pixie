#!/usr/bin/env python3

# Copyright 2018- The Pixie Authors.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# SPDX-License-Identifier: Apache-2.0

import argparse
import glob
import os
import sys
import yaml


def script_list_format(github_url, folder, script, desc):
    return f"- {folder}/[{script}]({github_url}/{folder}/{script}): {desc}"


def get_script_list(github_url):
    readme_lines = []
    manifest_paths = glob.glob('**/manifest.yaml', recursive=True)
    for path in sorted(manifest_paths):
        if 'private' in path:
            continue
        with open(path) as yaml_file:
            desc = yaml.safe_load(yaml_file).get("long").strip()
            path_parts = path.split(os.sep)
            readme_lines.append(script_list_format(github_url, path_parts[0], path_parts[1], desc))
    return readme_lines


header_text = """
<!-- The text in this file is automatically generated by the update_readme.py script. -->
# PXL Scripts Overview

Pixie open sources all of its scripts, which serve as examples of scripting in the PxL language.
To learn more about PxL, take a look at our [documentation](https://docs.px.dev/reference/pxl).
"""

# get relative path to PxL script folder (px/)
parser = argparse.ArgumentParser()
parser.add_argument('script_folder_path')
parser.add_argument('github_url')
args = parser.parse_args()

if not os.path.exists(args.script_folder_path):
    sys.exit("The PxL script folder path passed to update_readme.py does not exist.")

os.chdir(args.script_folder_path)

# put README.md file in directory containing PxL script folder
with open("./README.md", "w") as f:
    f.write(header_text)
    for line in get_script_list(args.github_url):
        f.write(line + "\n")
