# Keep warning policy local to test fixtures, without exporting it from the library target.
if (MSVC)
    add_compile_options(/WX)
else ()
    add_compile_options(-Wall -Wunused-parameter -Werror)
endif ()
