#ifndef PYCOOLS_INDEX_H
#define PYCOOLS_INDEX_H

#include <sqlite3.h>

/* Open (or create) the index database and initialize schema. Returns 0 on success. */
int index_open(const char *path, sqlite3 **db);

/* Alias for index_open (initial load). */
int index_init(const char *path, sqlite3 **db);

/* Remove a file and all its symbols/refs/types from the index. */
int index_remove_file(sqlite3 *db, const char *path);

/* Remove a directory and all files under it. */
int index_remove_dir(sqlite3 *db, const char *dir);

/* Close the database. */
void index_close(sqlite3 *db);

/* Insert or update a file row. Returns 0 on success. */
int index_upsert_file(sqlite3 *db, const char *path, long mtime, const char *hash, sqlite3_int64 *file_id);

/* Insert a symbol row. detail may be NULL (JSON string). Returns 0 on success. */
int index_add_symbol(sqlite3 *db, sqlite3_int64 file_id, const char *name, const char *kind, int line, int col, const char *detail, sqlite3_int64 *symbol_id);

/* Insert a reference row. Returns 0 on success. */
int index_add_ref(sqlite3 *db, sqlite3_int64 symbol_id, sqlite3_int64 file_id, int line, int col);

/* Insert a type row. Returns 0 on success. */
int index_add_type(sqlite3 *db, sqlite3_int64 symbol_id, const char *type_name);

/* Insert class details. Returns 0 on success. */
int index_add_class_detail(sqlite3 *db, sqlite3_int64 symbol_id, const char *bases, const char *methods, const char *docstring, const char *decorators);

/* Transaction helpers. */
int index_begin(sqlite3 *db);
int index_commit(sqlite3 *db);
int index_rollback(sqlite3 *db);

/* Return 1 if file is new or changed (mtime/hash differ), 0 if unchanged, -1 on error. */
int index_file_changed(sqlite3 *db, const char *path, long mtime, const char *hash);

#endif /* PYCOOLS_INDEX_H */
