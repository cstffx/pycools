#include <stdio.h>
#include "ast.h"
#include "traverse.h"
#include "subscriber/class.h"

int main(void)
{
	const char *src = "class Foo:\n    pass\nclass Bar(Foo, object):\n    pass\n";
	Ast *a = ast_parse(src, "t.py");
	AstSubscriber *subs = class_subscriber();
	ast_traverse(a, NULL, NULL, subs);
	ast_free(a);
	return 0;
}
