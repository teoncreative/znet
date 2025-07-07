# znet

znet is a modern C++20 networking library that provides seamless packet serialization, TLS encryption, and cross-platform support. It's designed to be simpler and more approachable than low-level libraries like asio or libuv.

## Features

- ✅ **Simple API** – Clean, event-driven design.
- 🔒 **TLS Encryption** – Secure communication out of the box.
- ⚡ **Async Connect** – Non-blocking connections.
- 📦 **Built-in Packet Serialization** – Define your own packets easily.
- 🛠 **Cross-Platform** – Windows, Linux, macOS.

## Installation

TODO

## Quick Example

Below is a minimal overview of how to use znet.

**Server:**
```cpp
ServerConfig config{"127.0.0.1", 25000};
Server server{config};
server.SetEventCallback(...);
server.Bind();
server.Listen();
server.Wait(); // Blocks main thread
````

**Client:**

```cpp
ClientConfig config{"127.0.0.1", 25000};
Client client{config};
client.SetEventCallback(...);
client.Bind();
client.Connect(); // Async connect
client.Wait();
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
