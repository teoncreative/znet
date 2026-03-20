# znet

znet is a modern C++20 networking library that provides seamless packet serialization, TLS encryption, and cross-platform support. It's designed to be simpler and more approachable than low-level libraries like asio or libuv.

## Features

- ✅ **Simple API** – Clean, event-driven design.
- 🔒 **TLS Encryption** – Secure communication out of the box.
- ⚡ **Async Connect** – Non-blocking connections.
- 📦 **Built-in Packet Serialization** – Define your own packets easily.
- 🛠 **Cross-Platform** – Windows, Linux, macOS.

## Installation

### Using as a Git Submodule

1. **Add znet to your project:**

```bash
git submodule add https://github.com/irrld/znet.git external/znet
git submodule update --init --recursive
```

2. **Link znet in your `CMakeLists.txt`:**

Example using the bundled `zstd` inside znet

```cmake
# Example using the bundled zstd inside znet
add_subdirectory(external/znet/vendor/zstd/build/cmake ${CMAKE_CURRENT_BINARY_DIR}/zstd)
add_subdirectory(external/znet/znet ${CMAKE_CURRENT_BINARY_DIR}/znet)
target_link_libraries(your_target PRIVATE znet)
```

Example using your own `zstd` submodules

```cmake
# Example using your own submodules
add_subdirectory(external/zstd/build/cmake)
add_subdirectory(external/znet/znet)
target_link_libraries(your_target PRIVATE znet)
```

Example using system-installed `zstd` (e.g. vcpkg, brew, etc.)

```cmake
# Example using system-installed zstd (e.g. vcpkg, brew, etc.)
set(ZNET_USE_EXTERNAL_ZSTD ON)
add_subdirectory(external/znet/znet)
target_link_libraries(your_target PRIVATE znet)
```

3. **Requirements:**

* **C++20** (GCC 10+, Clang 13+, MSVC 19.29+)
* **CMake 3.29+**
* **OpenSSL** (required)
  * Install via package manager (e.g. `libssl-dev` on Linux, `vcpkg` on Windows, `brew` on macOS)
  * znet will automatically detect and link OpenSSL if it's installed

## Quick Example

Below is a minimal overview of how to use znet.

**Server:**
```cpp
ServerConfig config{"127.0.0.1", 25000};
Server server{config};
server.SetEventCallback(...);
server.Bind();
server.Listen(); // Async listen
````

**Client:**

```cpp
ClientConfig config{"127.0.0.1", 25000};
Client client{config};
client.SetEventCallback(...);
client.Bind();
client.Connect(); // Async connect
```

**Packets:**
Implement `Packet` and `PacketSerializer` to define your messages.

See the [examples](examples) folder for full working code.

## Documentation

More details:

* **Usage guides**
* **TLS configuration**
* **Serialization**

👉 [Read the Wiki](https://github.com/irrld/znet/wiki)

## Contributions

We welcome and encourage community contributions to improve znet. If you find any bugs, have feature requests, or want to contribute in any other way, feel free to open an issue or submit a pull request.

## License

Apache License 2.0 – see [LICENSE](LICENSE) for details.
