/*
 * gumbo-libxml.c
 * Copyright (C) 2017 Kovid Goyal <kovid at kovidgoyal.net>
 *
 * Distributed under terms of the Apache 2.0 license.
 */

// Based on https://github.com/nostrademons/gumbo-libxml/blob/master/gumbo_libxml.c


#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <libxml/tree.h>
#include <libxml/dict.h>

#include <assert.h>
#include <string.h>

#include "../gumbo/gumbo.h"

#define UNUSED __attribute__ ((unused))

// Namespace constants, indexed by GumboNamespaceEnum.
static const char* kLegalXmlns[] = {
    "http://www.w3.org/1999/xhtml",
    "http://www.w3.org/2000/svg",
    "http://www.w3.org/1998/Math/MathML"
};

// Stack {{{

typedef struct {
    GumboNode *gumbo;
    xmlNodePtr xml;
} StackItem;

typedef struct {
    size_t length;
    size_t capacity;
    StackItem *items;
} Stack;

static inline Stack*
alloc_stack(size_t sz) {
    Stack *ans = (Stack*)calloc(sizeof(Stack), 1);
    if (ans) {
        ans->items = (StackItem*)malloc(sizeof(StackItem) * sz);
        if (ans->items) ans->capacity = sz;
        else { free(ans); ans = NULL; }
    }
    return ans;
}

static inline void
free_stack(Stack *s) { if (s) { free(s->items); free(s); } }

static inline void
stack_pop(Stack *s, GumboNode **g, xmlNodePtr *x) { StackItem *si = &(s->items[--(s->length)]); *g = si->gumbo; *x = si->xml; }

static inline bool
stack_push(Stack *s, GumboNode *g, xmlNodePtr x) {
    if (s->length >= s->capacity) {
        s->capacity *= 2;
        s->items = (StackItem*)realloc(s->items, s->capacity * sizeof(StackItem));
        if (!s->items) return false;
    }
    StackItem *si = &(s->items[(s->length)++]);
    si->gumbo = g; si->xml = x;
    return true;
}
// }}}

static inline bool
push_children(xmlNodePtr parent, GumboElement *elem, Stack *stack) {
    for (int i = elem->children.length - 1; i >= 0; i--) {
        if (!stack_push(stack, elem->children.data[i], parent)) return false;
    }
    return true;
}


static inline bool
create_attributes(xmlDocPtr doc, xmlNodePtr node, GumboElement *elem) {
    GumboAttribute* attr;
    const xmlChar *attr_name;
    for (unsigned int i = 0; i < elem->attributes.length; ++i) {
        attr = elem->attributes.data[i];
        attr_name = xmlDictLookup(doc->dict, BAD_CAST attr->name, -1);
        if (!attr_name) return false;
        if (!xmlNewNsPropEatName(node, NULL, (xmlChar*)attr->name, BAD_CAST attr->value)) return false;
    }
    return true;
}


static inline xmlNodePtr
create_element(xmlDocPtr doc, GumboNode *node, GumboElement **store_elem) {
    xmlNodePtr result = NULL;
    xmlNsPtr namespace = NULL;
    const xmlChar *tag_name = NULL;
    GumboElement* elem = &node->v.element;
    bool ok = true;
    *store_elem = elem;

    tag_name = xmlDictLookup(doc->dict, BAD_CAST gumbo_normalized_tagname(elem->tag), -1);
    if (!tag_name) return NULL;
    result = xmlNewNodeEatName(NULL, (xmlChar*)tag_name);
    if (!result) return NULL;

    // Namespace
    if (node->parent->type != GUMBO_NODE_DOCUMENT &&
            elem->tag_namespace != node->parent->v.element.tag_namespace) {
        namespace = xmlNewNs(
                result, BAD_CAST kLegalXmlns[elem->tag_namespace], NULL);
        if (!namespace) { ok = false; goto end; }
        xmlSetNs(result, namespace);
    } 
    if (!create_attributes(doc, result, elem)) { ok = false; goto end; }
end:
    if (!ok) { xmlFreeNode(result); result = NULL; }
    return result;
}


static xmlNodePtr convert_node(xmlDocPtr doc, GumboNode* node, GumboElement **elem) {
    xmlNodePtr ans = NULL;
    *elem = NULL;

    switch (node->type) {
        case GUMBO_NODE_DOCUMENT:
            assert(false &&
                    "convert_node cannot be used on the document node.  "
                    "Doctype information is automatically added to the xmlDocPtr.");
            break;
        case GUMBO_NODE_ELEMENT:
        case GUMBO_NODE_TEMPLATE:
            ans = create_element(doc, node, elem);
            break;
        case GUMBO_NODE_TEXT:
        case GUMBO_NODE_WHITESPACE:
            ans = xmlNewText(BAD_CAST node->v.text.text);
            break;
        case GUMBO_NODE_COMMENT:
            ans = xmlNewComment(BAD_CAST node->v.text.text);
            break;
        case GUMBO_NODE_CDATA:
            {
                // TODO: probably would be faster to use some calculation on
                // original_text.length rather than strlen, but I haven't verified that
                // that's correct in all cases.
                const char* node_text = node->v.text.text;
                ans = xmlNewCDataBlock(doc, BAD_CAST node_text, strlen(node_text));
            }
            break;
        default:
            assert(false && "unknown node type");
    }
    if (!ans && !PyErr_Occurred()) PyErr_NoMemory();
    return ans;
}

static xmlNodePtr
convert_tree(xmlDocPtr doc, GumboNode *root, size_t sz) {
    Stack *stack = alloc_stack(sz);
    xmlNodePtr ans = NULL, parent = NULL, child = NULL;
    GumboNode *gumbo = NULL;
    bool ok = true;
    GumboElement *elem;

    if (stack == NULL) { PyErr_NoMemory(); return NULL; }
    stack_push(stack, root, NULL);

    while(stack->length > 0) {
        stack_pop(stack, &gumbo, &parent);
        child = convert_node(doc, gumbo, &elem);
        if (!child) { ok = false; goto end; };
        if (parent) xmlAddChild(parent, child);
        else ans = child;
        if (elem != NULL) {
            if (!push_children(child, elem, stack)) { ok = false; goto end; };
        }

    }
end:
    if (!ok) {
        if (ans) { xmlFreeNode(ans); ans = NULL; }
        if (!PyErr_Occurred()) PyErr_NoMemory();
    }
    free_stack(stack);
    return ans;
}

static bool 
parse_with_options(xmlDocPtr doc, GumboOptions* options, const char* buffer, size_t buffer_length, bool keep_doctype) {
    GumboOutput *output = NULL;
    xmlNodePtr root = NULL;
    output = gumbo_parse_with_options(options, buffer, buffer_length);
    if (output == NULL) { PyErr_NoMemory(); return false; }
    if (keep_doctype) {
        GumboDocument* doctype = & output->document->v.document;
        if(!xmlCreateIntSubset(
                doc,
                BAD_CAST doctype->name,
                BAD_CAST doctype->public_identifier,
                BAD_CAST doctype->system_identifier)) {
            PyErr_NoMemory();
            gumbo_destroy_output(output);
            return false;
        }
    }
    root = convert_tree(doc, output->root, 16 * 1024);
    if (root) xmlDocSetRootElement(doc, root);
    gumbo_destroy_output(output);
    return root ? true : false;
}

// Python wrapper {{{

static char *NAME =  "libxml2:xmlDoc";
static char *DESTRUCTOR = "destructor:xmlFreeDoc";

static void 
free_encapsulated_doc(PyObject *capsule) {
    xmlDocPtr doc = PyCapsule_GetPointer(capsule, NAME);
    if (doc != NULL) {
        char *ctx = PyCapsule_GetContext(capsule);
        if (ctx == DESTRUCTOR) xmlFreeDoc(doc);
    }
}

static inline PyObject*
encapsulate(xmlDocPtr doc) {
    PyObject *ans = NULL;
    ans = PyCapsule_New(doc, NAME, free_encapsulated_doc);
    if (ans == NULL) { xmlFreeDoc(doc); return NULL; }
    if (PyCapsule_SetContext(ans, DESTRUCTOR) != 0) { Py_DECREF(ans); return NULL; }
    return ans;
}

static PyObject *
parse(PyObject UNUSED *self, PyObject *args) {
    GumboOptions options = kGumboDefaultOptions;
    xmlDocPtr doc = NULL;
    char *buffer = NULL;
    Py_ssize_t sz = 0;

    if (!PyArg_ParseTuple(args, "s#", &buffer, &sz)) return NULL;

    doc = xmlNewDoc(BAD_CAST "1.0");
    if (doc == NULL) return PyErr_NoMemory();
    if (doc->dict == NULL) {
        doc->dict = xmlDictCreate();
        if (doc->dict == NULL) {
            xmlFreeDoc(doc);
            return PyErr_NoMemory();
        }
    }

    if (!parse_with_options(doc, &options, buffer, (size_t)sz, false)) { xmlFreeDoc(doc); return NULL; }
    return encapsulate(doc);
}

static PyObject *
clone_doc(PyObject UNUSED *self, PyObject *capsule) {
    if (!PyCapsule_CheckExact(capsule)) { PyErr_SetString(PyExc_TypeError, "Must specify a capsule as the argument"); return NULL; }
    xmlDocPtr sdoc = PyCapsule_GetPointer(capsule, PyCapsule_GetName(capsule)), doc;
    if (sdoc == NULL) return NULL;
    doc = xmlCopyDoc(sdoc, 1);
    if (doc == NULL) return PyErr_NoMemory();
    return encapsulate(doc);
}

static PyMethodDef 
methods[] = {
    {"parse", parse, METH_VARARGS,
        "parse()\n\nParse specified bytestring which must be in the UTF-8 encoding."
    },

    {"clone_doc", clone_doc, METH_O,
        "clone_doc()\n\nClone the specified document. Which must be a document returned by the parse() function."
    },

    {NULL, NULL, 0, NULL}
};

#ifdef NDEBUG
static const char* MODULE = "html_parser";
PyMODINIT_FUNC
inithtml_parser(void) {
#else
static const char* MODULE = "html_parser_debug";
PyMODINIT_FUNC
inithtml_parser_debug(void) {
#endif
    PyObject *m;
    m = Py_InitModule3(MODULE, methods, "HTML parser in C for speed.");
    if (m == NULL) return;
}
// }}}