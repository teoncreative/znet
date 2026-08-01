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
