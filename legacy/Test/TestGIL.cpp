#include <duk_config.h>
#include <dukglue/register_function.h>
#include <duktape.h>

#include <dukglue/dukglue.h>
#include <ostream>
#include <iostream>

void
native_print(std::string str)
{
    std::cout << str << std::endl;
}
int
native_multiply(int a, int b)
{
    return a * b;
}
void
setNativeFunctions(duk_context *ctx)
{
    dukglue_register_function(ctx, &native_print, "print");
    dukglue_register_function(ctx, &native_multiply, "multiply");
}
int
main(int argc, char **argv)
{
    duk_context *ctx = duk_create_heap_default();

    duk_eval_string(ctx, R"(
	
		function add(a, b) {
			return a + b;
		}
		const a = 2;
		const b = 3;
		add(b, a);
	)");
    printf("2+3=%d\n", (int)duk_get_int(ctx, -1));

    setNativeFunctions(ctx);

    duk_eval_string_noresult(ctx, R"(
		var c = multiply(2, 3);
		print("2*3="+ c);
	)");

    duk_destroy_heap(ctx);

    return 0;
}
