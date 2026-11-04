#!/usr/bin/env python3
"""Add source files to the three build systems (CMakeLists.txt, Makefile.am,
Dismount.vcxproj and its filters) after a file which is already listed.

    add_sources.py <after file> <new file>...      (paths from the root)

Sources (.cpp) and headers (.h) are handled; the vcxproj files are CRLF.
"""
import os
import sys

REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..'))


def edit(path, function):
    with open(os.path.join(REPO, path), 'rb') as f:
        data = f.read().decode('utf-8')
    updated = function(data)
    with open(os.path.join(REPO, path), 'wb') as f:
        f.write(updated.encode('utf-8'))


def insert_after_line(data, marker, new_lines, newline):
    lines = data.split(newline)
    for i, line in enumerate(lines):
        if marker in line:
            lines[i + 1:i + 1] = new_lines
            return newline.join(lines)
    raise SystemExit('marker %s not found' % marker)


def main():
    after = sys.argv[1]
    files = [f for f in sys.argv[2:]]
    is_header = after.endswith('.h')

    def cmake(data):
        marker = after
        if marker not in data:
            return data
        if files[0] in data:
            return data
        lines = ['\t' + f for f in files]
        return insert_after_line(data, marker, lines, '\n')

    def automake(data):
        if after not in data or files[0] in data:
            return data
        return insert_after_line(data, after, ['                         %s \\' % f for f in files], '\n')

    def vcxproj(data):
        windows = after.replace('/', '\\')
        if windows not in data or files[0].replace('/', '\\') in data:
            return data
        tag = 'ClInclude' if is_header else 'ClCompile'
        return insert_after_line(
            data, windows, ['    <%s Include="%s" />' % (tag, f.replace('/', '\\')) for f in files], '\r\n')

    def filters(data):
        windows = after.replace('/', '\\')
        if windows not in data or files[0].replace('/', '\\') in data:
            return data
        tag = 'ClInclude' if is_header else 'ClCompile'
        lines = data.split('\r\n')
        for i, line in enumerate(lines):
            if windows in line:
                # the element spans until its closing tag
                j = i
                while '</%s>' % tag not in lines[j]:
                    j += 1
                block = []
                for f in files:
                    block.append('    <%s Include="%s">' % (tag, f.replace('/', '\\')))
                    block.append(lines[i + 1])
                    block.append('    </%s>' % tag)
                lines[j + 1:j + 1] = block
                return '\r\n'.join(lines)
        return data

    if not is_header:
        edit('CMakeLists.txt', cmake)
        edit('Makefile.am', automake)
    edit('Dismount.vcxproj', vcxproj)
    edit('Dismount.vcxproj.filters', filters)


if __name__ == '__main__':
    main()
