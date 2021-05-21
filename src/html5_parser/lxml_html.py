#!/usr/bin/env python
# vim:fileencoding=utf-8
# License: Apache 2.0 Copyright: 2017, Kovid Goyal <kovid at kovidgoyal.net>

from __future__ import absolute_import, division, print_function, unicode_literals
import copy
import unicodedata

from lxml.etree import _Comment
from lxml.html import html_parser


def convert_elem(src):
    return html_parser.makeelement(src.tag, attrib=src.attrib)


def no_control_characters(value):
    if not value:
        return value
    return ''.join(c for c in value if unicodedata.category(c)[0] != 'C')


def adapt(src_tree, return_root=True, **kw):
    src_root = src_tree.getroot()
    dest_root = convert_elem(src_root)
    stack = [(src_root, dest_root)]
    while stack:
        src, dest = stack.pop()
        for src_child in src.iterchildren():
            if isinstance(src_child, _Comment):
                dest_child = copy.copy(src_child)
            else:
                dest_child = convert_elem(src_child)
                dest_child.text = no_control_characters(src_child.text)
                dest_child.tail = no_control_characters(src_child.tail)
                stack.append((src_child, dest_child))
            dest.append(dest_child)
    return dest_root if return_root else dest_root.getroottree()
