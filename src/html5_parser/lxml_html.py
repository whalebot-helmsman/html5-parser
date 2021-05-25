#!/usr/bin/env python
# vim:fileencoding=utf-8
# License: Apache 2.0 Copyright: 2017, Kovid Goyal <kovid at kovidgoyal.net>

from __future__ import absolute_import, division, print_function, unicode_literals

from lxml.etree import _Comment
from lxml.html import html_parser, HtmlComment


control_characters = [ui for ui in range(32)] + [0x7f]
translator = str.maketrans({c: '' for c in control_characters})

def convert_elem(src):
    if isinstance(src, _Comment):
        element = HtmlComment(src.text.translate(translator))
    else:
        attrib = {k.translate(translator): v.translate(translator) for k, v in src.attrib.items()}
        element = html_parser.makeelement(
                src.tag,
                attrib=attrib,
            )

    return element


def no_control_characters(value):
    if not value:
        return value
    return value.translate(translator)


def adapt(src_tree, return_root=True, **kw):
    src_root = src_tree.getroot()
    dest_root = convert_elem(src_root)
    stack = [(src_root, dest_root)]
    while stack:
        src, dest = stack.pop()
        for src_child in src.iterchildren():
            dest_child = convert_elem(src_child)
            dest_child.text = no_control_characters(src_child.text)
            dest_child.tail = no_control_characters(src_child.tail)
            stack.append((src_child, dest_child))
            dest.append(dest_child)
    return dest_root if return_root else dest_root.getroottree()
