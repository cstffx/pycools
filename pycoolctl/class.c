#define _GNU_SOURCE
#include "class.h"
#include "../registry.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sqlite3.h>

static const char *field_name(char c)
{
	switch (c) {
	case 'A': return "name";
	case 'B': return "file";
	case 'C': return "line";
	case 'D': return "bases";
	case 'E': return "methods";
	case 'F': return "fields";
	case 'G': return "docstring";
	case 'H': return "types";
	case 'I': return "refs";
	case 'J': return "decorators";
	case 'K': return "module";
	default: return NULL;
	}
}

static void print_json_str(const char *s)
{
	putchar('"');
	if (s) {
		for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
			switch (*p) {
				case '"': fputs("\\\"", stdout); break;
				case '\\': fputs("\\\\", stdout); break;
				case '\n': fputs("\\n", stdout); break;
				case '\r': fputs("\\r", stdout); break;
				case '\t': fputs("\\t", stdout); break;
				default:
					if (*p < 0x20) printf("\\u%04x", *p);
					else putchar(*p);
			}
		}
	}
	putchar('"');
}

int class_command(int argc, char **argv)
{
	const char *name = NULL;
	const char *info = NULL;
	const char *dbpath = NULL;
	const char *file = NULL;
	int json_mode = 0;
	int compact_mode = 0;
	int full_mode = 0;
	for (int i = 2; i < argc; i++) {
		if (strcmp(argv[i], "--info") == 0 && i + 1 < argc) {
			info = argv[++i];
		} else if (strcmp(argv[i], "--db") == 0 && i + 1 < argc) {
			dbpath = argv[++i];
		} else if ((strcmp(argv[i], "--file") == 0 || strcmp(argv[i], "--module") == 0) && i + 1 < argc) {
			file = argv[++i];
		} else if (strcmp(argv[i], "--json") == 0) {
			json_mode = 1;
		} else if (strcmp(argv[i], "--compact") == 0) {
			compact_mode = 1;
		} else if (strcmp(argv[i], "--full") == 0 || strcmp(argv[i], "--verbose") == 0) {
			full_mode = 1;
		} else if (!name) {
			name = argv[i];
		}
	}
	if (!json_mode && info == NULL && !full_mode && !compact_mode) {
		compact_mode = 1;
	}
	if (!dbpath) {
		char **roots, **dbpaths; pid_t *pids; int n;
		if (registry_list_paths(&roots, &dbpaths, &pids, &n) != 0 || n == 0) {
			fprintf(stderr, "no running instance; pass --db PATH\n");
			return 1;
		}
		dbpath = dbpaths[0];
	}
	sqlite3 *db;
	if (sqlite3_open(dbpath, &db) != SQLITE_OK) {
		fprintf(stderr, "cannot open %s\n", dbpath);
		return 1;
	}
	char sql[2048];
	if (name) {
		if (file) {
			snprintf(sql, sizeof(sql),
			         "SELECT s.name, f.path, s.line, cd.bases, cd.methods, cd.docstring, cd.decorators "
			         "FROM symbols s JOIN files f ON s.file_id=f.id "
			         "LEFT JOIN class_details cd ON cd.symbol_id=s.id "
			         "WHERE s.kind='class' AND s.name=? AND f.path=?;");
		} else {
			snprintf(sql, sizeof(sql),
			         "SELECT s.name, f.path, s.line, cd.bases, cd.methods, cd.docstring, cd.decorators "
			         "FROM symbols s JOIN files f ON s.file_id=f.id "
			         "LEFT JOIN class_details cd ON cd.symbol_id=s.id "
			         "WHERE s.kind='class' AND s.name=?;");
		}
	} else {
		if (file) {
			snprintf(sql, sizeof(sql),
			         "SELECT s.name, f.path, s.line, cd.bases, cd.methods, cd.docstring, cd.decorators "
			         "FROM symbols s JOIN files f ON s.file_id=f.id "
			         "LEFT JOIN class_details cd ON cd.symbol_id=s.id "
			         "WHERE s.kind='class' AND f.path=?;");
		} else {
			snprintf(sql, sizeof(sql),
			         "SELECT s.name, f.path, s.line, cd.bases, cd.methods, cd.docstring, cd.decorators "
			         "FROM symbols s JOIN files f ON s.file_id=f.id "
			         "LEFT JOIN class_details cd ON cd.symbol_id=s.id "
			         "WHERE s.kind='class';");
		}
	}
	sqlite3_stmt *stmt;
	if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
		fprintf(stderr, "query error: %s\n", sqlite3_errmsg(db));
		sqlite3_close(db);
		return 1;
	}
	int bind_idx = 1;
	if (name) sqlite3_bind_text(stmt, bind_idx++, name, -1, SQLITE_TRANSIENT);
	if (file) sqlite3_bind_text(stmt, bind_idx++, file, -1, SQLITE_TRANSIENT);

	int all = (info == NULL);
	int had_rows = 0;
	int json_first = 1;
	if (json_mode) printf("[\n");
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		const char *n = (const char *)sqlite3_column_text(stmt, 0);
		const char *path = (const char *)sqlite3_column_text(stmt, 1);
		long line = sqlite3_column_int64(stmt, 2);
		const char *bases = (const char *)sqlite3_column_text(stmt, 3);
		const char *methods = (const char *)sqlite3_column_text(stmt, 4);
		const char *doc = (const char *)sqlite3_column_text(stmt, 5);
		const char *decs = (const char *)sqlite3_column_text(stmt, 6);
		had_rows = 1;

		if (json_mode) {
			if (!json_first) printf(",\n");
			json_first = 0;
			printf("  {\"name\":");
			print_json_str(n);
			printf(",\"file\":");
			print_json_str(path);
			printf(",\"line\":%ld", line);
			printf(",\"bases\":");
			print_json_str(bases);
			printf(",\"methods\":");
			print_json_str(methods);
			printf(",\"docstring\":");
			print_json_str(doc);
			printf(",\"decorators\":");
			print_json_str(decs);
			printf("}");
		} else if (compact_mode) {
			printf("%s %s:%ld", n ? n : "", path ? path : "", line);
			if (bases && bases[0]) printf(" bases=%s", bases);
			if (methods && methods[0]) printf(" methods=%s", methods);
			if (doc && doc[0]) printf(" docstring=%s", doc);
			if (decs && decs[0]) printf(" decorators=%s", decs);
			printf("\n");
		} else {
			/* print selected or all */
			int first = 1;
			const char *letters = all ? "ABCDEFGIJK" : info;
			for (const char *p = letters; *p; p++) {
				const char *fname = field_name(*p);
				if (!fname) continue;
				if (!first) printf(" | ");
				first = 0;
				if (*p == 'A') printf("name=%s", n ? n : "");
				else if (*p == 'B') printf("file=%s", path ? path : "");
				else if (*p == 'C') printf("line=%ld", line);
				else if (*p == 'D') printf("bases=%s", bases ? bases : "");
				else if (*p == 'E') printf("methods=%s", methods ? methods : "");
				else if (*p == 'F') printf("fields=n/a");
				else if (*p == 'G') printf("docstring=%s", doc ? doc : "");
				else if (*p == 'H') printf("types=n/a");
				else if (*p == 'I') printf("refs=n/a");
				else if (*p == 'J') printf("decorators=%s", decs ? decs : "");
				else if (*p == 'K') printf("module=%s", path ? path : "");
			}
			printf("\n");
		}
	}
	if (json_mode) {
		if (had_rows) printf("\n]\n");
		else printf("[]\n");
	}
	sqlite3_finalize(stmt);
	sqlite3_close(db);
	return 0;
}
