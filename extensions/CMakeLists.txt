cmake_minimum_required(VERSION 3.29)

# znet extensions.
#
# An extension is an optional, self-contained add-on that bridges znet to a
# third-party library. It lives entirely in this tree, links znet as a
# consumer would, and adds nothing to the core target -- so a build that never
# enables one is byte-identical to a build from before it existed.
#
# House rules for adding one:
#   * header-only INTERFACE target where possible, named znet-<thing>
#   * public headers under include/znet/ext/<thing>/
#   * everything in namespace znet::ext
#   * skip yourself with a message(STATUS ...) if your dependency is absent;
#     a missing optional dependency must never fail the parent build
#   * tests under tests/, wired only when the parent has already provided gtest

# Extensions may fetch their dependency when it is not installed. Off in an
# environment that must build without network access; the extension then skips
# itself rather than failing.
option(ZNET_EXT_ALLOW_FETCH "Let extensions download a missing dependency with FetchContent" ON)

# Stops a fetched dependency failing this build with its own warnings.
#
# Several turn warnings into errors on themselves: Box2D and FlatBuffers both
# do. Which warnings fire depends on their code and the compiler version,
# neither of which znet controls, so a newer GCC finding a maybe-uninitialized
# in Box2D's solver becomes a znet build failure with no znet involvement.
# Nothing leaks from znet into them; this is their own strictness, and it
# reproduces building them standalone.
#
# Two mechanisms have to be undone, which is why appending -Wno-error is not
# enough on its own:
#
#   COMPILE_WARNING_AS_ERROR   a target property CMake honours by adding
#                              -Werror *after* every compile option, so nothing
#                              appended can outrank it
#   -Werror in COMPILE_OPTIONS a literal flag, removed rather than overridden
#                              so ordering stops mattering
#
# Marking the fetch SYSTEM does not help either: that governs warnings from
# their headers, not from compiling their sources. znet's own -Werror is
# untouched.
function(znet_ext_relax_dependency_warnings)
    foreach(target IN LISTS ARGN)
        if(NOT TARGET ${target})
            continue()
        endif()
        get_target_property(type ${target} TYPE)
        if(type STREQUAL "INTERFACE_LIBRARY")
            continue()
        endif()

        set_target_properties(${target} PROPERTIES COMPILE_WARNING_AS_ERROR OFF)

        get_target_property(options ${target} COMPILE_OPTIONS)
        if(options)
            list(REMOVE_ITEM options -Werror /WX)
            set_target_properties(${target} PROPERTIES COMPILE_OPTIONS "${options}")
        endif()
    endforeach()
endfunction()

# Dependency-free extensions first, so a later one may build on them.
add_subdirectory(quantize)
add_subdirectory(reflect)
add_subdirectory(bitpack)
add_subdirectory(delta)

add_subdirectory(box2d)
add_subdirectory(bullet)
add_subdirectory(entt)
add_subdirectory(flatbuffers)
add_subdirectory(glm)
add_subdirectory(jolt)
add_subdirectory(json)
add_subdirectory(spdlog)
