#include "Halide.h"
#include <stdio.h>

#include "test/common/halide_test_dirs.h"

using namespace Halide;

int main(int argc, char **argv) {
    Param<float> p("myParam");
    Func f, g, h, j;
    Var x, y;
    f(x, y) = x + y;
    g(x, y) = cast<float>(f(x, y) + f(x + 1, y)) * p;
    h(x, y) = f(x, y) + g(x, y);
    j(x, y) = h(x, y) * 2;

    f.compute_root();
    g.compute_root();
    h.compute_root();

    {
        std::string result_file = Internal::get_test_tmp_dir() + "compile_to_lowered_stmt.stmt";

        Internal::ensure_no_file_exists(result_file);

        j.compile_to_lowered_stmt(result_file, j.infer_arguments());

        Internal::assert_file_exists(result_file);
    }

    printf("Success!\n");
    return 0;
}
