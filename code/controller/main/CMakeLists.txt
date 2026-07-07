execute_process(
  COMMAND git rev-parse HEAD
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  OUTPUT_VARIABLE BUILD_GIT_COMMIT
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_QUIET
)
if(NOT BUILD_GIT_COMMIT)
  set(BUILD_GIT_COMMIT "unknown")
endif()

execute_process(
  COMMAND git rev-parse --abbrev-ref HEAD
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  OUTPUT_VARIABLE BUILD_GIT_BRANCH
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_QUIET
)
if(NOT BUILD_GIT_BRANCH)
  set(BUILD_GIT_BRANCH "unknown")
endif()

execute_process(
  COMMAND git status --porcelain --untracked-files=no
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  OUTPUT_VARIABLE BUILD_GIT_DIRTY_OUTPUT
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_QUIET
)
if(BUILD_GIT_DIRTY_OUTPUT)
  set(BUILD_GIT_DIRTY "1")
else()
  set(BUILD_GIT_DIRTY "0")
endif()

idf_component_register(SRCS "main.c" "services/log_store.c"
                      INCLUDE_DIRS "."
                      EMBED_FILES "public/favicon.ico" "public/index.html" "public/script.js" "public/style.css")

target_compile_definitions(${COMPONENT_LIB} PRIVATE
  BUILD_GIT_COMMIT="${BUILD_GIT_COMMIT}"
  BUILD_GIT_BRANCH="${BUILD_GIT_BRANCH}"
  BUILD_GIT_DIRTY=${BUILD_GIT_DIRTY}
)
