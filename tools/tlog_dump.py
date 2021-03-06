#!/usr/bin/env python

# Copyright 2015 Josh Pieper, jjp@pobox.com.  All rights reserved.
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

import itertools
import optparse
import os
import sys
import time

SCRIPT_PATH=os.path.dirname(os.path.realpath(__file__))
sys.path.append(os.path.join(SCRIPT_PATH, '../python'))

import telemetry_log

def main():
    parser = optparse.OptionParser()
    parser.add_option('-i', '--input', default=None, help='input file')
    parser.add_option('-n', '--name', default=[], action='append',
                      help='name to export')
    parser.add_option('-o', '--output', default='-', help='destination')
    parser.add_option('-s', '--sort', action='store_true',
                      help='sort record by type')
    parser.add_option('-t', '--timestamps', action='store_true',
                      help='Make timestamps readable')

    options, args = parser.parse_args()

    if options.input is None:
        options.input = args[0]

    if options.output == '-':
        out = sys.stdout
    else:
        out = open(options.output, 'w')

    br = telemetry_log.BulkReader(open(options.input))
    pred = lambda x: True
    if options.name:
        pred = lambda x: x in options.name

    if options.sort:
        data = itertools.chain(
            *(data for (_, data) in sorted(br.get(pred).iteritems()))
        )
    else:
        data = (data for (_, data) in br.items(pred))

    for item in data:
        if options.timestamps:
            ts_str = time.strftime('%F %T', time.gmtime(item.timestamp)) + \
                     ('%.6f' % (item.timestamp % 1.0))[1:]
            item = item._replace(timestamp=ts_str)
        print >>out, item

if __name__ == '__main__':
    main()
