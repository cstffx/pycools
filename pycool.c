#define _GNU_SOURCE
#define _GNU_SOURCE
#include "pycool.h"
#include "ast.h"
#include "traverse.h"
#include "index/index.h"
#include "observer.h"
#include "registry.h"
#include "ignore.h"
#include <signal.h>
#include <Python.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>

static Ignore *pycool_ignore = NULL;
static const char *pycool_root = NULL;
static volatile sig_atomic_t pycool_running = 1;
static volatile sig_atomic_t pycool_clean_requested = 0;
static volatile sig_atomic_t pycool_reindex_requested = 0;

typedef struct { sqlite3 *db; sqlite3_int64 file_id; } Ctx;

static const char *symbol_types[] = { "ClassDef", "FunctionDef", "AsyncFunctionDef", "Assign", "Name", NULL };

static void extract_assign_targets(PyObject *target, Ctx *ctx, int line)
{
	if (!target) return;
	PyObject *cls = PyObject_GetAttrString(target, "__class__");
	PyObject *cname = cls ? PyObject_GetAttrString(cls, "__name__") : NULL;
	const char *tname = cname ? PyUnicode_AsUTF8(cname) : NULL;
	if (tname && strcmp(tname, "Name") == 0) {
		PyObject *id = PyObject_GetAttrString(target, "id");
		if (id) {
			const char *s = PyUnicode_AsUTF8(id);
			if (s) {
				sqlite3_int64 sid;
				index_add_symbol(ctx->db, ctx->file_id, s, "variable", line, 0, NULL, &sid);
			}
			Py_DECREF(id);
		}
	} else if (tname && (strcmp(tname, "Tuple") == 0 || strcmp(tname, "List") == 0)) {
		PyObject *elts = PyObject_GetAttrString(target, "elts");
		if (elts && PyList_Check(elts)) {
			Py_ssize_t n = PyList_Size(elts);
			for (Py_ssize_t i = 0; i < n; i++) {
				extract_assign_targets(PyList_GetItem(elts, i), ctx, line);
			}
		}
		Py_XDECREF(elts);
	}
	Py_XDECREF(cname);
	Py_XDECREF(cls);
	PyErr_Clear();
}

static void on_node(const char *type, int lineno, const char *name, void *node_ptr, void *ud)
{
	Ctx *ctx = ud;
	PyObject *node = node_ptr;
	sqlite3_int64 sid;
	if (strcmp(type, "ClassDef") == 0) {
		char *bases = NULL, *methods = NULL, *docstring = NULL, *decorators = NULL;
		sqlite3_int64 sid = 0;
		if (name) index_add_symbol(ctx->db, ctx->file_id, name, "class", lineno, 0, NULL, &sid);
		PyObject *ast_mod = PyImport_ImportModule("ast");
		if (ast_mod && node) {
			/* bases */
			PyObject *bases_obj = PyObject_GetAttrString(node, "bases");
			if (bases_obj && PyList_Check(bases_obj)) {
				size_t cap = 256, len = 0;
				bases = malloc(cap);
				bases[0] = '\0';
				Py_ssize_t nb = PyList_Size(bases_obj);
				for (Py_ssize_t i = 0; i < nb; i++) {
					PyObject *b = PyList_GetItem(bases_obj, i);
					PyObject *u = NULL;
					PyObject *unparse = PyObject_GetAttrString(ast_mod, "unparse");
					if (unparse) { u = PyObject_CallFunction(unparse, "(O)", b); Py_DECREF(unparse); }
					if (!u) u = PyObject_Str(b);
					if (u) {
						const char *bs = PyUnicode_AsUTF8(u);
						if (bs) {
							size_t add = strlen(bs) + 2;
							if (len + add >= cap) { cap = (len+add)*2; bases = realloc(bases, cap); }
							if (len > 0) bases[len++] = ',';
							strcpy(bases + len, bs);
							len += strlen(bs);
						}
						Py_DECREF(u);
					}
				}
				if (bases) bases[len] = '\0';
			}
			Py_XDECREF(bases_obj);
			/* methods */
			PyObject *body = PyObject_GetAttrString(node, "body");
			if (body && PyList_Check(body)) {
				size_t cap = 256, len = 0;
				methods = malloc(cap);
				methods[0] = '\0';
				Py_ssize_t nb = PyList_Size(body);
				for (Py_ssize_t i = 0; i < nb; i++) {
					PyObject *item = PyList_GetItem(body, i);
					PyObject *icls = PyObject_GetAttrString(item, "__class__");
					PyObject *iname = icls ? PyObject_GetAttrString(icls, "__name__") : NULL;
					const char *cn = iname ? PyUnicode_AsUTF8(iname) : NULL;
					if (cn && (strcmp(cn, "FunctionDef") == 0 || strcmp(cn, "AsyncFunctionDef") == 0)) {
						PyObject *mname = PyObject_GetAttrString(item, "name");
						const char *ms = mname ? PyUnicode_AsUTF8(mname) : NULL;
						if (ms) {
							int is_async = (strcmp(cn, "AsyncFunctionDef") == 0);
							PyObject *args_obj = PyObject_GetAttrString(item, "args");
							PyObject *returns_obj = PyObject_GetAttrString(item, "returns");
							PyObject *decs_obj = PyObject_GetAttrString(item, "decorator_list");
							PyObject *unparse_fn = ast_mod ? PyObject_GetAttrString(ast_mod, "unparse") : NULL;
							PyObject *args_str = (unparse_fn && args_obj) ? PyObject_CallFunction(unparse_fn, "(O)", args_obj) : NULL;
							PyObject *ret_str = (unparse_fn && returns_obj && returns_obj != Py_None) ? PyObject_CallFunction(unparse_fn, "(O)", returns_obj) : NULL;
							const char *args_s = (args_str && PyUnicode_Check(args_str)) ? PyUnicode_AsUTF8(args_str) : "";
							const char *ret_s = (ret_str && PyUnicode_Check(ret_str)) ? PyUnicode_AsUTF8(ret_str) : NULL;

							char dec_prefix[512];
							dec_prefix[0] = '\0';
							if (decs_obj && PyList_Check(decs_obj) && unparse_fn) {
								Py_ssize_t nd = PyList_Size(decs_obj);
								size_t dlen = 0;
								for (Py_ssize_t di = 0; di < nd; di++) {
									PyObject *dec = PyList_GetItem(decs_obj, di);
									PyObject *u = PyObject_CallFunction(unparse_fn, "(O)", dec);
									const char *ds = u ? (PyUnicode_Check(u) ? PyUnicode_AsUTF8(u) : NULL) : NULL;
									if (ds) {
										if (dlen > 0 && dlen + 1 < sizeof(dec_prefix)) dec_prefix[dlen++] = ' ';
										size_t dl = strlen(ds);
										if (dlen + dl + 2 < sizeof(dec_prefix)) {
											dec_prefix[dlen++] = '@';
											memcpy(dec_prefix + dlen, ds, dl);
											dlen += dl;
											dec_prefix[dlen] = '\0';
										}
									}
									Py_XDECREF(u);
								}
							}

							char *sig = NULL;
							char prefix[600];
							snprintf(prefix, sizeof(prefix), "%s%s%s", dec_prefix, dec_prefix[0] ? " " : "", is_async ? "async " : "");
							if (ret_s) {
								if (asprintf(&sig, "%s%s(%s) -> %s", prefix, ms, args_s ? args_s : "", ret_s) == -1) sig = NULL;
							} else {
								if (asprintf(&sig, "%s%s(%s)", prefix, ms, args_s ? args_s : "") == -1) sig = NULL;
							}
							if (sig) {
								size_t add = strlen(sig) + 2;
								if (len + add >= cap) { cap = (len+add)*2; methods = realloc(methods, cap); }
								if (len > 0) methods[len++] = ',';
								strcpy(methods + len, sig);
								len += strlen(sig);
								free(sig);
							}

							/* docstring */
							char *mdoc = NULL;
							PyObject *gds = ast_mod ? PyObject_GetAttrString(ast_mod, "get_docstring") : NULL;
							if (gds) {
								PyObject *ds = PyObject_CallFunction(gds, "(O)", item);
								if (ds && ds != Py_None) {
									const char *dstr = PyUnicode_AsUTF8(ds);
									if (dstr) mdoc = strdup(dstr);
								}
								Py_XDECREF(ds);
								Py_DECREF(gds);
							}

							/* insert method row + args */
							if (name) {
								sqlite3_int64 mid = 0;
								int mline = lineno;
								int mend = 0;
								PyObject *end_lineno_obj = PyObject_GetAttrString(item, "end_lineno");
								if (end_lineno_obj) {
									mend = (int)PyLong_AsLong(end_lineno_obj);
									if (PyErr_Occurred()) PyErr_Clear();
									Py_DECREF(end_lineno_obj);
								} else {
									PyErr_Clear();
								}
								index_add_method(ctx->db, sid, ms, mline, mend, is_async, dec_prefix, ret_s, mdoc, &mid);
								if (mid > 0 && args_obj) {
									PyObject *posonly = PyObject_GetAttrString(args_obj, "posonlyargs");
									PyObject *pos = PyObject_GetAttrString(args_obj, "args");
									PyObject *vararg = PyObject_GetAttrString(args_obj, "vararg");
									PyObject *kwonly = PyObject_GetAttrString(args_obj, "kwonlyargs");
									PyObject *defaults = PyObject_GetAttrString(args_obj, "defaults");
									PyObject *kw_defaults = PyObject_GetAttrString(args_obj, "kw_defaults");
									PyObject *kwarg = PyObject_GetAttrString(args_obj, "kwarg");

									/* combined positional list */
									PyObject *combined = PyList_New(0);
									if (posonly && PyList_Check(posonly)) {
										Py_ssize_t np = PyList_Size(posonly);
										for (Py_ssize_t i = 0; i < np; i++) PyList_Append(combined, PyList_GetItem(posonly, i));
									}
									if (pos && PyList_Check(pos)) {
										Py_ssize_t np = PyList_Size(pos);
										for (Py_ssize_t i = 0; i < np; i++) PyList_Append(combined, PyList_GetItem(pos, i));
									}
									Py_ssize_t ncombined = PyList_Check(combined) ? PyList_Size(combined) : 0;
									Py_ssize_t ndefaults = (defaults && PyList_Check(defaults)) ? PyList_Size(defaults) : 0;

									for (Py_ssize_t i = 0; i < ncombined; i++) {
										PyObject *arg = PyList_GetItem(combined, i);
										PyObject *argname = arg ? PyObject_GetAttrString(arg, "arg") : NULL;
										const char *an = argname ? PyUnicode_AsUTF8(argname) : NULL;
										if (an && PyErr_Occurred()) { PyErr_Clear(); an = NULL; }
										PyObject *ann = arg ? PyObject_GetAttrString(arg, "annotation") : NULL;
										PyObject *ann_str = (unparse_fn && ann && ann != Py_None) ? PyObject_CallFunction(unparse_fn, "(O)", ann) : NULL;
										const char *ann_s = (ann_str && PyUnicode_Check(ann_str)) ? PyUnicode_AsUTF8(ann_str) : NULL;
										char *def_s = NULL;
										Py_ssize_t def_index = i - (ncombined - ndefaults);
										if (def_index >= 0 && defaults && PyList_Check(defaults)) {
											PyObject *dobj = PyList_GetItem(defaults, def_index);
											PyObject *dstr = (unparse_fn && dobj) ? PyObject_CallFunction(unparse_fn, "(O)", dobj) : NULL;
											if (dstr && PyUnicode_Check(dstr)) {
												const char *ds = PyUnicode_AsUTF8(dstr);
												if (ds) def_s = strdup(ds);
											}
											Py_XDECREF(dstr);
										}
										const char *kind = "positional";
										if (posonly && PyList_Check(posonly) && i < PyList_Size(posonly)) kind = "positional-only";
										index_add_method_arg(ctx->db, mid, an, ann_s, def_s, kind, (int)i);
										free(def_s);
										Py_XDECREF(ann_str);
										Py_XDECREF(ann);
										Py_XDECREF(argname);
									}
									Py_XDECREF(combined);

									/* vararg */
									if (vararg && vararg != Py_None) {
										PyObject *vname = PyObject_GetAttrString(vararg, "arg");
										const char *vn = vname ? PyUnicode_AsUTF8(vname) : NULL;
										if (vn && PyErr_Occurred()) { PyErr_Clear(); vn = NULL; }
										PyObject *vann = PyObject_GetAttrString(vararg, "annotation");
										PyObject *vann_str = (unparse_fn && vann && vann != Py_None) ? PyObject_CallFunction(unparse_fn, "(O)", vann) : NULL;
										const char *vann_s = (vann_str && PyUnicode_Check(vann_str)) ? PyUnicode_AsUTF8(vann_str) : NULL;
										index_add_method_arg(ctx->db, mid, vn, vann_s, NULL, "varargs", -1);
										Py_XDECREF(vann_str);
										Py_XDECREF(vann);
										Py_XDECREF(vname);
									}

									/* kwonly args */
									if (kwonly && PyList_Check(kwonly)) {
										Py_ssize_t nkw = PyList_Size(kwonly);
										for (Py_ssize_t i = 0; i < nkw; i++) {
											PyObject *arg = PyList_GetItem(kwonly, i);
											PyObject *argname = arg ? PyObject_GetAttrString(arg, "arg") : NULL;
											const char *an = argname ? PyUnicode_AsUTF8(argname) : NULL;
											if (an && PyErr_Occurred()) { PyErr_Clear(); an = NULL; }
											PyObject *ann = arg ? PyObject_GetAttrString(arg, "annotation") : NULL;
											PyObject *ann_str = (unparse_fn && ann && ann != Py_None) ? PyObject_CallFunction(unparse_fn, "(O)", ann) : NULL;
											const char *ann_s = (ann_str && PyUnicode_Check(ann_str)) ? PyUnicode_AsUTF8(ann_str) : NULL;
											char *def_s = NULL;
											if (kw_defaults && PyList_Check(kw_defaults) && i < PyList_Size(kw_defaults)) {
												PyObject *dobj = PyList_GetItem(kw_defaults, i);
												if (dobj && dobj != Py_None && unparse_fn) {
													PyObject *dstr = PyObject_CallFunction(unparse_fn, "(O)", dobj);
													if (dstr && PyUnicode_Check(dstr)) {
														const char *ds = PyUnicode_AsUTF8(dstr);
														if (ds) def_s = strdup(ds);
													}
													Py_XDECREF(dstr);
												}
											}
											index_add_method_arg(ctx->db, mid, an, ann_s, def_s, "keyword-only", (int)i);
											free(def_s);
											Py_XDECREF(ann_str);
											Py_XDECREF(ann);
											Py_XDECREF(argname);
										}
									}

									/* kwarg */
									if (kwarg && kwarg != Py_None) {
										PyObject *kname = PyObject_GetAttrString(kwarg, "arg");
										const char *kn = kname ? PyUnicode_AsUTF8(kname) : NULL;
										if (kn && PyErr_Occurred()) { PyErr_Clear(); kn = NULL; }
										PyObject *kann = PyObject_GetAttrString(kwarg, "annotation");
										PyObject *kann_str = (unparse_fn && kann && kann != Py_None) ? PyObject_CallFunction(unparse_fn, "(O)", kann) : NULL;
										const char *kann_s = (kann_str && PyUnicode_Check(kann_str)) ? PyUnicode_AsUTF8(kann_str) : NULL;
										index_add_method_arg(ctx->db, mid, kn, kann_s, NULL, "kwargs", -1);
										Py_XDECREF(kann_str);
										Py_XDECREF(kann);
										Py_XDECREF(kname);
									}

									Py_XDECREF(kwarg);
									Py_XDECREF(kw_defaults);
									Py_XDECREF(defaults);
									Py_XDECREF(kwonly);
									Py_XDECREF(vararg);
									Py_XDECREF(pos);
									Py_XDECREF(posonly);
								}
								free(mdoc);
							}

							Py_XDECREF(ret_str);
							Py_XDECREF(args_str);
							Py_XDECREF(unparse_fn);
							Py_XDECREF(returns_obj);
							Py_XDECREF(args_obj);
							Py_XDECREF(decs_obj);
						}
						Py_XDECREF(mname);
					}
					Py_XDECREF(iname);
					Py_XDECREF(icls);
				}
				if (methods) methods[len] = '\0';
			}
			Py_XDECREF(body);
			/* docstring */
			PyObject *gds = PyObject_GetAttrString(ast_mod, "get_docstring");
			if (gds) {
				PyObject *ds = PyObject_CallFunction(gds, "(O)", node);
				if (ds && ds != Py_None) {
					const char *dstr = PyUnicode_AsUTF8(ds);
					if (dstr) docstring = strdup(dstr);
				}
				Py_XDECREF(ds);
				Py_DECREF(gds);
			}
			/* decorators */
			PyObject *decs = PyObject_GetAttrString(node, "decorator_list");
			if (decs && PyList_Check(decs)) {
				size_t cap = 256, len = 0;
				decorators = malloc(cap);
				decorators[0] = '\0';
				Py_ssize_t nd = PyList_Size(decs);
				for (Py_ssize_t i = 0; i < nd; i++) {
					PyObject *dec = PyList_GetItem(decs, i);
					PyObject *unparse = PyObject_GetAttrString(ast_mod, "unparse");
					PyObject *u = NULL;
					if (unparse) { u = PyObject_CallFunction(unparse, "(O)", dec); Py_DECREF(unparse); }
					if (!u) u = PyObject_Str(dec);
					if (u) {
						const char *ds = PyUnicode_AsUTF8(u);
						if (ds) {
							size_t add = strlen(ds) + 2;
							if (len + add >= cap) { cap = (len+add)*2; decorators = realloc(decorators, cap); }
							if (len > 0) decorators[len++] = ',';
							strcpy(decorators + len, ds);
							len += strlen(ds);
						}
						Py_DECREF(u);
					}
				}
				if (decorators) decorators[len] = '\0';
			}
			Py_XDECREF(decs);
		}
		if (ast_mod) Py_DECREF(ast_mod);
		int end_line = 0;
		PyObject *end_lineno_obj = PyObject_GetAttrString(node, "end_lineno");
		if (end_lineno_obj) {
			end_line = (int)PyLong_AsLong(end_lineno_obj);
			if (PyErr_Occurred()) PyErr_Clear();
			Py_DECREF(end_lineno_obj);
		} else {
			PyErr_Clear();
		}
		index_add_class_detail(ctx->db, sid, bases, methods, docstring, decorators, end_line);
		free(bases); free(methods); free(docstring); free(decorators);
	} else if (strcmp(type, "FunctionDef") == 0 || strcmp(type, "AsyncFunctionDef") == 0) {
		if (name) index_add_symbol(ctx->db, ctx->file_id, name, "function", lineno, 0, NULL, &sid);
	} else if (strcmp(type, "Assign") == 0) {
		PyObject *targets = PyObject_GetAttrString(node, "targets");
		if (targets && PyList_Check(targets)) {
			Py_ssize_t n = PyList_Size(targets);
			for (Py_ssize_t i = 0; i < n; i++) {
				extract_assign_targets(PyList_GetItem(targets, i), ctx, lineno);
			}
		}
		Py_XDECREF(targets);
	} else if (strcmp(type, "Name") == 0 && name) {
		/* Record uses of class names as references. */
		const char *sql = "SELECT id FROM symbols WHERE name=?1 AND kind='class';";
		sqlite3_stmt *stmt;
		if (sqlite3_prepare_v2(ctx->db, sql, -1, &stmt, NULL) == SQLITE_OK) {
			sqlite3_bind_text(stmt, 1, name, -1, SQLITE_TRANSIENT);
			while (sqlite3_step(stmt) == SQLITE_ROW) {
				sqlite3_int64 sid = sqlite3_column_int64(stmt, 0);
				index_add_ref(ctx->db, sid, ctx->file_id, lineno, 0);
			}
			sqlite3_finalize(stmt);
		}
	}
	PyErr_Clear();
}

static unsigned long fnv1a_file(const char *path)
{
	FILE *f = fopen(path, "rb");
	if (!f) return 0;
	unsigned long h = 2166136261UL;
	char buf[65536];
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
		for (size_t i = 0; i < n; i++) {
			h ^= (unsigned char)buf[i];
			h *= 16777619UL;
		}
	}
	fclose(f);
	return h;
}

static char *read_file(const char *path)
{
	FILE *f = fopen(path, "rb");
	if (!f) return NULL;
	fseek(f, 0, SEEK_END);
	long sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (sz < 0) { fclose(f); return NULL; }
	char *buf = malloc(sz + 1);
	if (!buf) { fclose(f); return NULL; }
	size_t got = fread(buf, 1, sz, f);
	buf[got] = '\0';
	fclose(f);
	return buf;
}

static int is_python_file(const char *path)
{
	size_t n = strlen(path);
	return n >= 3 && strcmp(path + n - 3, ".py") == 0;
}

static void reindex_file(const char *path, sqlite3 *db)
{
	if (!is_python_file(path)) {
		/* keep the index limited to Python sources */
		index_remove_file(db, path);
		return;
	}
	if (pycool_root && ignore_is_ignored(pycool_ignore, pycool_root, path)) {
		/* best effort: remove from index */
		index_remove_file(db, path);
		return;
	}
	struct stat st;
	if (stat(path, &st) != 0) return;
	if (!S_ISREG(st.st_mode)) return;

	char hash[32];
	snprintf(hash, sizeof(hash), "%lu", fnv1a_file(path));

	if (index_file_changed(db, path, st.st_mtime, hash) == 0) return;

	index_begin(db);
	index_remove_file(db, path);

	sqlite3_int64 file_id;
	if (index_upsert_file(db, path, st.st_mtime, hash, &file_id) != 0) {
		index_rollback(db);
		return;
	}

	char *src = read_file(path);
	if (src) {
		Ast *a = ast_parse(src, path);
		if (a) {
			Ctx ctx = { db, file_id };
			AstSubscriber sub;
			sub.node_types = symbol_types;
			sub.on_node = on_node;
			sub.userdata = &ctx;
			sub.next = NULL;
			ast_traverse(a, NULL, NULL, &sub);
			ast_free(a);
		}
		free(src);
	}
	index_commit(db);
}

static void scan_dir(const char *root, sqlite3 *db)
{
	DIR *d = opendir(root);
	if (!d) {
		if (errno != ENOENT) fprintf(stderr, "opendir %s: %s\n", root, strerror(errno));
		return;
	}
	struct dirent *ent;
	while ((ent = readdir(d)) != NULL) {
		if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
		size_t plen = strlen(root) + strlen(ent->d_name) + 2;
		char *path = malloc(plen);
		if (!path) continue;
		snprintf(path, plen, "%s/%s", root, ent->d_name);
		if (ignore_is_ignored(pycool_ignore, root, path)) { free(path); continue; }
		struct stat st;
		if (lstat(path, &st) != 0) { free(path); continue; }
		if (S_ISDIR(st.st_mode)) {
			scan_dir(path, db);
			free(path);
		} else if (S_ISREG(st.st_mode)) {
			reindex_file(path, db);
			free(path);
		} else {
			free(path);
		}
	}
	closedir(d);
}

static void on_event(const char *path, uint32_t mask, void *userdata)
{
	sqlite3 *db = userdata;
	if (mask & IN_Q_OVERFLOW) {
		if (pycool_root) scan_dir(pycool_root, db);
		return;
	}
	if (pycool_root && (mask & (IN_DELETE_SELF | IN_IGNORED)) && strcmp(path, pycool_root) == 0) {
		pycool_running = 0;
		observer_stop();
		return;
	}
	if (pycool_root && ignore_is_ignored(pycool_ignore, pycool_root, path)) {
		if (!(mask & (IN_DELETE | IN_MOVED_FROM))) index_remove_file(db, path);
		return;
	}
	struct stat st;
	if (mask & (IN_DELETE | IN_MOVED_FROM)) {
		if (mask & IN_ISDIR) index_remove_dir(db, path);
		else index_remove_file(db, path);
		return;
	}
	if (stat(path, &st) != 0) return;
	if (S_ISDIR(st.st_mode)) {
		scan_dir(path, db);
	} else if (S_ISREG(st.st_mode)) {
		reindex_file(path, db);
	}
}

static void handle_signal(int sig)
{
	if (sig == SIGUSR1) {
		pycool_clean_requested = 1;
	} else if (sig == SIGUSR2) {
		pycool_reindex_requested = 1;
	} else {
		pycool_running = 0;
	}
	observer_stop();
}

static void clean_index(sqlite3 *db)
{
	index_begin(db);
	sqlite3_exec(db, "DELETE FROM types;", NULL, NULL, NULL);
	sqlite3_exec(db, "DELETE FROM refs;", NULL, NULL, NULL);
	sqlite3_exec(db, "DELETE FROM symbols;", NULL, NULL, NULL);
	sqlite3_exec(db, "DELETE FROM files;", NULL, NULL, NULL);
	index_commit(db);
}

int pycool_run(const char *root, const char *dbpath)
{
	char *root_abs = realpath(root, NULL);
	if (!root_abs) root_abs = strdup(root);
	if (!root_abs) return -1;
	if (registry_overlap(root_abs)) {
		fprintf(stderr, "error: %s is already monitored (or overlaps a monitored directory)\n", root_abs);
		free(root_abs);
		return -1;
	}
	signal(SIGTERM, handle_signal);
	signal(SIGINT, handle_signal);
	signal(SIGUSR1, handle_signal);
	signal(SIGUSR2, handle_signal);

	sqlite3 *db;
	if (index_init(dbpath, &db) != 0) {
		fprintf(stderr, "cannot open db %s\n", dbpath);
		free(root_abs);
		return -1;
	}
	pycool_root = root_abs;
	pycool_ignore = ignore_load(root_abs);
	registry_register(root_abs, dbpath);
	scan_dir(root_abs, db);
	while (pycool_running) {
		observer_watch(root_abs, on_event, db);
		if (pycool_clean_requested) {
			clean_index(db);
			pycool_clean_requested = 0;
		}
		if (pycool_reindex_requested) {
			clean_index(db);
			scan_dir(root_abs, db);
			pycool_reindex_requested = 0;
		}
	}
	index_close(db);
	registry_unregister();
	ignore_free(pycool_ignore);
	pycool_ignore = NULL;
	pycool_root = NULL;
	free(root_abs);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: %s ROOT [DBPATH]\n", argv[0]);
		return 2;
	}
	const char *root = argv[1];
	char *dbpath = NULL;
	if (argc >= 3) {
		dbpath = strdup(argv[2]);
		if (!dbpath) {
			perror("strdup");
			return 2;
		}
	} else {
		char *dir = NULL;
		if (asprintf(&dir, "%s/.pycool", root) == -1 || !dir) {
			perror("asprintf");
			return 2;
		}
		mkdir(dir, 0755);
		if (asprintf(&dbpath, "%s/index.db", dir) == -1 || !dbpath) {
			free(dir);
			perror("asprintf");
			return 2;
		}
		free(dir);
	}
	int rc = pycool_run(root, dbpath);
	free(dbpath);
	return rc;
}
