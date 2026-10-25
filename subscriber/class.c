#include "class.h"
#include <Python.h>
#include <stdio.h>
#include <string.h>

static const char *class_types[] = { "ClassDef", NULL };

static void on_class(const char *type, int lineno, const char *name, void *node_ptr, void *userdata)
{
	(void)type; (void)userdata;
	PyObject *node = (PyObject *)node_ptr;
	PyObject *bases = PyObject_GetAttrString(node, "bases");
	printf("class %s (line %d)", name ? name : "<anon>", lineno);
	if (bases && PyList_Check(bases) && PyList_Size(bases) > 0) {
		printf(" bases:");
		Py_ssize_t n = PyList_Size(bases);
		for (Py_ssize_t i = 0; i < n; i++) {
			PyObject *b = PyList_GetItem(bases, i);
			PyObject *s = PyObject_Str(b);
			if (s) {
				const char *bs = PyUnicode_AsUTF8(s);
				printf(" %s", bs ? bs : "?");
				Py_DECREF(s);
			}
		}
	}
	printf("\n");
	Py_XDECREF(bases);
}

AstSubscriber *class_subscriber(void)
{
	static AstSubscriber sub;
	static int init = 0;
	if (!init) {
		sub.node_types = class_types;
		sub.on_node = on_class;
		sub.userdata = NULL;
		sub.next = NULL;
		init = 1;
	}
	return &sub;
}
