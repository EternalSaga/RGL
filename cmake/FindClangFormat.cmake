find_program(CLANG_FORMAT_EXECUTABLE
             NAMES clang-format
                   clang-format-7
                   clang-format-6.0
                   clang-format-5.0
                   clang-format-4.0
                   clang-format-3.9
                   clang-format-3.8
                   clang-format-3.7
                   clang-format-3.6
                   clang-format-3.5
                   clang-format-3.4
                   clang-format-3.3
             DOC "clang-format executable")
mark_as_advanced(CLANG_FORMAT_EXECUTABLE)

# Extract version from command "clang-format -version"
if(CLANG_FORMAT_EXECUTABLE)
  execute_process(COMMAND ${CLANG_FORMAT_EXECUTABLE} -version
                  OUTPUT_VARIABLE clang_format_version
                  ERROR_QUIET OUTPUT_STRIP_TRAILING_WHITESPACE)

  if(clang_format_version MATCHES "^clang-format version .*")
    # clang_format_version sample: "clang-format version 3.9.1-4ubuntu3~16.04.1
    # (tags/RELEASE_391/rc2)"
    string(REGEX
           REPLACE "clang-format version ([.0-9]+).*"
                   "\\1"
                   CLANG_FORMAT_VERSION
                   "${clang_format_version}")
    # CLANG_FORMAT_VERSION sample: "3.9.1"

    # Extract version components
    string(REPLACE "."
                   ";"
                   clang_format_version
                   "${CLANG_FORMAT_VERSION}")
    list(LENGTH clang_format_version CLANG_FORMAT_VERSION_COUNT)
    if(CLANG_FORMAT_VERSION_COUNT GREATER 0)
      list(GET clang_format_version 0 CLANG_FORMAT_VERSION_MAJOR)
    else()
      set(CLANG_FORMAT_VERSION_MAJOR 0)
    endif()
    if(CLANG_FORMAT_VERSION_COUNT GREATER 1)
      list(GET clang_format_version 1 CLANG_FORMAT_VERSION_MINOR)
    else()
      set(CLANG_FORMAT_VERSION_MINOR 0)
    endif()
    if(CLANG_FORMAT_VERSION_COUNT GREATER 2)
      list(GET clang_format_version 2 CLANG_FORMAT_VERSION_PATCH)
    else()
      set(CLANG_FORMAT_VERSION_PATCH 0)
    endif()
  endif()
  unset(clang_format_version)
endif()

if(CLANG_FORMAT_EXECUTABLE)
  set(CLANG_FORMAT_FOUND TRUE)
else()
  set(CLANG_FORMAT_FOUND FALSE)
endif()


function(execute_format)
    set(srcs ${ARGV0})
    foreach(src ${srcs})
        #如果src是一个cpp文件或者hpp文件
        if(src MATCHES "\\.cpp$" OR src MATCHES "\\.hpp$")
            execute_process(COMMAND ${CLANG_FORMAT_EXECUTABLE} -i ${src})
        endif()
    endforeach(src ${srcs})
endfunction()
